#include <cspace/cspace.h>
#include <cpio/cpio.h>
#include <limits.h>

#include "vmem_layout.h"
#include "file.h"
#include "process.h"
#include "addrspace.h"
#include <utils/page.h>
#include <clock/clock.h>
#include "vnode.h"
#define verbose 5
#include <assert.h>
#include <sys/panic.h>

extern _cpio_archive[];

extern struct oft_entry of_table[MAX_OPEN_FILE];

struct PCB *curproc;

static struct PCB *PCB_table[MAX_PROCESSES];
static unsigned int PCB_end_time[MAX_PROCESSES];

int process_new(char *app_name, seL4_CPtr fault_ep, int parent_pid) {
    /* These required for loading program sections */
    char *elf_base;
    unsigned long elf_size;
    elf_base = cpio_get_file(_cpio_archive, app_name, &elf_size);
    if (elf_base == NULL) {
        return -1;
    }

    int id = -1;
    unsigned int end_time = UINT_MAX;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (PCB_table[i] != NULL) continue;
        if (PCB_end_time[i] < end_time) {
            id = i;
            end_time = PCB_end_time[i];
        }
    }

    if (id != -1) {
        conditional_panic(id == -1, "Max processes\n");
        /* return -1; */
    }

    struct PCB *proc = malloc(sizeof(struct PCB));
    if (proc == NULL) {
        conditional_panic(proc == NULL, "Out of memory for PCB\n");
        /* return -1; */
    }

    /* Set first proc as curproc */
    if (curproc == NULL) {
        curproc = proc;
    }

    /* Set proc */
    PCB_table[id] = proc;
    proc->app_name = malloc(strlen(app_name));
    strcpy(proc->app_name, app_name);
    proc->stime = time_stamp() / 1000;
    proc->pid = id;
    proc->wait = PROCESS_WAIT_NONE;
    proc->coroutine_id = -1;
    proc->parent = parent_pid;

    int err;

    seL4_CPtr user_ep_cap;

    /* These required for setting up the TCB */
    seL4_UserContext context;

    proc->addrspace = as_new();

    /* Create a VSpace */
    proc->vroot_addr = ut_alloc(seL4_PageDirBits);
    conditional_panic(!proc->vroot_addr,
            "No memory for new Page Directory");
    err = cspace_ut_retype_addr(proc->vroot_addr,
            seL4_ARM_PageDirectoryObject,
            seL4_PageDirBits,
            cur_cspace,
            &proc->vroot);
    conditional_panic(err, "Failed to allocate page directory cap for client");

    /* Create a simple 1 level CSpace */
    proc->croot = cspace_create(1);
    assert(proc->croot != NULL);

    /* IPC buffer region */
    err = as_define_region(proc->addrspace,
            PROCESS_IPC_BUFFER,
            (1 << seL4_PageBits),
            seL4_AllRights);
    conditional_panic(err, "Could not define IPC buffer region");

    /* Create an IPC buffer */
    err = sos_map_page(PROCESS_IPC_BUFFER,
            &proc->ipc_buffer_addr, proc);
    proc->ipc_buffer_cap = get_cap(proc->ipc_buffer_addr);
    conditional_panic(err, "No memory for ipc buffer\n");

    /* Copy the fault endpoint to the user app to enable IPC */
    user_ep_cap = cspace_mint_cap(proc->croot,
            cur_cspace,
            fault_ep,
            seL4_AllRights,
            seL4_CapData_Badge_new(id));

    /* should be the first slot in the space, hack I know */
    assert(user_ep_cap == 1);
    assert(user_ep_cap == USER_EP_CAP);

    /* Create a new TCB object */
    proc->tcb_addr = ut_alloc(seL4_TCBBits);
    conditional_panic(!proc->tcb_addr, "No memory for new TCB");
    err = cspace_ut_retype_addr(proc->tcb_addr,
            seL4_TCBObject,
            seL4_TCBBits,
            cur_cspace,
            &proc->tcb_cap);
    conditional_panic(err, "Failed to create TCB");

    /* Configure the TCB */
    err = seL4_TCB_Configure(proc->tcb_cap, user_ep_cap, APP_PRIORITY,
            proc->croot->root_cnode, seL4_NilData,
            proc->vroot, seL4_NilData, PROCESS_IPC_BUFFER,
            proc->ipc_buffer_cap);
    conditional_panic(err, "Unable to configure new TCB");


    /* parse the cpio image */
    dprintf(1, "\nStarting \"%s\"...\n", app_name);

    /* load the elf image */
    err = elf_load(proc->vroot, proc, elf_base);
    conditional_panic(err, "Failed to load elf image");

    /* Heap region */
    err = as_define_region(proc->addrspace,
            PROCESS_HEAP_START,
            0,
            seL4_AllRights);
    conditional_panic(err, "Could not define heap region");

    /* Stack region */
    err = as_define_region(proc->addrspace,
            PROCESS_STACK_BOT,
            PROCESS_STACK_TOP - PROCESS_STACK_BOT,
            seL4_AllRights);
    conditional_panic(err, "Could not define stack region");

    /* Start the new process */
    memset(&context, 0, sizeof(context));
    context.pc = elf_getEntryPoint(elf_base);
    context.sp = PROCESS_STACK_TOP;
    seL4_TCB_WriteRegisters(proc->tcb_cap, 1, 0, 2, &context);

    return id;
}

