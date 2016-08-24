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
#define SYSCALL_ENDPOINT_SLOT  (1)

int sos_sys_open(const char *path, fmode_t mode) {
    // TODO for M4
    assert(!"You need to implement this");
    return -1;
}

int sos_sys_close(int file) {
    // TODO for M4
    printf("System call not yet implemented\n");
    return -1;
}

int sos_sys_read(int file, char *buf, size_t nbyte) {
    // TODO for M4
    assert(!"You need to implement this");
    return -1;
}

int sos_sys_write(int file, const char *buf, size_t nbyte) {
    // TODO for M4
    assert(!"You need to implement this");
    return -1;
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
    // TODO for M4
    assert(!"You need to implement this");
}

int64_t sos_sys_time_stamp(void) {
    // TODO for M4
    assert(!"You need to implement this");
    return -1;
}

size_t sos_write(void *vData, size_t count) {
    int data_sent = 0;

    // Multiple writes if it exceeds seL4_MsgMaxLength
    while (data_sent < count) {
        // Data length (num chars)
        int data_length = count - data_sent;
        // Args: syscall num and data length
        int num_args = 2;
        // args + # registers for data to write (min 1)
        int length = num_args + 1 + ((sizeof(char) * data_length - 1) / sizeof(seL4_Word));

        // Check max length
        if (length > seL4_MsgMaxLength) {
            length = seL4_MsgMaxLength;
            // Calculate message length minus syscall num and data length registers
            data_length = (length - num_args) * sizeof(seL4_Word);
        }

        // Set up message
        seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, length);
        seL4_SetTag(tag);

        // Set syscall number
        seL4_SetMR(0, 0);
        // Set data length
        seL4_SetMR(1, data_length);

        // Copy the data to write to message registers
        memcpy(&seL4_GetIPCBuffer()->msg[num_args], vData + data_sent, data_length);

        // Send message
        seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);

        data_sent += seL4_GetMR(0);
    }

    return data_sent;
}

size_t sos_read(void *vData, size_t count) {
    //implement this to use your syscall
    return 0;
}
