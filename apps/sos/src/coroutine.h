#ifndef __COROUTINE_H_
#define __COROUTINE_H_

#include <cspace/cspace.h>
#include <setjmp.h>
#include <process.h>
#include <mapping.h>

/* Release the current execution and give it to other task */
void yield();
/* Resume a coroutine correspond to the next_resume_id */
void resume();
/* Set the value of next_resume_id */
void set_resume(int id);

void cleanup_coroutine();
void set_cleanup_coroutine(int id);

int start_coroutine(void (*task)(seL4_Word badge, int num_args),
                    seL4_Word badge, int num_args, struct PCB *pcb);

seL4_Word get_routine_arg(int id, int i);

void set_routine_arg(int id, int i, seL4_Word arg);

#endif
