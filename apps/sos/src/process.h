#ifndef _PROCESS_H_
#define _PROCESS_H_

#include <cspace/cspace.h>
#include <sos.h>

#define TTY_NAME             CONFIG_SOS_STARTUP_APP
#define APP_PRIORITY         (0)
/* #define TTY_EP_BADGE         (101) */

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP          (1)

#define MAX_PROCESSES 32

#define PROCESS_WAIT_NONE -2
#define PROCESS_WAIT_ANY -1

#define PROCESS_STATUS_NOT_BUSY 0
#define PROCESS_STATUS_BUSY 1
#define PROCESS_STATUS_SELF_DESTRUCT 2

struct PCB {
    seL4_Word tcb_addr;
    seL4_TCB tcb_cap;

    seL4_Word vroot_addr;
    seL4_ARM_PageDirectory vroot;

    seL4_Word ipc_buffer_addr;
    seL4_CPtr ipc_buffer_cap;

    cspace_t *croot;

    char *app_name;
    unsigned int stime;

    int status;
    int pid;
    int wait;
    int coroutine_id;
    int parent;	

    struct app_addrspace *addrspace;
};

int is_still_valid_proc(pid_t pid, unsigned int stime);
int process_new_cpio(char* app_name, seL4_CPtr fault_ep, int parent_pid);
int process_new(char* app_name, seL4_CPtr fault_ep, int parent_pid);
int process_destroy(pid_t pid);
void process_management_init();
struct PCB *process_status(pid_t pid);

#endif
