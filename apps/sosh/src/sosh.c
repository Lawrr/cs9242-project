/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

/* Simple shell to run on SOS */

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

#define BUF_SIZ    6144
#define MAX_ARGS   32

static int in;
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

static int cat(int argc, char **argv) {
    int fd;
    char buf[BUF_SIZ];
    int num_read, stdout_fd, num_written = 0;


    if (argc != 2) {
        printf("Usage: cat filename\n");
        return 1;
    }

    printf("<%s>\n", argv[1]);

    fd = open(argv[1], O_RDONLY);
    stdout_fd = open("console", O_WRONLY);

    assert(fd >= 0);

    while ((num_read = read(fd, buf, BUF_SIZ)) > 0)
        num_written = write(stdout_fd, buf, num_read);

    close(stdout_fd);

    if (num_read == -1 || num_written == -1) {
        printf("error on write\n");
        return 1;
    }

    return 0;
}

static int cp(int argc, char **argv) {
    int fd, fd_out;
    char *file1, *file2;
    char buf[BUF_SIZ];
    int num_read, num_written = 0;

    if (argc != 3) {
        printf("Usage: cp from to\n");
        return 1;
    }

    file1 = argv[1];
    file2 = argv[2];

    fd = open(file1, O_RDONLY);
    fd_out = open(file2, O_WRONLY);

    assert(fd >= 0);

    while ((num_read = read(fd, buf, BUF_SIZ)) > 0)
        num_written = write(fd_out, buf, num_read);

    if (num_read == -1 || num_written == -1) {
        printf("error on cp\n");
        return 1;
    }

    return 0;
}

#define MAX_PROCESSES 10

static int ps(int argc, char **argv) {
    sos_process_t *process;
    int i, processes;

    process = malloc(MAX_PROCESSES * sizeof(*process));

    if (process == NULL) {
        printf("%s: out of memory\n", argv[0]);
        return 1;
    }

    processes = sos_process_status(process, MAX_PROCESSES);

    printf("TID SIZE   STIME   CTIME COMMAND\n");

    for (i = 0; i < processes; i++) {
        printf("%3x %4x %7d %s\n", process[i].pid, process[i].size,
                process[i].stime, process[i].command);
    }

    free(process);

    return 0;
}

static int exec(int argc, char **argv) {
    pid_t pid;
    int r;
    int bg = 0;

    if (argc < 2 || (argc > 2 && argv[2][0] != '&')) {
        printf("Usage: exec filename [&]\n");
        return 1;
    }

    if ((argc > 2) && (argv[2][0] == '&')) {
        bg = 1;
    }

    if (bg == 0) {
        r = close(in);
        assert(r == 0);
    }

    pid = sos_process_create(argv[1]);
    if (pid >= 0) {
        printf("Child pid=%d\n", pid);
        if (bg == 0) {
            sos_process_wait(pid);
        }
    } else {
        printf("Failed!\n");
    }
    if (bg == 0) {
        in = open("console", O_RDONLY);
        assert(in >= 0);
    }
    return 0;
}

static int dir(int argc, char **argv) {
    int i = 0, r;
    char buf[BUF_SIZ];

    if (argc > 2) {
        printf("usage: %s [file]\n", argv[0]);
        return 1;
    }

    if (argc == 2) {
        r = sos_stat(argv[1], &sbuf);
        if (r < 0) {
            printf("stat(%s) failed: %d\n", argv[1], r);
            return 0;
        }
        prstat(argv[1]);
        return 0;
    }

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
    return 0;
}

static int second_sleep(int argc,char *argv[]) {
    if (argc != 2) {
        printf("Usage %s seconds\n", argv[0]);
        return 1;
    }
    sleep(atoi(argv[1]));
    return 0;
}

static int milli_sleep(int argc,char *argv[]) {
    struct timespec tv;
    uint64_t nanos;
    if (argc != 2) {
        printf("Usage %s milliseconds\n", argv[0]);
        return 1;
    }
    nanos = (uint64_t)atoi(argv[1]) * NS_IN_MS;
    /* Get whole seconds */
    tv.tv_sec = nanos / NS_IN_S;
    /* Get nanos remaining */
    tv.tv_nsec = nanos % NS_IN_S;
    nanosleep(&tv, NULL);
    return 0;
}

static int second_time(int argc, char *argv[]) {
    printf("%d seconds since boot\n", (int)time(NULL));
    return 0;
}

static int micro_time(int argc, char *argv[]) {
    struct timeval time;
    gettimeofday(&time, NULL);
    uint64_t micros = (uint64_t)time.tv_sec * US_IN_S + (uint64_t)time.tv_usec;
    printf("%llu microseconds since boot\n", micros);
    return 0;
}

static int kill(int argc, char *argv[]) {
    pid_t pid;
    if (argc != 2) {
        printf("Usage: kill pid\n");
        return 1;
    }

    pid = atoi(argv[1]);
    return sos_process_delete(pid);
}

struct command {
    char *name;
    int (*command)(int argc, char **argv);
};

#define SMALL_BUF_SZ 2

char test_str[] = "Basic test string for read/write";
char small_buf[SMALL_BUF_SZ];

void test_buffers(int console_fd) {
    /* test a small string from the code segment */
    int result = sos_sys_write(console_fd, test_str, strlen(test_str));
    assert(result == strlen(test_str));

    /* test reading to a small buffer */
    result = sos_sys_read(console_fd, small_buf, SMALL_BUF_SZ);
    /* make sure you type in at least SMALL_BUF_SZ */
    assert(result == SMALL_BUF_SZ);

    /* test a reading into a large on-stack buffer */
    char stack_buf[BUF_SIZ];
    /* for this test you'll need to paste a lot of data into 
       the console, without newlines */
    result = sos_sys_read(console_fd, &stack_buf, BUF_SIZ);
    assert(result == BUF_SIZ);

    result = sos_sys_write(console_fd, &stack_buf, BUF_SIZ);
    assert(result == BUF_SIZ);

    /* this call to malloc should trigger an sbrk */
    char *heap_buf = malloc(BUF_SIZ);
    assert(heap_buf != NULL);

    /* for this test you'll need to paste a lot of data into 
       the console, without newlines */
    result = sos_sys_read(console_fd, &heap_buf, BUF_SIZ);
    assert(result == BUF_SIZ);

    result = sos_sys_write(console_fd, &heap_buf, BUF_SIZ);
    assert(result == BUF_SIZ);

    /* try sleeping */
    for (int i = 0; i < 5; i++) {
        time_t prev_seconds = time(NULL);
        sleep(1);
        time_t next_seconds = time(NULL);
        assert(next_seconds > prev_seconds);
        printf("Tick\n");
    }
}

struct command commands[] = { { "dir", dir }, { "ls", dir }, { "cat", cat }, {
        "cp", cp }, { "ps", ps }, { "exec", exec }, {"sleep",second_sleep}, {"msleep",milli_sleep},
        {"time", second_time}, {"mtime", micro_time}, {"kill", kill} };

int main(void) {
    char buf[BUF_SIZ];
    char *argv[MAX_ARGS];
    int i, r, done, found, new, argc;
    char *bp, *p;

    int console_fd = open("console", O_RDWR);
    test_buffers(console_fd);
    sos_sys_close(console_fd);

    printf("[SOS Exiting]\n");
}
