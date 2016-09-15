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

/* Coroutine slots */
static jmp_buf coroutines[NUM_COROUTINES];
/* List of free coroutine slots */
static int free_list[NUM_COROUTINES];

/* Slots for storing args passed to callback */
static seL4_Word routine_args[NUM_COROUTINES][5];
static char *routine_frames[NUM_COROUTINES];
print_jmpbuf(jmp_buf t){
   for (int i = 0;i < 32; i++){
      printf("jmp_buf[%d]=%d\n",i,t[i]);
   }
}

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
//    print_jmpbuf(syscall_loop_entry); 
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

int start_coroutine(void (*task)(seL4_Word badge, int num_args), void *data) {
    /* Check reached max coroutines */
    if (num_tasks == NUM_COROUTINES) return 1;

    num_tasks++;

    /* Find free slot */
    int task_id = 0;
    while (free_list[task_id] == 0) {
        task_id++;
    }
    free_list[task_id] = 0;
    curr_coroutine_id = task_id;

    /* Allocate new stack frame */
    char *sptr = routine_frames[curr_coroutine_id];
    
    /* Move stack ptr up by a frame (since stack grows down) */
    sptr += 4096;

    /* Add stuff to the new stack */
    void *sptr_new = sptr;
    sptr -= sizeof(void *);
    *sptr = sptr_new;

    void *data_new = data;
    sptr -= sizeof(void *);
    *sptr = (seL4_Word) data_new;

    /* Change to new sp */
    asm volatile("mov sp, %[newsp]" : : [newsp] "r" (sptr) : "sp");

    /* Run task */
    task(((seL4_Word *) data_new)[0], ((seL4_Word *) data_new)[1]);

    /* Task finished - clean up */
    memset(PAGE_ALIGN_4K((seL4_Word) sptr_new), 0, PAGE_SIZE_4K);
    free_list[task_id] = 1;
    num_tasks--;

    /* Return to main loop */
    longjmp(syscall_loop_entry, 1);

    /* Never reached */
}

seL4_Word get_routine_arg(int id, int i) {
    return routine_args[id][i];
}

void set_routine_arg(int id, int i, seL4_Word arg) {
    routine_args[id][i] = arg;
}


