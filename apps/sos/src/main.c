/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <setjmp.h>

#include <cspace/cspace.h>

#include <cpio/cpio.h>
#include <nfs/nfs.h>
#include <elf/elf.h>
#include <serial/serial.h>
#include <clock/clock.h>
#include <utils/page.h>
#include <autoconf.h>

#include "addrspace.h"
#include "frametable.h"
#include "network.h"
#include "elf.h"
#include "sos_syscall.h"
#include "ut_manager/ut.h"
#include "vmem_layout.h"
#include "mapping.h"
#include "sos.h"
#include "file.h"
#include "process.h"
#include "vnode.h"
#include "console.h"
#include "coroutine.h"

#define verbose 5
#include <sys/debug.h>
#include <sys/panic.h>

/* To differencient between async and and sync IPC, we assign a
 * badge to the async endpoint. The badge that we receive will
 * be the bitwise 'OR' of the async endpoint badge and the badges
 * of all pending notifications. */
#define IRQ_EP_BADGE         (1 << (seL4_BadgeBits - 1))
/* All badged IRQs set high bet, then we use uniq bits to
 * distinguish interrupt sources */
#define IRQ_BADGE_NETWORK (1 << 0)
#define IRQ_BADGE_TIMER (1 << 1)



#define EPIT1_PADDR 0x020D0000
#define EPIT2_PADDR 0x020D4000
#define EPIT_REGISTERS 5

#define NFS_TIMEOUT_INTERVAL 100000 /* Microseconds */

char *sys_name[14] = {
    "Sos write",
    "Sos read",
    "Sos open",
    "Sos close",
    "Sos brk",
    "Sos sleep",
    "Sos timestamp",
    "Sos getdirent",
    "Sos stat",
    "Sos process create",
    "Sos process delete",
    "Sos process id",
    "Sos process wait",
    "Sos process status"
};
/* The linker will link this symbol to the start address  *
 * of an archive of attached applications.                */
extern char _cpio_archive[];

/**
 * NFS mount point
 */
extern fhandle_t mnt_point;

extern struct PCB *curproc;

const seL4_BootInfo* _boot_info;

jmp_buf syscall_loop_entry;

seL4_CPtr _sos_ipc_ep_cap;
seL4_CPtr _sos_interrupt_ep_cap;

struct oft_entry of_table[MAX_OPEN_FILE];
seL4_Word ofd_count = 0;
seL4_Word curr_free_ofd = 0;

static void of_table_init() {
    /* Add console device */
    struct vnode *console_vnode;
    console_init(&console_vnode);

    /* Set up of table */

    /* Note: Below line is not needed. Client must explicitly open STDIN */
    //of_table[STDIN].vnode = console_vnode;
    //of_table[STDIN].file_info.st_fmode = FM_READ;

    of_table[STDOUT].vnode = console_vnode;
    of_table[STDOUT].file_info.st_fmode = FM_WRITE;
    ofd_count++;
    curr_free_ofd++;
}

