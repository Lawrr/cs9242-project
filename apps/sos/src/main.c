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

#include <cspace/cspace.h>

#include <cpio/cpio.h>
#include <nfs/nfs.h>
#include <elf/elf.h>
#include <serial/serial.h>
#include <clock/clock.h>
#include <utils/page.h>

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
#include <autoconf.h>

#define verbose 5
#include <sys/debug.h>
#include <sys/panic.h>

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP          (1)
/* To differencient between async and and sync IPC, we assign a
 * badge to the async endpoint. The badge that we receive will
 * be the bitwise 'OR' of the async endpoint badge and the badges
 * of all pending notifications. */
#define IRQ_EP_BADGE         (1 << (seL4_BadgeBits - 1))
/* All badged IRQs set high bet, then we use uniq bits to
 * distinguish interrupt sources */
#define IRQ_BADGE_NETWORK (1 << 0)
#define IRQ_BADGE_TIMER (1 << 1)

#define TTY_NAME             CONFIG_SOS_STARTUP_APP
#define TTY_PRIORITY         (0)
#define TTY_EP_BADGE         (101)

#define EPIT1_PADDR 0x020D0000
#define EPIT2_PADDR 0x020D4000
#define EPIT_REGISTERS 5

/* The linker will link this symbol to the start address  *
 * of an archive of attached applications.                */
extern char _cpio_archive[];

const seL4_BootInfo* _boot_info;

struct PCB tty_test_process;

//struct PCB PCB_Array[MAX_PROCESS];
char *console = "console";

seL4_CPtr _sos_ipc_ep_cap;
seL4_CPtr _sos_interrupt_ep_cap;

struct serial *serial_handle;

struct oft_entry of_table[MAX_OPEN_FILE];
seL4_Word ofd_count = 0;
seL4_Word curr_free_ofd = 3;

void of_table_init() {
   of_table[STD_IN].ptr = console;
   of_table[STD_IN].file_info.st_fmode = FM_READ;
   of_table[STD_OUT].ptr = console;
   of_table[STD_OUT].file_info.st_fmode = FM_WRITE;
   of_table[STD_INOUT].ptr = console;
   of_table[STD_INOUT].file_info.st_fmode = FM_WRITE | FM_READ;
}

/**
 * NFS mount point
 */
extern fhandle_t mnt_point;

void handle_syscall(seL4_Word badge, int num_args) {
    seL4_Word syscall_number;
    seL4_CPtr reply_cap;

    syscall_number = seL4_GetMR(0);

    /* Save the caller */
    reply_cap = cspace_save_reply_cap(cur_cspace);
    assert(reply_cap != CSPACE_NULL);

    printf("Syscall id: %d - received from user application\n", syscall_number);

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

        default:
            printf("Unknown syscall %d\n", syscall_number);
            /* we don't want to reply to an unknown syscall */

            /* Free the saved reply cap */
            cspace_free_slot(cur_cspace, reply_cap);
    }
}

