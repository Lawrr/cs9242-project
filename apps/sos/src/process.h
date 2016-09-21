#ifndef _PROCESS_H_
#define _PROCESS_H_

#include <cspace/cspace.h>
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
struct PCB tty_test_process;

struct PCB *curproc = &tty_test_process;

void start_process(char* app_name, seL4_CPtr fault_ep); 
#endif