void handle_syscall(seL4_Word badge, int num_args) {
    seL4_Word syscall_number;
    seL4_CPtr reply_cap;

    syscall_number = seL4_GetMR(0);

    printf("[App #%d] Syscall :%s  -- received from user application\n", badge, sys_name[syscall_number]);

    /* Save the caller */
    reply_cap = cspace_save_reply_cap(cur_cspace);
    assert(reply_cap != CSPACE_NULL);

    /* Process system call */
    switch (syscall_number) {
        case SOS_WRITE_SYSCALL:
            syscall_write(reply_cap);
            break;

        case SOS_READ_SYSCALL:
            syscall_read(reply_cap);
            break;

        case SOS_OPEN_SYSCALL:
            syscall_open(reply_cap);
            break;

        case SOS_CLOSE_SYSCALL:
            syscall_close(reply_cap);
            break;

        case SOS_BRK_SYSCALL:
            syscall_brk(reply_cap);
            break;

        case SOS_USLEEP_SYSCALL:
            syscall_usleep(reply_cap); 
            break;

        case SOS_TIME_STAMP_SYSCALL:
            syscall_time_stamp(reply_cap);
            break;

        case SOS_GETDIRENT_SYSCALL:
            syscall_getdirent(reply_cap);
            break;

        case SOS_STAT_SYSCALL:
            syscall_stat(reply_cap);
            break;

        case SOS_PROCESS_CREATE_SYSCALL:
            syscall_process_create(reply_cap, badge);
            break;

        case SOS_PROCESS_DELETE_SYSCALL:
            syscall_process_delete(reply_cap, badge);
            break;

        case SOS_PROCESS_ID_SYSCALL:
            syscall_process_id(reply_cap, badge);
            break;

        case SOS_PROCESS_WAIT_SYSCALL:
            syscall_process_wait(reply_cap, badge);
            break;

        case SOS_PROCESS_STATUS_SYSCALL:
            syscall_process_status(reply_cap);
            break;
        case SOS_VM_SHARE_SYSCALL:
            syscall_vm_share(reply_cap);
            break;

        default:
            printf("Unknown syscall %d\n", syscall_number);
            /* we don't want to reply to an unknown syscall */

            /* Free the saved reply cap */
            cspace_free_slot(cur_cspace, reply_cap);
    }
}

static void vm_fault_handler(seL4_Word badge, int num_args) {
    /* Save the caller */
    seL4_CPtr reply_cap = cspace_save_reply_cap(cur_cspace);
    assert(reply_cap != CSPACE_NULL);

    int err;
    seL4_Word sos_vaddr, map_vaddr, instruction_vaddr;

    int isInstruction = seL4_GetMR(2);
    /* Check whether instruction fault or data fault */
    printf("[App #%d] ", badge);
    if (isInstruction) {
        /* Instruction fault */
        printf("Instruction fault - ");
        map_vaddr = seL4_GetMR(0);
    } else {
        /* Data fault */
        printf("Data fault - ");
        map_vaddr = seL4_GetMR(1); 
        instruction_vaddr = seL4_GetMR(0);
        pin_frame_entry(PAGE_ALIGN_4K(instruction_vaddr), PAGE_SIZE_4K);
    }

    printf("In vm_fault_handler for uaddr: %p, instr: %p\n", map_vaddr, seL4_GetMR(0));

    err = sos_map_page(map_vaddr, &sos_vaddr, curproc);

    if (err) {
        printf("Vm fault error: %d - Destroying process %d\n", err, curproc->pid);
        process_destroy(curproc->pid);
    } else {
        if (!isInstruction) {
            unpin_frame_entry(PAGE_ALIGN_4K(instruction_vaddr), PAGE_SIZE_4K);
        }

        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 0);
        seL4_Send(reply_cap, reply);

        /* Free the saved reply cap */
        cspace_free_slot(cur_cspace, reply_cap); 
    }
}

void syscall_loop(seL4_CPtr ep) {
    seL4_Word badge;
    seL4_Word label;
    seL4_MessageInfo_t message;
    while (1) {
        setjmp(syscall_loop_entry); 
        cleanup_coroutine();
        resume();

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

        } else if (label == seL4_VMFault) {
            /* Page fault */
            curproc = process_status(badge);

            start_coroutine(&vm_fault_handler, badge,
                            seL4_MessageInfo_get_length(message) - 1,
                            NULL);

        } else if (label == seL4_NoFault) {
            /* System call */
            curproc = process_status(badge);
            
            start_coroutine(&handle_syscall, badge,
                            seL4_MessageInfo_get_length(message) - 1,
                            NULL);

            /* Self destruct after a create/delete syscall */
            if (curproc->status == PROCESS_STATUS_SELF_DESTRUCT) {
                process_destroy(curproc->pid);
            }

        } else {
            printf("Rootserver got an unknown message\n");
        }
    }
}

