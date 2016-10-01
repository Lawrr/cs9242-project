#ifndef _SOS_SYSCALL_H
#define _SOS_SYSCALL_H

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

#include <cspace/cspace.h>

void syscall_brk(seL4_CPtr reply_cap);

void syscall_usleep(seL4_CPtr reply_cap);

void syscall_time_stamp(seL4_CPtr reply_cap);

void syscall_write(seL4_CPtr reply_cap);

void syscall_read(seL4_CPtr reply_cap);

void syscall_open(seL4_CPtr reply_cap);

void syscall_close(seL4_CPtr reply_cap);

void syscall_getdirent(seL4_CPtr reply_cap);

void syscall_stat(seL4_CPtr reply_cap);

void syscall_process_create(seL4_CPtr reply_cap);

void syscall_process_delete(seL4_CPtr reply_cap, seL4_Word badge);

void syscall_process_id(seL4_CPtr reply_cap, seL4_Word badge);

void syscall_process_wait(seL4_CPtr reply_cap);

void syscall_process_status(seL4_CPtr reply_cap);
#endif
