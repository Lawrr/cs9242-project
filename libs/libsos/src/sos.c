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

#define STDOUT_FD 1

#define SOS_WRITE_SYSCALL 0
#define SOS_READ_SYSCALL 1
#define SOS_OPEN_SYSCALL 2
#define SOS_CLOSE_SYSCALL 3
#define SOS_BRK_SYSCALL 4
#define SOS_USLEEP_SYSCALL 5
#define SOS_TIME_STAMP_SYSCALL 6
#define SOS_GETDIRENT_SYSCALL 7
#define SOS_STAT_SYSCALL 8
#define SOS_PROCESS_CREATE_SYSCALL 9
#define SOS_PROCESS_DELETE_SYSCALL 10
#define SOS_PROCESS_ID_SYSCALL 11
#define SOS_PROCESS_WAIT_SYSCALL 12
#define SOS_PROCESS_STATUS_SYSCALL 13

int sos_sys_open(const char *path, fmode_t mode) {
    int numRegs = 3;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_OPEN_SYSCALL);
    /* Set file name pointer */
    seL4_SetMR(1, (seL4_Word) path);
    /* Set open mode information */
    seL4_SetMR(2, mode);

    seL4_Call(SOS_IPC_EP_CAP, tag);

    /* Return fd / err */
    return seL4_GetMR(0);
}

int sos_sys_close(int file) {
    int numRegs = 2;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_CLOSE_SYSCALL);
    /* Set file descriptor */
    seL4_SetMR(1, file);

    seL4_Call(SOS_IPC_EP_CAP, tag);

    /* Return error code */
    return seL4_GetMR(0);
}

int sos_sys_read(int file, char *buf, size_t nbyte) {
    int numRegs = 4;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_READ_SYSCALL);
    /* Set file descriptor */
    seL4_SetMR(1, file);
    /* Set buf addr */
    seL4_SetMR(2, (seL4_Word) buf);
    /* Set num bytes */
    seL4_SetMR(3, nbyte);

    seL4_Call(SOS_IPC_EP_CAP, tag);

    /* Return number of bytes read */
    return seL4_GetMR(0);
}

int sos_sys_write(int file, const char *buf, size_t nbyte) {
    int numRegs = 4;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_WRITE_SYSCALL);
    /* Set file descriptor */
    seL4_SetMR(1, file);
    /* Set buf addr */
    seL4_SetMR(2, (seL4_Word) buf);
    /* Set num bytes */
    seL4_SetMR(3, nbyte);

    seL4_Call(SOS_IPC_EP_CAP, tag);

    /* Return number of bytes written */
    return seL4_GetMR(0);
}

int sos_getdirent(int pos, char *name, size_t nbyte) {
    int numRegs = 4;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_GETDIRENT_SYSCALL);
    /* Set pos */
    seL4_SetMR(1, pos);
    /* Set name */
    seL4_SetMR(2, (seL4_Word) name);
    /* Set num bytes */
    seL4_SetMR(3, nbyte);

    seL4_Call(SOS_IPC_EP_CAP, tag);

    return seL4_GetMR(0);
}

int sos_stat(const char *path, sos_stat_t *buf) {
    int numRegs = 3;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_STAT_SYSCALL);
    /* Set path */
    seL4_SetMR(1, (seL4_Word) path);
    /* Set buf */
    seL4_SetMR(2, (seL4_Word) buf);

    seL4_Call(SOS_IPC_EP_CAP, tag);

    return seL4_GetMR(0);
}

pid_t sos_process_create(const char *path) {
    //printf("System call not yet implemented\n");
    int numRegs = 2;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_PROCESS_CREATE_SYSCALL);
    /* Set file name pointer */
    seL4_SetMR(1, (seL4_Word) path);

    seL4_Call(SOS_IPC_EP_CAP, tag);

    /* Return pid / err */
    return seL4_GetMR(0);
}

int sos_process_delete(pid_t pid) {
    int numRegs = 2;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_PROCESS_DELETE_SYSCALL);
    /* Set pid */
    seL4_SetMR(1, (seL4_Word) pid);

    seL4_Call(SOS_IPC_EP_CAP, tag);

    /* Return / err */
    return seL4_GetMR(0);
}

pid_t sos_my_id(void) {
    int numRegs = 1;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_PROCESS_ID_SYSCALL);

    seL4_Call(SOS_IPC_EP_CAP, tag);

    /* Return / err */
    return seL4_GetMR(0);
}

int sos_process_status(sos_process_t *processes, unsigned max) {
    int numRegs = 3;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_PROCESS_STATUS_SYSCALL);
    /* Set processes pointer */
    seL4_SetMR(1, (seL4_Word) processes);

    /* Set maximum number */
    seL4_SetMR(2, (seL4_Word) max);

    seL4_Call(SOS_IPC_EP_CAP, tag);

    /* Return fd / err */
    return seL4_GetMR(0);
}

pid_t sos_process_wait(pid_t pid) {
    int numRegs = 2;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_PROCESS_WAIT_SYSCALL);

    /*set pid */
    seL4_SetMR(1, (seL4_Word) pid);


    seL4_Call(SOS_IPC_EP_CAP, tag);

    /* Return pid / err */
    return seL4_GetMR(0);
}

void sos_sys_usleep(int msec) {
    int numRegs = 2;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_USLEEP_SYSCALL);
    /* Set sleep duration */
    seL4_SetMR(1, msec);

    seL4_Call(SOS_IPC_EP_CAP, tag);
}

int64_t sos_sys_time_stamp(void) {
    int numRegs = 1;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_TIME_STAMP_SYSCALL);

    seL4_Call(SOS_IPC_EP_CAP, tag);

    /* Return timestamp */
    return seL4_GetMR(0);
}

int sos_brk(uintptr_t newbrk) {
    int numRegs = 2;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(seL4_NoFault, 0, 0, numRegs);
    seL4_SetTag(tag);

    /* Set syscall number */
    seL4_SetMR(0, SOS_BRK_SYSCALL);
    /* Set brk point */
    seL4_SetMR(1, newbrk);

    seL4_Call(SOS_IPC_EP_CAP, tag);

    /* Return error code */
    return seL4_GetMR(0);
}

size_t sos_write(void *vData, size_t count) {
    return sos_sys_write(STDOUT_FD, vData, count);
}

size_t sos_read(void *vData, size_t count) {
    // return sos_sys_read(std_input, vData, count);
    return 0;
}
