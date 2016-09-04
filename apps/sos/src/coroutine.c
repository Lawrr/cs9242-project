#include "coroutine.h"
#include <setjmp.h>
#include <cspace/cspace.h>
#include <alloca.h>
#include "frametable.h"
#define CO_STACK_SIZE 512
#define ROUTINE_NUM 7

static jmp_buf routine_list[ROUTINE_NUM];
static int free_list[ROUTINE_NUM];

void coroutine_init() {
    for (int i = 0; i < ROUTINE_NUM; i++) {
        free_list[i] = 1;
    }
}

extern jmp_buf syscall_loop_entry;
static int current = 0;//current free slot
static int num = 0;//current num of routine (not include main routine)
static int next_to_yield = 0;//next one to resume
static int me = 0;//mark currunt routine num

void yield(){
    printf("stop routine%d\n",me);
    me = setjmp(routine_list[me]);
    //printf("In yield with me = %d\n", me);
    if (!me){
        //printf("Setup new env: %d\n", routine_list[me]);
        //printf("LNGJMP TO ENTRY %d\n", num);
        longjmp(syscall_loop_entry,1);
    } else{
        if (me == -1) me = 0;
        printf("restart routine%d\n",me);
        return; 
    }
    //never reach
}

//used by main routine to call subroutine --syscall_loop is the main routine
void resume(){
    /* printf("In resume %d\n", num); */
    printf("resume_num%d",num);
    if (num == 0) return;
    //printf("Keep resume\n");
    uint32_t tar = next_to_yield;
    while (free_list[next_to_yield] == 0) {
        next_to_yield = (next_to_yield+1)%ROUTINE_NUM; 
    }
    //printf("long jumping to %d = %d\n", tar, routine_list[tar]);

    longjmp(routine_list[tar],tar==0?-1:tar);

    /* Never reach */
}

void test() {
    int a = 5;
    printf("a %p\n", &a);
}

int start_coroutine(void (*function)(seL4_Word badge, int numargs),
                    void *data) {
    if (num==ROUTINE_NUM) {
        return 0; 
    }

    printf("Start coroutine%d\n",current);
    num++;
    me = current;
    free_list[current] = 0;

    /* Set new current */
    while (free_list[current] != 0){
        current++;
        current %= ROUTINE_NUM;
    }

    /* Allocate new stack frame */
    char *sptr;
    int err = frame_alloc((seL4_Word *) &sptr);
    sptr += 4096;
    /* conditional_panic(err, "Could not allocate frame\n"); */
    //printf("Frame %p\n", sptr);
    //printf("data[0] %d, data[1] %d\n", ((seL4_Word*)data)[0],((seL4_Word*)data)[1]);

    //TODO memcpy instead?
    /* Add stuff to the new stack */
    void *sptr_new = sptr;
    sptr -= sizeof(void *);
    *sptr = sptr_new;

    void *data_new = data;
    sptr -= sizeof(void *);
    *sptr = (seL4_Word) data_new;

    /* Change to new sp */
    //printf("change\n");
    asm volatile("mov sp, %[newsp]" : : [newsp] "r" (sptr) : "sp");
    // TODO change stack limit register?
    //test();

    //printf("Start task\n");
    function(((seL4_Word *) data_new)[0], ((seL4_Word *) data_new)[1]);
    //printf("TASK FINISH\n");

    num--;
    free_list[me] = 1;
    frame_free(sptr_new);
    printf("Finish %d\n",me);
    longjmp(syscall_loop_entry, 1);

    /* Never reach */
}
