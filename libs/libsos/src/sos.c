/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <stdarg.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sos.h>

#include <sel4/sel4.h>

/**
 * This is our system call endpoint cap, as defined by the root server
 */
//default value. May be changed by user if they close and reopen console
static int std_input = 0;
static int std_output = 1;
static int std_err = 1;

int sos_sys_open(const char *path, fmode_t mode) {
    
    int ret = -1;
    int numRegs = 3;
    //Reach max open file for an app

    //Set up message
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);
   
    //Set syscall number
    seL4_SetMR(0, 2);
   
    //set file name pointer
    seL4_SetMR(1, (seL4_Word)path);
    
    //Set open mode information
    seL4_SetMR(2, mode);
    
    seL4_Call(SOS_IPC_EP_CAP, tag);
    return seL4_GetMR(0);
}

int sos_sys_close(int file) {
    int numRegs = 2;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    //Set syscall number
    seL4_SetMR(0,3);
   


    //Set file descriptor
    seL4_SetMR(1,file);
    
    seL4_Call(SOS_IPC_EP_CAP,tag);
    return seL4_GetMR(0);
}


//Console can not be both readable and writable
int sos_sys_read(int file, char *buf, size_t nbyte) {

    //Set up message
    int numRegs = 4;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);
   
    //Set syscall number
    seL4_SetMR(0, 1);
   
    //set file descriptor 
    seL4_SetMR(1, file);
    
    //Set buf addr
    seL4_SetMR(2, (seL4_Word) buf);

    //set nbyte
    seL4_SetMR(3,nbyte);
    
    seL4_Call(SOS_IPC_EP_CAP, tag);
    return seL4_GetMR(0);
}

int sos_sys_write(int file, const char *buf, size_t nbyte) {
    int numRegs = 4;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);
   
    //Set syscall number
    seL4_SetMR(0, 0);
   
    //set file descriptor 
    seL4_SetMR(1, file);
    
    //Set buf addr
    seL4_SetMR(2, (seL4_Word) buf);

    //set nbyte
    seL4_SetMR(3,nbyte);
    
    seL4_Call(SOS_IPC_EP_CAP, tag);    
    return seL4_GetMR(0);
}

int sos_getdirent(int pos, char *name, size_t nbyte) {
    printf("System call not yet implemented\n");
    return -1;
}

int sos_stat(const char *path, sos_stat_t *buf) {
    printf("System call not yet implemented\n");
    return -1;
}

pid_t sos_process_create(const char *path) {
    printf("System call not yet implemented\n");
    return -1;
}

int sos_process_delete(pid_t pid) {
    printf("System call not yet implemented\n");
    return -1;
}

pid_t sos_my_id(void) {
    printf("System call not yet implemented\n");
    return -1;
}

int sos_process_status(sos_process_t *processes, unsigned max) {
    printf("System call not yet implemented\n");
    return -1;
}

pid_t sos_process_wait(pid_t pid) {
    printf("System call not yet implemented\n");
    return -1;
}

void sos_sys_usleep(int msec) {
    int numRegs = 2;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);
   
    //Set syscall number
    seL4_SetMR(0, 5);
    seL4_SetMR(1, msec);

    seL4_Call(SOS_IPC_EP_CAP, tag);    

    // TODO check if sleep succeeded
    int64_t timestamp = seL4_GetMR(0);
}

int64_t sos_sys_time_stamp(void) {
    int numRegs = 1;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);
   
    //Set syscall number
    seL4_SetMR(0, 6);

    seL4_Call(SOS_IPC_EP_CAP, tag);    

    int64_t timestamp = seL4_GetMR(0);
    return timestamp;
}

int sos_brk(uintptr_t newbrk) {
    int numRegs = 2;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);
   
    //Set syscall number
    seL4_SetMR(0, 4);
    seL4_SetMR(1, newbrk);

    seL4_Call(SOS_IPC_EP_CAP, tag);    

    int err = seL4_GetMR(0);
    return err;
}

size_t sos_write(void *vData, size_t count) {
    return sos_sys_write(std_output,vData,count);
}

size_t sos_read(void *vData, size_t count) {
    //implement this to use your syscall
    return sos_sys_read(std_input,vData,count);
}
