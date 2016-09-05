#include "coroutine.h"
#include <setjmp.h>
#include <cspace/cspace.h>
#include <alloca.h>
#include "frametable.h"
#define NUM_COROUTINES 8

extern jmp_buf syscall_loop_entry;

int curr_coroutine_id = 0;

static int num_tasks = 0;
static int next_yield_id = -1;

static jmp_buf coroutines[NUM_COROUTINES];
static int free_list[NUM_COROUTINES];
//slot for storing arguments passed to callback
static seL4_Word routine_arguments[NUM_COROUTINES][4];

void coroutine_init() {
    for (int i = 0; i < NUM_COROUTINES; i++) {
        free_list[i] = 1;
    }
}

yield() {
    int id = setjmp(coroutines[curr_coroutine_id]);

    if (id == 0) {
        longjmp(syscall_loop_entry, 1);
    } else {
        return;//when jump bakc use return value as err code
    }

    /* Never reached */
}

void resume() {
   if (next_yield_id != -1){
      int tar = next_yield_id;
      curr_coroutine_id = tar; 
      next_yield_id = -1;
      longjmp(coroutines[tar], 1);
   }
   return;
}


void set_resume(int id){
    next_yield_id = id; 
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
    curr_coroutine_id = task_id;
    
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

    longjmp(syscall_loop_entry, 1);

    /* Never reached */
}

seL4_Word get_routine_argument(int id,int i){
    return routine_arguments[id][i];
}

void set_routine_argument(int i,seL4_Word arg){
   routine_arguments[curr_coroutine_id][i] = arg;
}


