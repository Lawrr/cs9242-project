#include "coroutine.h"
#include <setjmp.h>
#include <cspace/cspace.h>
#include <alloca.h>
#include "frametable.h"

#define NUM_COROUTINES 8

extern jmp_buf syscall_loop_entry;

static int curr_id = 0;
static int num_tasks = 0;
static int next_yield_id = 0;

static jmp_buf coroutines[NUM_COROUTINES];
static int free_list[NUM_COROUTINES];

void coroutine_init() {
    for (int i = 0; i < NUM_COROUTINES; i++) {
        free_list[i] = 1;
    }
}

void yield() {
    int id = setjmp(coroutines[curr_id]);

    if (id == 0) {
        longjmp(syscall_loop_entry, 1);
    } else {
        return;
    }

    /* Never reached */
}

void resume() {
    if (num_tasks == 0) return;

    uint32_t continue_id = next_yield_id;

    /* Find next to yield */
    do {
        next_yield_id = (next_yield_id + 1) % NUM_COROUTINES;
    } while (free_list[next_yield_id] == 1);

    longjmp(coroutines[continue_id], 1);

    /* Never reached */
}

int start_coroutine(void (*task)(seL4_Word badge, int num_args),
                    void *data) {
    if (num_tasks == NUM_COROUTINES) return 1;

    num_tasks++;

    /* Find free slot */
    int task_id = 0;
    while (free_list[task_id] == 0) {
        task_id++;
    }
    free_list[task_id] = 0;
    curr_id = task_id;

    /* Allocate new stack frame */
    char *sptr;
    int err = frame_alloc((seL4_Word *) &sptr);
    sptr += 4096;
    /* conditional_panic(err, "Could not allocate frame\n"); */

    //TODO memcpy instead?
    /* Add stuff to the new stack */
    void *sptr_new = sptr;
    sptr -= sizeof(void *);
    *sptr = sptr_new;

    void *data_new = data;
    sptr -= sizeof(void *);
    *sptr = (seL4_Word) data_new;

    /* Change to new sp */
    asm volatile("mov sp, %[newsp]" : : [newsp] "r" (sptr) : "sp");
    // TODO change stack limit register?

    /* Run task */
    task(((seL4_Word *) data_new)[0], ((seL4_Word *) data_new)[1]);

    frame_free(sptr_new);

    free_list[task_id] = 1;

    num_tasks--;

    printf("Task done\n");
    longjmp(syscall_loop_entry, 1);

    /* Never reached */
}
