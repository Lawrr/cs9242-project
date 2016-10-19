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
    int pages = 50;
    int a[pages][4096];

    for (int i = 0; i < pages; i++) {
        for (int j = 0; j < 4096; j++) {
            a[i][j]= i * 4096 + j;
        }
        printf("a[%d][0] = %d\n", i, a[i][0]);
        printf("a[%d][2048] = %d\n", i, a[i][2048]);
        printf("a[%d][4095] = %d\n", i, a[i][4095]);
    }

    printf("#################################\n");
    printf("Checking values...\n");
    printf("#################################\n");

    for (int i = 0; i < pages; i++) {
        printf("Checking: a[%d][0] = %d - Should be: %d\n", i, a[i][0], i * 4096);
        assert(a[i][0] == i * 4096);
        printf("Checking: a[%d][2048] = %d - Should be: %d\n", i, a[i][2048], i * 4096 + 2048);
        assert(a[i][2048] == i * 4096 + 2048);
        printf("Checking: a[%d][4095] = %d - Should be: %d\n", i, a[i][4095], i * 4096 + 4095);
        assert(a[i][4095] == i * 4096 + 4095);
    }

    return 0;
}

static sos_stat_t sbuf;

static void prstat(const char *name) {
    /* print out stat buf */
    printf("%c%c%c%c 0x%06x 0x%lx 0x%06lx %s\n",
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
            printf("dirent(%d) failed: %d\n", i, r);
            break;
        } else if (!r) {
            break;
        }
        r = sos_stat(buf, &sbuf);
        if (r < 0) {
            printf("stat(%s) failed: %d\n", buf, r);
            break;
        }
        prstat(buf);
        i++;
    }
}

void write_read() {
    char filename[100];
    int sz;
    sz = snprintf(filename, 100, "%d.test", sos_my_id());
    assert(sz >= 6 && sz < 100);
    printf("Using file for write: %s\n", &filename);
    int fd_write = open(filename, O_WRONLY);
    printf("Starting write\n");
    for (int i = 0; i < 1000; i++) {
        char str[4];
        sz = snprintf(str, 4, "%d", i % 10);
        assert(sz >= 1 && sz < 4);
        int res = sos_sys_write(fd_write, str, strlen(str));
        assert(res == strlen(str));
    }
    printf("Using file for read: %s\n", &filename);
    int fd_read = open(filename, O_RDONLY);
    printf("Starting read\n");
    char buff[5];
    for (int i = 0; i < 1000; i++) {
        int res = sos_sys_read(fd_read, &buff, 1);
        assert(res == 1);
        assert(buff[0] == i % 10);
    }
    close(fd_write);
    close(fd_read);
}

int main(void) {
    printf("[%d] Hello world!\n", sos_my_id());
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
