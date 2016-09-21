#include <cspace/cspace.h>
#include <alloca.h>
#include <sys/panic.h>
#include <utils/page.h>

#include "coroutine.h"
#include "frametable.h"

#define NUM_COROUTINES 8

/* jmp_buf for syscall loop */
extern jmp_buf syscall_loop_entry;

int curr_coroutine_id = 0;

/* Number of current tasks */
static int num_tasks = 0;
/* Next task to resume to */
static int next_resume_id = -1;

static int start_index = 0;

/* Coroutine slots */
static jmp_buf coroutines[NUM_COROUTINES];
/* List of free coroutine slots */
static int free_list[NUM_COROUTINES];

/* Slots for storing args passed to callback */
static seL4_Word routine_args[NUM_COROUTINES][5];
static char *routine_frames[NUM_COROUTINES];

void coroutine_init() {
    int err;
    for (int i = 0; i < NUM_COROUTINES; i++) {
        free_list[i] = 1;
        err = unswappable_alloc((seL4_Word *) &routine_frames[i]);
        conditional_panic(err, "Could not initialise coroutines\n");
    }
}

yield() {
    int id = setjmp(coroutines[curr_coroutine_id]);
    if (id == 0) {
        /* First time */
        longjmp(syscall_loop_entry, 1);
    } else {
        /* Returning to coroutine's function */
        return;
    }

    /* Never reached */
}



void resume() {
    if (next_resume_id != -1) {
        /* Resume back to coroutine */
        int resume_id = next_resume_id;
        curr_coroutine_id = resume_id;
        next_resume_id = -1;

        longjmp(coroutines[resume_id], 1);
    }

    /* Nothing to resume to */
    return;
}

void set_resume(int id) {
    next_resume_id = id;
}

int start_coroutine(void (*task)(seL4_Word badge, int num_args),
                    jmp_buf return_entry,
                    seL4_Word badge, int num_args) {
    /* Check reached max coroutines */
    if (num_tasks == NUM_COROUTINES) return 1;

    num_tasks++;

    /* Find free slot */
    int task_id = start_index;
    while (free_list[task_id] == 0) {
        task_id++;
    }
    free_list[task_id] = 0;
    curr_coroutine_id = task_id;

    /* Allocate new stack frame */
    char *sptr = routine_frames[curr_coroutine_id];

    /* Clean frame before using */
    memset(PAGE_ALIGN_4K((seL4_Word) sptr), 0, PAGE_SIZE_4K);
    
    /* Move stack ptr up by a frame (since stack grows down) */
    sptr += PAGE_SIZE_4K;

    /* Add stuff to the new stack */
    void *sptr_new = sptr;
    sptr -= sizeof(void *);
    *sptr = sptr_new;

    seL4_Word badge_new = badge;
    sptr -= sizeof(seL4_Word);
    *sptr = badge;

    int num_args_new = num_args;
    sptr -= sizeof(int);
    *sptr = num_args;

    /* Change to new sp */
    asm volatile("mov sp, %[newsp]" : : [newsp] "r" (sptr) : "sp");

    /* Run task */
    task(badge_new, num_args_new);

    /* Task finished */
    free_list[task_id] = 1;
    start_index = task_id;
    num_tasks--;

    /* Return to main loop */
    longjmp(return_entry, 1);

    /* Never reached */
}

seL4_Word get_routine_arg(int id, int i) {
    return routine_args[id][i];
}

void set_routine_arg(int id, int i, seL4_Word arg) {
    routine_args[id][i] = arg;
}


