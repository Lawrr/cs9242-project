#include <cspace/cspace.h>
#include <cpio/cpio.h>

#include "vmem_layout.h"
#include "file.h"
#include "process.h"

#define verbose 5
#include <assert.h>
#include <sys/panic.h>

extern _cpio_archive[]; 

extern struct oft_entry of_table[];

struct PCB tty_test_process;

struct PCB *curproc = &tty_test_process;

void start_process(char* app_name, seL4_CPtr fault_ep) {
    int err;

    seL4_CPtr user_ep_cap;

    /* These required for setting up the TCB */
    seL4_UserContext context;

    /* These required for loading program sections */
    char* elf_base;
    unsigned long elf_size;

    tty_test_process.addrspace = as_new();

    /*open file table increase ref count*/
    //of_table[STDIN].ref_count++;
    of_table[STDOUT].ref_count += 1;

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
                       &tty_test_process.ipc_buffer_addr);
    tty_test_process.ipc_buffer_cap = get_cap(tty_test_process.ipc_buffer_addr);
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