static void print_bootinfo(const seL4_BootInfo* info) {
    int i;

    /* General info */
    dprintf(1, "Info Page:  %p\n", info);
    dprintf(1,"IPC Buffer: %p\n", info->ipcBuffer);
    dprintf(1,"Node ID: %d (of %d)\n",info->nodeID, info->numNodes);
    dprintf(1,"IOPT levels: %d\n",info->numIOPTLevels);
    dprintf(1,"Init cnode size bits: %d\n", info->initThreadCNodeSizeBits);

    /* Cap details */
    dprintf(1,"\nCap details:\n");
    dprintf(1,"Type              Start      End\n");
    dprintf(1,"Empty             0x%08x 0x%08x\n", info->empty.start, info->empty.end);
    dprintf(1,"Shared frames     0x%08x 0x%08x\n", info->sharedFrames.start, 
                                                   info->sharedFrames.end);
    dprintf(1,"User image frames 0x%08x 0x%08x\n", info->userImageFrames.start, 
                                                   info->userImageFrames.end);
    dprintf(1,"User image PTs    0x%08x 0x%08x\n", info->userImagePTs.start, 
                                                   info->userImagePTs.end);
    dprintf(1,"Untypeds          0x%08x 0x%08x\n", info->untyped.start, info->untyped.end);

    /* Untyped details */
    dprintf(1,"\nUntyped details:\n");
    dprintf(1,"Untyped Slot       Paddr      Bits\n");
    for (i = 0; i < info->untyped.end-info->untyped.start; i++) {
        dprintf(1,"%3d     0x%08x 0x%08x %d\n", i, info->untyped.start + i,
                                                   info->untypedPaddrList[i],
                                                   info->untypedSizeBitsList[i]);
    }

    /* Device untyped details */
    dprintf(1,"\nDevice untyped details:\n");
    dprintf(1,"Untyped Slot       Paddr      Bits\n");
    for (i = 0; i < info->deviceUntyped.end-info->deviceUntyped.start; i++) {
        dprintf(1,"%3d     0x%08x 0x%08x %d\n", i, info->deviceUntyped.start + i,
                                                   info->untypedPaddrList[i + (info->untyped.end - info->untyped.start)],
                                                   info->untypedSizeBitsList[i + (info->untyped.end-info->untyped.start)]);
    }

    dprintf(1,"-----------------------------------------\n\n");

    /* Print cpio data */
    dprintf(1,"Parsing cpio data:\n");
    dprintf(1,"--------------------------------------------------------\n");
    dprintf(1,"| index |        name      |  address   | size (bytes) |\n");
    dprintf(1,"|------------------------------------------------------|\n");
    for(i = 0;; i++) {
        unsigned long size;
        const char *name;
        void *data;

        data = cpio_get_entry(_cpio_archive, i, &name, &size);
        if(data != NULL){
            dprintf(1,"| %3d   | %16s | %p | %12d |\n", i, name, data, size);
        }else{
            break;
        }
    }
    dprintf(1,"--------------------------------------------------------\n");
}



static void _sos_ipc_init(seL4_CPtr* ipc_ep, seL4_CPtr* async_ep){
    seL4_Word ep_addr, aep_addr;
    int err;

    /* Create an Async endpoint for interrupts */
    aep_addr = ut_alloc(seL4_EndpointBits);
    conditional_panic(!aep_addr, "No memory for async endpoint");
    err = cspace_ut_retype_addr(aep_addr,
                                seL4_AsyncEndpointObject,
                                seL4_EndpointBits,
                                cur_cspace,
                                async_ep);
    conditional_panic(err, "Failed to allocate c-slot for Interrupt endpoint");

    /* Bind the Async endpoint to our TCB */
    err = seL4_TCB_BindAEP(seL4_CapInitThreadTCB, *async_ep);
    conditional_panic(err, "Failed to bind ASync EP to TCB");


    /* Create an endpoint for user application IPC */
    ep_addr = ut_alloc(seL4_EndpointBits);
    conditional_panic(!ep_addr, "No memory for endpoint");
    err = cspace_ut_retype_addr(ep_addr, 
                                seL4_EndpointObject,
                                seL4_EndpointBits,
                                cur_cspace,
                                ipc_ep);
    conditional_panic(err, "Failed to allocate c-slot for IPC endpoint");
}