int process_new_other(char *app_name, seL4_CPtr fault_ep, int parent_pid) {
    /* These required for loading program sections */
    char *elf_base = malloc(PAGE_SIZE_4K);
    unsigned long elf_size;
    struct vnode *vn;
    int err = vfs_open(app_name,FM_READ,&vn);
    if (err) return -1;

    struct uio uio = {
        .uaddr = NULL,
        .vaddr = elf_base,
        .size = PAGE_SIZE_4K,
        .remaining = PAGE_SIZE_4K,
        .offset = 0
    };
    err = vn->ops->vop_read(vn,&uio);
    conditional_panic(err,"fail to load excutable file header from nfs");
    
    //elf_base = cpio_get_file(_cpio_archive, app_name, &elf_size);
    //if (elf_base == NULL) {
      //  return -1;
    //}

    int id = -1;
    unsigned int end_time = UINT_MAX;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (PCB_table[i] != NULL) continue;
        if (PCB_end_time[i] < end_time) {
            id = i;
            end_time = PCB_end_time[i];
        }
    }

    if (id != -1) {
        conditional_panic(id == -1, "Max processes\n");
        /* return -1; */
    }

    struct PCB *proc = malloc(sizeof(struct PCB));
    if (proc == NULL) {
        conditional_panic(proc == NULL, "Out of memory for PCB\n");
        /* return -1; */
    }

    /* Set first proc as curproc */
    if (curproc == NULL) {
        curproc = proc;
    }

    /* Set proc */
    PCB_table[id] = proc;
    proc->app_name = malloc(strlen(app_name));
    strcpy(proc->app_name, app_name);
    proc->stime = time_stamp() / 1000;
    proc->pid = id;
    proc->wait = PROCESS_WAIT_NONE;
    proc->coroutine_id = -1;
    proc->parent = parent_pid;


    seL4_CPtr user_ep_cap;

    /* These required for setting up the TCB */
    seL4_UserContext context;

    proc->addrspace = as_new();

    /* Create a VSpace */
    proc->vroot_addr = ut_alloc(seL4_PageDirBits);
    conditional_panic(!proc->vroot_addr,
            "No memory for new Page Directory");
    err = cspace_ut_retype_addr(proc->vroot_addr,
            seL4_ARM_PageDirectoryObject,
            seL4_PageDirBits,
            cur_cspace,
            &proc->vroot);
    conditional_panic(err, "Failed to allocate page directory cap for client");

    /* Create a simple 1 level CSpace */
    proc->croot = cspace_create(1);
    assert(proc->croot != NULL);

    /* IPC buffer region */
    err = as_define_region(proc->addrspace,
            PROCESS_IPC_BUFFER,
            (1 << seL4_PageBits),
            seL4_AllRights);
    conditional_panic(err, "Could not define IPC buffer region");

    /* Create an IPC buffer */
    err = sos_map_page(PROCESS_IPC_BUFFER,
            &proc->ipc_buffer_addr, proc);
    proc->ipc_buffer_cap = get_cap(proc->ipc_buffer_addr);
    conditional_panic(err, "No memory for ipc buffer\n");

    /* Copy the fault endpoint to the user app to enable IPC */
    user_ep_cap = cspace_mint_cap(proc->croot,
            cur_cspace,
            fault_ep,
            seL4_AllRights,
            seL4_CapData_Badge_new(id));

    /* should be the first slot in the space, hack I know */
    assert(user_ep_cap == 1);
    assert(user_ep_cap == USER_EP_CAP);

    /* Create a new TCB object */
    proc->tcb_addr = ut_alloc(seL4_TCBBits);
    conditional_panic(!proc->tcb_addr, "No memory for new TCB");
    err = cspace_ut_retype_addr(proc->tcb_addr,
            seL4_TCBObject,
            seL4_TCBBits,
            cur_cspace,
            &proc->tcb_cap);
    conditional_panic(err, "Failed to create TCB");

    /* Configure the TCB */
    err = seL4_TCB_Configure(proc->tcb_cap, user_ep_cap, APP_PRIORITY,
            proc->croot->root_cnode, seL4_NilData,
            proc->vroot, seL4_NilData, PROCESS_IPC_BUFFER,
            proc->ipc_buffer_cap);
    conditional_panic(err, "Unable to configure new TCB");


    /* parse the cpio image */
    dprintf(1, "\nStarting \"%s\"...\n", app_name);

    /* load the elf image */
    err = elf_load(proc->vroot, proc, elf_base,vn);
    conditional_panic(err, "Failed to load elf image");

    /* Heap region */
    err = as_define_region(proc->addrspace,
            PROCESS_HEAP_START,
            0,
            seL4_AllRights);
    conditional_panic(err, "Could not define heap region");

    /* Stack region */
    err = as_define_region(proc->addrspace,
            PROCESS_STACK_BOT,
            PROCESS_STACK_TOP - PROCESS_STACK_BOT,
            seL4_AllRights);
    conditional_panic(err, "Could not define stack region");

    /* Start the new process */
    memset(&context, 0, sizeof(context));
    context.pc = elf_getEntryPoint(elf_base);
    context.sp = PROCESS_STACK_TOP;
    seL4_TCB_WriteRegisters(proc->tcb_cap, 1, 0, 2, &context);
    free(elf_base);
    vfs_close(vn,FM_READ); 
    return id;
}





int process_destroy(pid_t pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) return -1;

    struct PCB *pcb = PCB_table[pid];
    if (pcb == NULL) return -1;

    /* Addrspace */
    as_destroy(pcb->addrspace);

    /* TCB */
    cspace_delete_cap(cur_cspace, pcb->tcb_cap);
    ut_free(pcb->tcb_addr, seL4_TCBBits);

    /* VSpace */
    cspace_delete_cap(cur_cspace, pcb->vroot);
    ut_free(pcb->vroot_addr, seL4_PageDirBits);

    /* CSpace */
    cspace_destroy(pcb->croot);

    /* PCB */
    if (pcb->coroutine_id != -1) {
        set_cleanup_coroutine(pcb->coroutine_id);
    }
    free(pcb->app_name);
    free(pcb);

    PCB_table[pid] = NULL;
    PCB_end_time[pid] = time_stamp() / 1000;

    return 0;
}

struct PCB *process_status(pid_t pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) return NULL;
    return PCB_table[pid];
}
