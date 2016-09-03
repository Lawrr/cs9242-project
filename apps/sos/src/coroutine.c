#include "coroutine.h"
#include <string.h>
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

jmp_buf *entry = NULL;
int next = 0;
int current = 0;
int num = -1;
int next_to_yield = 0;
void yield(){
   if(num==-1){
      num++;
      setjmp(entry); 
      return;
   }  else{
      longjmp(entry);
      //never reach
   }  
}

//used by main routine to call subroutine --syscall_loop is the main routine
void resume(){
   seL4_Word tar = next_to_yield;
   while (routine_list[next_to_yield].free) next_to_yield = (next_to_yield+1)%ROUTINE_NUM;
   longjmp(tar,1);
   //never reach
}

int start_coroutine(void (*function)(seL4_Word badge,int numargs),void*data){
   num++;
   char buffer[(current+1)*CO_STACK_SIZE];
   if (num==ROUTINE_NUM) {
      return 0; 
   } 
   function(data[0],data[1]);
   longjmp(*entry,1);//return success
   //never reach
}