static void _sos_init(seL4_CPtr* ipc_ep, seL4_CPtr* async_ep){
    seL4_Word dma_addr;
    seL4_Word low, high;
    int err;

    /* Retrieve boot info from seL4 */
    _boot_info = seL4_GetBootInfo();
    conditional_panic(!_boot_info, "Failed to retrieve boot info\n");
    if(verbose > 0){
        print_bootinfo(_boot_info);
    }

    /* Initialise the untyped sub system and reserve memory for DMA */
    err = ut_table_init(_boot_info);
    conditional_panic(err, "Failed to initialise Untyped Table\n");
    /* DMA uses a large amount of memory that will never be freed */
    dma_addr = ut_steal_mem(DMA_SIZE_BITS);
    conditional_panic(dma_addr == 0, "Failed to reserve DMA memory\n");

    /* find available memory */
    ut_find_memory(&low, &high);

    /* Initialise the untyped memory allocator */
    ut_allocator_init(low, high);

    /* Initialise the cspace manager */
    err = cspace_root_task_bootstrap(ut_alloc, ut_free, ut_translate,
                                     malloc, free);
    conditional_panic(err, "Failed to initialise the c space\n");

    /* Initialise DMA memory */
    err = dma_init(dma_addr, DMA_SIZE_BITS);
    conditional_panic(err, "Failed to intiialise DMA memory\n");

    /* Initialise frame table */
    frame_init(high,low);

    /* Initialise vfs */
    vfs_init();

    /* Initialiase other system compenents here */

    _sos_ipc_init(ipc_ep, async_ep);
}

static inline seL4_CPtr badge_irq_ep(seL4_CPtr ep, seL4_Word badge) {
    seL4_CPtr badged_cap = cspace_mint_cap(cur_cspace, cur_cspace, ep, seL4_AllRights, seL4_CapData_Badge_new(badge | IRQ_EP_BADGE));
    conditional_panic(!badged_cap, "Failed to allocate badged cap");
    return badged_cap;
}

static void nfs_timeout_callback(uint32_t id, void *data) {
    register_timer(NFS_TIMEOUT_INTERVAL, nfs_timeout_callback, NULL);
    nfs_timeout();
}

/*
 * Main entry point - called by crt.
 */
int main(void) {

    dprintf(0, "\nSOS Starting...\n");

    _sos_init(&_sos_ipc_ep_cap, &_sos_interrupt_ep_cap);

    /* Initialise the network hardware */
    network_init(badge_irq_ep(_sos_interrupt_ep_cap, IRQ_BADGE_NETWORK));

    /* Initialise coroutines */
    coroutine_init();

    /* Start the user application */
    int proc_id = process_new_cpio(TTY_NAME, _sos_ipc_ep_cap, -1);
    conditional_panic(proc_id == -1, "Could not start first process\n");

    /* Initialise open file table */
    of_table_init();

    /* Initialise the timer */
    void *epit1_vaddr = map_device(EPIT1_PADDR, EPIT_REGISTERS * sizeof(uint32_t));
    void *epit2_vaddr = map_device(EPIT2_PADDR, EPIT_REGISTERS * sizeof(uint32_t));
    timer_init(epit1_vaddr, epit2_vaddr);
    seL4_CPtr timer_badge = badge_irq_ep(_sos_interrupt_ep_cap, IRQ_BADGE_TIMER);
    start_timer(timer_badge);
    
    /* NFS timeout every 100ms */
    register_timer(NFS_TIMEOUT_INTERVAL, nfs_timeout_callback, NULL);

    /* Wait on synchronous endpoint for IPC */
    dprintf(0, "\nSOS entering syscall loop\n");
    syscall_loop(_sos_ipc_ep_cap);

    /* Not reached */
}