void syscall_loop(seL4_CPtr ep) {
    while (1) {
        seL4_Word badge;
        seL4_Word label;
        seL4_MessageInfo_t message;

        message = seL4_Wait(ep, &badge);
        label = seL4_MessageInfo_get_label(message);
        if(badge & IRQ_EP_BADGE){
            /* Interrupt */
            if (badge & IRQ_BADGE_NETWORK) {
                network_irq();
            }
            if (badge & IRQ_BADGE_TIMER) {
                timer_interrupt();
            }

        }else if(label == seL4_VMFault){
            /* Page fault */
            dprintf(0, "vm fault at 0x%08x, pc = 0x%08x, %s\n", 
                    seL4_GetMR(1),
                    seL4_GetMR(0),
                    seL4_GetMR(2) ? "Instruction Fault" : "Data fault");
            int err;
            
            seL4_Word sos_vaddr, map_vaddr;

            /* Check whether instruction fault or data fault */
            if (seL4_GetMR(2)) {
                /* Instruction fault */
                map_vaddr = seL4_GetMR(0);
            } else {
                /* Data fault */
                map_vaddr = seL4_GetMR(1);
            }

            //Not used
            seL4_CPtr app_cap;
            err = sos_map_page(map_vaddr,
                               tty_test_process.vroot,
                               tty_test_process.addrspace,
                               &sos_vaddr,
                               &app_cap);
            conditional_panic(err, "Fail to map the page to the application\n"); 

            /* Save the caller */
            seL4_CPtr reply_cap = cspace_save_reply_cap(cur_cspace);
            assert(reply_cap != CSPACE_NULL);

            seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 0);
            seL4_Send(reply_cap, reply);

        }else if(label == seL4_NoFault) {
            /* System call */
            handle_syscall(badge, seL4_MessageInfo_get_length(message) - 1);

        }else{
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

void start_first_process(char* app_name, seL4_CPtr fault_ep) {
    int err;

    seL4_CPtr user_ep_cap;

    /* These required for setting up the TCB */
    seL4_UserContext context;

    /* These required for loading program sections */
    char* elf_base;
    unsigned long elf_size;

    tty_test_process.addrspace = as_new();

    /*open file table increase ref count*/
    of_table[STD_IN].ref++;
    of_table[STD_OUT].ref+=2;

    /* Create a VSpace */
    tty_test_process.vroot_addr = ut_alloc(seL4_PageDirBits);
    conditional_panic(!tty_test_process.vroot_addr, 
                      "No memory for new Page Directory");
    err = cspace_ut_retype_addr(tty_test_process.vroot_addr,
                                seL4_ARM_PageDirectoryObject,
                                seL4_PageDirBits,
                                cur_cspace,
                                &tty_test_process.vroot);
    conditional_panic(err, "Failed to allocate page directory cap for client");

    /* Create a simple 1 level CSpace */
    tty_test_process.croot = cspace_create(1);
    assert(tty_test_process.croot != NULL);

    /* IPC buffer region */
    err = as_define_region(tty_test_process.addrspace,
                           PROCESS_IPC_BUFFER,
                           (1 << seL4_PageBits),
                           seL4_AllRights);
    conditional_panic(err, "Could not define IPC buffer region");

    /* Create an IPC buffer */
    err = sos_map_page(PROCESS_IPC_BUFFER,
                       tty_test_process.vroot,
                       tty_test_process.addrspace,
                       &tty_test_process.ipc_buffer_addr,
		       &tty_test_process.ipc_buffer_cap);
    conditional_panic(err, "No memory for ipc buffer");
    /* TODO dud asid number
    err = get_app_cap(tty_test_process.ipc_buffer_addr,
                      tty_test_process.addrspace->page_table,
                      &tty_test_process.ipc_buffer_cap);
    conditional_panic(err, "Can't get app cap");*/

    /* Copy the fault endpoint to the user app to enable IPC */
    user_ep_cap = cspace_mint_cap(tty_test_process.croot,
                                  cur_cspace,
                                  fault_ep,
                                  seL4_AllRights, 
                                  seL4_CapData_Badge_new(TTY_EP_BADGE));
    
    /* should be the first slot in the space, hack I know */
    assert(user_ep_cap == 1);
    assert(user_ep_cap == USER_EP_CAP);

    /* Create a new TCB object */
    tty_test_process.tcb_addr = ut_alloc(seL4_TCBBits);
    conditional_panic(!tty_test_process.tcb_addr, "No memory for new TCB");
    err =  cspace_ut_retype_addr(tty_test_process.tcb_addr,
                                 seL4_TCBObject,
                                 seL4_TCBBits,
                                 cur_cspace,
                                 &tty_test_process.tcb_cap);
    conditional_panic(err, "Failed to create TCB");

    /* Configure the TCB */
    err = seL4_TCB_Configure(tty_test_process.tcb_cap, user_ep_cap, TTY_PRIORITY,
                             tty_test_process.croot->root_cnode, seL4_NilData,
                             tty_test_process.vroot, seL4_NilData, PROCESS_IPC_BUFFER,
                             tty_test_process.ipc_buffer_cap);
    conditional_panic(err, "Unable to configure new TCB");


    /* parse the cpio image */
    dprintf(1, "\nStarting \"%s\"...\n", app_name);
    elf_base = cpio_get_file(_cpio_archive, app_name, &elf_size);
    conditional_panic(!elf_base, "Unable to locate cpio header");

    /* load the elf image */
    err = elf_load(tty_test_process.vroot, tty_test_process.addrspace, elf_base);
    conditional_panic(err, "Failed to load elf image");

    /* Heap region */
    err = as_define_region(tty_test_process.addrspace,
                           PROCESS_HEAP_START,
                           0,
                           seL4_AllRights);
    conditional_panic(err, "Could not define heap region");

    /* Stack region */
    err = as_define_region(tty_test_process.addrspace,
                           PROCESS_STACK_BOT,
                           PROCESS_STACK_TOP - PROCESS_STACK_BOT,
                           seL4_AllRights);
    conditional_panic(err, "Could not define stack region");

    /* Start the new process */
    memset(&context, 0, sizeof(context));
    context.pc = elf_getEntryPoint(elf_base);
    context.sp = PROCESS_STACK_TOP;
    seL4_TCB_WriteRegisters(tty_test_process.tcb_cap, 1, 0, 2, &context);
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

    /* Initialise open file table */
    of_table_init(); 

    /* Initialiase other system compenents here */

    _sos_ipc_init(ipc_ep, async_ep);
}

static inline seL4_CPtr badge_irq_ep(seL4_CPtr ep, seL4_Word badge) {
    seL4_CPtr badged_cap = cspace_mint_cap(cur_cspace, cur_cspace, ep, seL4_AllRights, seL4_CapData_Badge_new(badge | IRQ_EP_BADGE));
    conditional_panic(!badged_cap, "Failed to allocate badged cap");
    return badged_cap;
}

void frame_table_test() {
    /* Allocate 10 pages and make sure you can touch them all */
    printf("Test 1\n");
    for (int i = 0; i < 10; i++) {
        /* Allocate a page */
        seL4_Word *vaddr;
        frame_alloc(&vaddr);
        assert(vaddr);

        /* Test you can touch the page */
        *vaddr = 0x37;
        assert(*vaddr == 0x37);

        printf("Page #%d allocated at %p\n",  i, (void *) vaddr);
    }

    /* Test that you never run out of memory if you always free frames. */
    printf("Test 2\n");
    for (int i = 0; i < 10000; i++) {
        /* Allocate a page */
        seL4_Word *vaddr;
        int page = frame_alloc(&vaddr);
        assert(vaddr != 0);

        /* Test you can touch the page */
        *vaddr = 0x37;
        assert(*vaddr == 0x37);

        /* print every 1000 iterations */
        if (i % 1000 == 0) {
            printf("Page #%d allocated at %p\n",  i, vaddr);
        }

        frame_free(vaddr);
    }

    /* Test that you eventually run out of memory gracefully,
       and doesn't crash */
    printf("Test 3\n");
    while (1) {
        /* Allocate a page */
        seL4_Word *vaddr;
        frame_alloc(&vaddr);
        if (!vaddr) {
            printf("Out of memory!\n");
            break;
        }

        /* Test you can touch the page */
        *vaddr = 0x37;
        assert(*vaddr == 0x37);
    }
}

/*
 * Main entry point - called by crt.
 */
int main(void) {

    dprintf(0, "\nSOS Starting...\n");

    _sos_init(&_sos_ipc_ep_cap, &_sos_interrupt_ep_cap);

    /* Initialise the network hardware */
    network_init(badge_irq_ep(_sos_interrupt_ep_cap, IRQ_BADGE_NETWORK));

    /* Initialise the timer */
    void *epit1_vaddr = map_device(EPIT1_PADDR, EPIT_REGISTERS * sizeof(uint32_t));
    void *epit2_vaddr = map_device(EPIT2_PADDR, EPIT_REGISTERS * sizeof(uint32_t));
    timer_init(epit1_vaddr, epit2_vaddr);
    seL4_CPtr timer_badge = badge_irq_ep(_sos_interrupt_ep_cap, IRQ_BADGE_TIMER);
    start_timer(timer_badge);
    
    /* Initialise serial driver */
    serial_handle = serial_init();
    
    /* Start the user application */
    start_first_process(TTY_NAME, _sos_ipc_ep_cap);

    //frame_table_test();

    /* Wait on synchronous endpoint for IPC */
    dprintf(0, "\nSOS entering syscall loop\n");
    syscall_loop(_sos_ipc_ep_cap);

    /* Not reached */
    return 0;
}


