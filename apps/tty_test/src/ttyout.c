/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

/****************************************************************************
 *
 *      $Id:  $
 *
 *      Description: Simple milestone 0 code.
 *      		     Libc will need sos_write & sos_read implemented.
 *
 *      Author:      Ben Leslie
 *
 ****************************************************************************/

#include <stdarg.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "ttyout.h"

#include <sel4/sel4.h>

void ttyout_init(void) {
    /* Perform any initialisation you require here */
}

static size_t sos_debug_print(const void *vData, size_t count) {
    size_t i;
    const char *realdata = vData;
    for (i = 0; i < count; i++)
        seL4_DebugPutChar(realdata[i]);
    return count;
}

size_t sos_write(void *vData, size_t count) {
    // Calculate message length
    // syscall num + data length + # registers for data to write (min 1)
    int length = 3 + ((sizeof(char) * count - 1) / sizeof(seL4_Word));

    // Set up message
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, length);
    seL4_SetTag(tag);

    // Set syscall number
    seL4_SetMR(0, 0);
    // Set data length
    seL4_SetMR(1, count);
    // Copy the data to write to message registers
    strcpy(&seL4_GetIPCBuffer()->msg[2], vData);

    // Send message
    seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);

    int bytes_written = seL4_GetMR(0);
    return bytes_written;
}

size_t sos_read(void *vData, size_t count) {
    //implement this to use your syscall
    return 0;
}

