#include "coroutine.h"
#include <setjmp.h>
#include <cspace/cspace.h>
#include <alloca.h>
#include "frametable.h"
#define CO_STACK_SIZE 512
#define ROUTINE_NUM 7


struct routine{
    int free;
    jmp_buf env;
};
static struct routine routine_list[ROUTINE_NUM] = {0};

void coroutine_init(){
    for (int i = 0; i < ROUTINE_NUM; i++){
        routine_list[i].free = 1;
    }
}

extern jmp_buf syscall_loop_entry;
static int current = 0;//current free slot
static int num = 0;//current num of routine (not include main routine)
static int next_to_yield = 0;//next one to resume
static int me = 0;//mark currunt routine num

void yield(){
    me = setjmp(routine_list[me].env);
    if (!me){
        printf("LNGJMP TO ENTRY %d\n", num);
        longjmp(syscall_loop_entry,1);
    } else{
        if (me == -1) me = 0;
        printf("Returning to task\n");
        return; 
    }
    //never reach
}

//used by main routine to call subroutine --syscall_loop is the main routine
void resume(){
    /* printf("In resume %d\n", num); */
    if (num == 0) return;
    printf("Keep resume\n");
    uint32_t tar = next_to_yield;
    while (routine_list[next_to_yield].free) {
        next_to_yield = (next_to_yield+1)%ROUTINE_NUM; 
    }
    printf("long jumping to %d\n", tar);
    longjmp(routine_list[tar].env,tar==0?-1:tar);
    //never reach
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

    printf("Start coroutine\n");
    num++;
    me = current;
    routine_list[me].free=0;

    /* Set new current */
    while (!routine_list[current].free){
        current++;
        current %= ROUTINE_NUM;
    }

    /* Allocate new stack frame */
    char *sptr;
    int err = frame_alloc((seL4_Word *) &sptr);
    /* conditional_panic(err, "Could not allocate frame\n"); */
    printf("Frame %p\n", sptr);

    /* Add stuff to the new stack */
    void *sptr_new = sptr;
    *sptr = sptr_new;
    sptr -= sizeof(void *);

    void *data_new = data;
    *sptr = (seL4_Word) data_new;
    sptr -= sizeof(void *);

    /* Change to new sp */
    printf("change\n");
    sptr += 4096;
    asm volatile("mov sp, %[newsp]" : : [newsp] "r" (sptr) : "sp");
    // TODO change stack limit register?
    test();

    printf("Start task\n");
    function(((seL4_Word *) data_new)[0], ((seL4_Word *) data_new)[1]);
    printf("TASK FINISH\n");

    num--;
    routine_list[me].free=1; 
    frame_free(sptr_new);

    longjmp(syscall_loop_entry, 1);

    /* Never reach */
}
