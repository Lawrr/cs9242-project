/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <sel4/sel4.h>
#include <elf/elf.h>
#include <string.h>
#include <assert.h>
#include <cspace/cspace.h>
#include <setjmp.h>

#include "elf.h"
#include "addrspace.h"
#include "frametable.h"

#include <vmem_layout.h>
#include <ut_manager/ut.h>
#include <mapping.h>

#define verbose 0
#include <sys/debug.h>
#include <sys/panic.h>

/* Minimum of two values. */
#define MIN(a,b) (((a)<(b))?(a):(b))

#define PAGESIZE              (1 << (seL4_PageBits))
#define PAGEMASK              ((PAGESIZE) - 1)
#define PAGE_ALIGN(addr)      ((addr) & ~(PAGEMASK))
#define IS_PAGESIZE_ALIGNED(addr) !((addr) &  (PAGEMASK))

#define LOWER_BITS_SHIFT 20

/* To differencient between async and and sync IPC, we assign a
 * badge to the async endpoint. The badge that we receive will
 * be the bitwise 'OR' of the async endpoint badge and the badges
 * of all pending notifications. */
#define IRQ_EP_BADGE         (1 << (seL4_BadgeBits - 1))
/* All badged IRQs set high bet, then we use uniq bits to
 * distinguish interrupt sources */
#define IRQ_BADGE_NETWORK (1 << 0)
#define IRQ_BADGE_TIMER (1 << 1)

extern seL4_ARM_PageDirectory dest_as;

extern jmp_buf syscall_loop_entry;
extern seL4_CPtr _sos_ipc_ep_cap;

jmp_buf mapping_entry;

/*
 * Convert ELF permissions into seL4 permissions.
 */
static inline seL4_Word get_sel4_rights_from_elf(unsigned long permissions) {
    seL4_Word result = 0;

    if (permissions & PF_R)
        result |= seL4_CanRead;
    if (permissions & PF_X)
        result |= seL4_CanRead;
    if (permissions & PF_W)
        result |= seL4_CanWrite;

    return result;
}

void mapping_loop(seL4_CPtr ep, int exit_after_setjmp) {
    seL4_Word badge;
    seL4_Word label;
    seL4_MessageInfo_t message;
    while (1) {
        int err = setjmp(syscall_loop_entry);
        if (exit_after_setjmp && err == 0) {
            longjmp(mapping_entry, 1);
        }
        exit_after_setjmp = 0;
        message = seL4_Wait(ep, &badge);
        label = seL4_MessageInfo_get_label(message);
        if (badge & IRQ_EP_BADGE) {
            /* Interrupt */
            if (badge & IRQ_BADGE_NETWORK) {
                network_irq();
            }
            if (badge & IRQ_BADGE_TIMER) {
                timer_interrupt();
            }

        } else {
            printf("Rootserver got an unknown message\n");
        }

        seL4_Word req_mask = get_routine_arg(1, 0);
        if (req_mask == 0) {
            break;
        }
    }

    resume();
}

/*
 * Inject data into the given vspace.
 * TODO: Don't keep these pages mapped in
 */
static int load_segment_into_vspace(seL4_ARM_PageDirectory dest_pd,
        struct app_addrspace *dest_as,
        char *src, unsigned long segment_size,
        unsigned long file_size, unsigned long dst,
        unsigned long permissions) {

    /* Overview of ELF segment loading

dst: destination base virtual address of the segment being loaded
segment_size: obvious

So the segment range to "load" is [dst, dst + segment_size).

The content to load is either zeros or the content of the ELF
file itself, or both.

The split between file content and zeros is a follows.

File content: [dst, dst + file_size)
Zeros:        [dst + file_size, dst + segment_size)

Note: if file_size == segment_size, there is no zero-filled region.
Note: if file_size == 0, the whole segment is just zero filled.

The code below relies on seL4's frame allocator already
zero-filling a newly allocated frame.

*/



    assert(file_size <= segment_size);

    unsigned long pos;

    /* We work a page at a time in the destination vspace. */
    pos = 0;

    while(pos < segment_size) {
        seL4_Word sos_vaddr;
        seL4_CPtr sos_cap, tty_cap;
        int nbytes;
        int err;

        /* Map the frame into address space */
        jmp_buf reenter_entry;
        int reenter = setjmp(reenter_entry);
        if (!reenter) {
            start_coroutine(sos_map_page, reenter_entry, dst, &sos_vaddr);
        } else {
           /* Now copy our data into the destination vspace. */
            nbytes = PAGESIZE - (dst & PAGEMASK);
            if (pos < file_size){
                memcpy((void*) (sos_vaddr | ((dst << LOWER_BITS_SHIFT) >> LOWER_BITS_SHIFT)),
                        (void*)src, MIN(nbytes, file_size - pos));
            }
            sos_cap = get_cap(sos_vaddr);

            /* Not observable to I-cache yet so flush the frame */
            seL4_ARM_Page_Unify_Instruction(sos_cap, 0, PAGESIZE);

            pos += nbytes;
            dst += nbytes;
            src += nbytes;    
        }


    }
    return 0;
}

int elf_load(seL4_ARM_PageDirectory dest_pd, struct app_addrspace *dest_as, char *elf_file) {

    int num_headers;
    int err;
    int i;

    /* Ensure that the ELF file looks sane. */
    if (elf_checkFile(elf_file)){
        return seL4_InvalidArgument;
    }

    /* Set mapping jmp point */
    err = setjmp(mapping_entry);
    if (!err) {
        start_coroutine(mapping_loop, mapping_entry, _sos_ipc_ep_cap, 1);
    }

    num_headers = elf_getNumProgramHeaders(elf_file);
    for (i = 0; i < num_headers; i++) {
        char *source_addr;
        unsigned long flags, file_size, segment_size, vaddr;

        /* Skip non-loadable segments (such as debugging data). */
        if (elf_getProgramHeaderType(elf_file, i) != PT_LOAD)
            continue;

        /* Fetch information about this segment. */
        source_addr = elf_file + elf_getProgramHeaderOffset(elf_file, i);
        file_size = elf_getProgramHeaderFileSize(elf_file, i);
        segment_size = elf_getProgramHeaderMemorySize(elf_file, i);
        vaddr = elf_getProgramHeaderVaddr(elf_file, i);
        flags = elf_getProgramHeaderFlags(elf_file, i);

        /* Copy it across into the vspace. */
        dprintf(1, " * Loading segment %08x-->%08x\n", (int)vaddr, (int)(vaddr + segment_size));

        /* Define region */
        err = as_define_region(dest_as, vaddr, segment_size, get_sel4_rights_from_elf(flags) & seL4_AllRights);
        if (err) {
            return err;
        }

        /* Load segment */
        err = load_segment_into_vspace(dest_pd, dest_as, source_addr, segment_size, file_size, vaddr, get_sel4_rights_from_elf(flags) & seL4_AllRights);
        if (err) {
            return err;
        }
        conditional_panic(err != 0, "Elf loading failed!\n");
    }

    return 0;
}
