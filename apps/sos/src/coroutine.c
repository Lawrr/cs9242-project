#include "coroutine.h"
#include <setjmp.h>
#include <cspace/cspace.h>
#include <alloca.h>
#define CO_STACK_SIZE 512
#define ROUTINE_NUM 7


struct routine{
   int free;
   jmp_buf env;
};
static struct routine routine_list[ROUTINE_NUM]= {0};
void coroutine_init(){
   for (int i = 0; i < ROUTINE_NUM; i++){
       routine_list[i].free = 1;
   }   
}

jmp_buf entry;
int current = 0;//current free slot
int num = -1;//current num of routine (not include main routine)
int next_to_yield = 0;//next one to resume
int me = 0;//mark currunt routine num

void yield(){
   if(num==-1){
      num++;
      setjmp(entry); 
      return;
   }  else{
      me = setjmp(routine_list[me].env);
      if (!me){
         longjmp(entry,1);
      }  else{
         me == -1?0:me;
	 return; 
      }
      //never reach
   }  
}

//used by main routine to call subroutine --syscall_loop is the main routine
void resume(){
   uint32_t tar = next_to_yield;
   while (routine_list[next_to_yield].free) next_to_yield = (next_to_yield+1)%ROUTINE_NUM; 
   longjmp(routine_list[tar].env,tar==0?-1:tar);
   //never reach
}

int start_coroutine(void (*function)(seL4_Word badge,int numargs),void* data){
   
   alloca((current+1)*CO_STACK_SIZE);
   if (num==ROUTINE_NUM) {
      return 0; 
   }
   num++;
   
   function(((seL4_Word*)data)[0],((seL4_Word*)data)[1]);
   longjmp(entry,1);
   //never reach
}
