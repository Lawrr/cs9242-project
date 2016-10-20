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
#include <setjmp.h>
#include <cspace/cspace.h>
#include <utils/page.h>
#include <ut_manager/ut.h>

#include "elf.h"
#include "addrspace.h"
#include "frametable.h"
#include "process.h"
#include "mapping.h"
#include "vmem_layout.h"

#define verbose 0
#include <sys/debug.h>
#include <sys/panic.h>

/* Minimum of two values. */
#define MIN(a, b) (((a)<(b))?(a):(b))

#define PAGESIZE (1 << (seL4_PageBits))
#define PAGEMASK ((PAGESIZE) - 1)
#define PAGE_ALIGN(addr) ((addr) & ~(PAGEMASK))
#define IS_PAGESIZE_ALIGNED(addr) !((addr) & (PAGEMASK))

#define LOWER_BITS_SHIFT 20

/* To differencient between async and and sync IPC, we assign a
 * badge to the async endpoint. The badge that we receive will
 * be the bitwise 'OR' of the async endpoint badge and the badges
 * of all pending notifications. */
#define IRQ_EP_BADGE (1 << (seL4_BadgeBits - 1))
/* All badged IRQs set high bet, then we use uniq bits to
 * distinguish interrupt sources */
#define IRQ_BADGE_NETWORK (1 << 0)
#define IRQ_BADGE_TIMER (1 << 1)

extern seL4_ARM_PageDirectory dest_as;

extern jmp_buf syscall_loop_entry;
extern seL4_CPtr _sos_ipc_ep_cap;
extern struct PCB *curproc;
extern int curr_coroutine_id;

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

static void first_process_mapping_loop(seL4_CPtr ep) {
    seL4_Word badge;
    seL4_Word label;
    seL4_MessageInfo_t message;
    while (1) {
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

        seL4_Word req_mask = get_routine_arg(curr_coroutine_id, 0);
        if (req_mask == 0 || req_mask == -1) {
            return;
        }
    }
}

/*
 * Inject data into the given vspace.
 * TODO: Don't keep these pages mapped in
 *
 * Note: This should only be used to load the initial app
 */
static int cpio_elf_load_segment_into_vspace(seL4_ARM_PageDirectory dest_pd,
        struct app_addrspace *dest_as,
        char *src, unsigned long segment_size,
        unsigned long file_size, unsigned long dst,
        unsigned long permissions, struct PCB *pcb) {

    /* Overview of ELF segment loading

    dst: destination base virtual address of the segment being loaded
    segment_size: obvious

    So the segment range to "load" is [dst, dst + segment_size).

    The content to load is either zeros or the content of the ELF
    file itself, or both.

    The split between file content and zeros is a follows.

    File content: [dst, dst + file_size)
    Zeros: [dst + file_size, dst + segment_size)

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
        int reenter = setjmp(syscall_loop_entry);
        cleanup_coroutine();
        if (!reenter) {
            start_coroutine(sos_map_page, dst, &sos_vaddr, pcb);
        } else {
            /* Check the mask to see if we are done with the loop */
            seL4_Word req_mask = get_routine_arg(curr_coroutine_id, 0);
            if (req_mask == -1) return -1;
            if (req_mask) {
                first_process_mapping_loop(_sos_ipc_ep_cap);
                resume();
            } else {
                /* Now copy our data into the destination vspace. */
                nbytes = PAGESIZE - (dst & PAGEMASK);
                if (pos < file_size) {
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
    }

    return 0;
}

int cpio_elf_load(seL4_ARM_PageDirectory dest_pd, struct PCB *pcb, char *elf_file) {

    int num_headers;
    int err;
    int i;

    struct app_addrspace *dest_as = pcb->addrspace;

    /* Ensure that the ELF file looks sane. */
    if (elf_checkFile(elf_file)) {
        return seL4_InvalidArgument;
    }

    num_headers = elf_getNumProgramHeaders(elf_file);
    for (i = 0; i < num_headers; i++) {
        char *source_addr;
        unsigned long flags, file_size, segment_size, vaddr;

        /* Skip non-loadable segments (such as debugging data). */
        if (elf_getProgramHeaderType(elf_file, i) != PT_LOAD)
            continue;

        /* Fetch information about this segment. */
        source_addr =  elf_file + elf_getProgramHeaderOffset(elf_file, i);
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
        err = cpio_elf_load_segment_into_vspace(dest_pd,
                dest_as,
                source_addr,
                segment_size,
                file_size, vaddr,
                get_sel4_rights_from_elf(flags) & seL4_AllRights,
                pcb);
        if (err) {
            return err;
        }
    }

    return 0;
}


static int elf_load_segment_into_vspace(seL4_ARM_PageDirectory dest_pd,
        struct app_addrspace *dest_as,
        char *src, unsigned long segment_size,
        unsigned long file_size, unsigned long dst,
        unsigned long permissions, struct PCB *pcb,
        struct vnode *vnode) {

    /* Overview of ELF segment loading

    dst: destination base virtual address of the segment being loaded
    segment_size: obvious

    So the segment range to "load" is [dst, dst + segment_size).

    The content to load is either zeros or the content of the ELF
    file itself, or both.

    The split between file content and zeros is a follows.

    File content: [dst, dst + file_size)
    Zeros: [dst + file_size, dst + segment_size)

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
        sos_map_page(dst, &sos_vaddr, pcb);
        nbytes = PAGESIZE - (dst & PAGEMASK);
        if (pos < file_size) {
            struct uio uio = {
                .uaddr = dst,
                .vaddr = NULL,
                .size = MIN(nbytes, file_size - pos),
                .remaining = MIN(nbytes, file_size - pos),
                .offset = src,
                .pcb = pcb
            };

            int err = vnode->ops->vop_read(vnode, &uio);
            if (err) return -1;
        }
        sos_cap = get_cap(sos_vaddr);

        /* Not observable to I-cache yet so flush the frame */
        seL4_ARM_Page_Unify_Instruction(sos_cap, 0, PAGESIZE);

        pos += nbytes;
        dst += nbytes;
        src += nbytes;

    }
    return 0;
}

int elf_load(seL4_ARM_PageDirectory dest_pd, struct PCB *pcb, char *elf_file, struct vnode *vnode) {

    int num_headers;
    int err;
    int i;

    struct app_addrspace *dest_as = pcb->addrspace;

    /* Ensure that the ELF file looks sane. */
    if (elf_checkFile(elf_file)) {
        return seL4_InvalidArgument;
    }

    num_headers = elf_getNumProgramHeaders(elf_file);
    for (i = 0; i < num_headers; i++) {
        char *source_addr;
        unsigned long flags, file_size, segment_size, vaddr;

        /* Skip non-loadable segments (such as debugging data). */
        if (elf_getProgramHeaderType(elf_file, i) != PT_LOAD)
            continue;

        /* Fetch information about this segment. */
        source_addr =  elf_getProgramHeaderOffset(elf_file, i);
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
        err = elf_load_segment_into_vspace(dest_pd,
                dest_as,
                source_addr,
                segment_size,
                file_size, vaddr,
                get_sel4_rights_from_elf(flags) & seL4_AllRights,
                pcb,
                vnode);
        if (err) {
            return err;
        }
    }

    return 0;
}
