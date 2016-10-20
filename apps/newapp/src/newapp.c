/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <utils/time.h>

/* Your OS header file */
#include <sos.h>

#define BUF_SIZ    4000

void vm_fault() {
    pid_t id = sos_my_id();
    int pages = 50;
    int a[pages][4096];

    for (int i = 0; i < pages; i++) {
        for (int j = 0; j < 4096; j++) {
            a[i][j]= i * 4096 + j;
        }
        printf("[%d] a[%d][0] = %d\n", id, i, a[i][0]);
        printf("[%d] a[%d][2048] = %d\n", id, i, a[i][2048]);
        printf("[%d] a[%d][4095] = %d\n", id, i, a[i][4095]);
    }

    printf("[%d] #################################\n", id);
    printf("[%d] Checking values...\n", id);
    printf("[%d] #################################\n", id);

    for (int i = 0; i < pages; i++) {
        printf("[%d] Checking: a[%d][0] = %d - Should be: %d\n", id, i, a[i][0], i * 4096);
        assert(a[i][0] == i * 4096);
        printf("[%d] Checking: a[%d][2048] = %d - Should be: %d\n", id, i, a[i][2048], i * 4096 + 2048);
        assert(a[i][2048] == i * 4096 + 2048);
        printf("[%d] Checking: a[%d][4095] = %d - Should be: %d\n", id, i, a[i][4095], i * 4096 + 4095);
        assert(a[i][4095] == i * 4096 + 4095);
    }

    return 0;
}

static sos_stat_t sbuf;

static void prstat(const char *name) {
    /* print out stat buf */
    printf("[%d] %c%c%c%c 0x%06x 0x%lx 0x%06lx %s\n",
            sos_my_id(),
            sbuf.st_type == ST_SPECIAL ? 's' : '-',
            sbuf.st_fmode & FM_READ ? 'r' : '-',
            sbuf.st_fmode & FM_WRITE ? 'w' : '-',
            sbuf.st_fmode & FM_EXEC ? 'x' : '-', sbuf.st_size, sbuf.st_ctime,
            sbuf.st_atime, name);
}

void ls() {
    int i = 0, r;
    char buf[BUF_SIZ];
    while (1) {
        r = sos_getdirent(i, buf, BUF_SIZ);
        if (r < 0) {
            printf("[%d] dirent(%d) failed: %d\n", sos_my_id(), i, r);
            break;
        } else if (!r) {
            break;
        }
        r = sos_stat(buf, &sbuf);
        if (r < 0) {
            printf("[%d] stat(%s) failed: %d\n", sos_my_id(), buf, r);
            break;
        }
        prstat(buf);
        i++;
    }
}

void write_read() {
    pid_t id = sos_my_id();
    char filename[100];
    int sz;
    sz = snprintf(filename, 100, "%d.test", sos_my_id());
    assert(sz >= 6 && sz < 100);
    printf("[%d] Using file for write: %s\n", id, &filename);
    int fd_write = open(filename, O_WRONLY);
    assert(fd_write >= 0);
    printf("[%d] Starting write\n", id);
    for (int i = 0; i < 500; i++) {
        char str[4];
        sz = snprintf(str, 4, "%d", i % 10);
        assert(sz >= 1 && sz < 4);
        int res = sos_sys_write(fd_write, str, strlen(str));
        assert(res == strlen(str));
    }
    printf("[%d] Using file for read: %s\n", id, &filename);
    int fd_read = open(filename, O_RDONLY);
    assert(fd_read >= 0);
    printf("[%d] Starting read\n", id);
    char buff[5];
    for (int i = 0; i < 500; i++) {
        int res = sos_sys_read(fd_read, &buff, 1);
        assert(res == 1);
        assert(buff[0] - 48 == i % 10);
    }
    close(fd_write);
    close(fd_read);
}

#define MAX_PROCESSES 32

static int ps() {
    sos_process_t *process;
    int i, processes;

    process = malloc(MAX_PROCESSES * sizeof(*process));

    if (process == NULL) {
        printf("[%d] out of memory\n", sos_my_id());
        return 1;
    }

    processes = sos_process_status(process, MAX_PROCESSES);

    printf("TID SIZE   STIME   CTIME COMMAND\n");

    for (i = 0; i < processes; i++) {
        printf("[%d] %3x %4x %7d %s\n", sos_my_id(), process[i].pid, process[i].size,
                process[i].stime, process[i].command);
    }

    free(process);

    return 0;
}

int main(void) {
    printf("[%d] Hello world!\n", sos_my_id());

    if (sos_my_id() % 5 != 0) {
        printf("[%d] Creating child\n", sos_my_id());
        pid_t child = sos_process_create("newapp");
        assert(child >= 0);
        printf("[%d] Created child %d\n", sos_my_id(), child);
    }

    printf("[%d] Process list:\n", sos_my_id());
    ps();

    printf("[%d] Time is: %lld\n", sos_my_id(), sos_sys_time_stamp());
    printf("[%d] Trying some vm faults\n", sos_my_id());
    vm_fault();
    printf("[%d] Finished vm faults\n" ,sos_my_id());
    printf("[%d] Trying ls a lot of times\n", sos_my_id());
    ls();
    printf("[%d] ls done\n", sos_my_id());
    printf("[%d] Sleeping 3s...\n", sos_my_id());
    sleep(3);
    printf("[%d] Now trying writing and reading\n", sos_my_id());
    write_read();
    printf("[%d] Testing done\n", sos_my_id());

    return 0;
}
