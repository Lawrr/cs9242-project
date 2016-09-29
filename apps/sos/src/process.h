#ifndef _PROCESS_H_
#define _PROCESS_H_

#include <cspace/cspace.h>

#define TTY_NAME             CONFIG_SOS_STARTUP_APP
#define TTY_PRIORITY         (0)
#define TTY_EP_BADGE         (101)

/* This is the index where a clients syscall enpoint will
 * be stored in the clients cspace. */
#define USER_EP_CAP          (1)

struct PCB {
    seL4_Word tcb_addr;
    seL4_TCB tcb_cap;
  
    seL4_Word vroot_addr;
    seL4_ARM_PageDirectory vroot;
  
    seL4_Word ipc_buffer_addr;
    seL4_CPtr ipc_buffer_cap;
  
    cspace_t *croot;
  
    struct app_addrspace *addrspace;
};

int process_new(char* app_name, seL4_CPtr fault_ep); 
int process_destroy(seL4_Word pid);
#endif
