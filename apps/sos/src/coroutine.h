#ifndef __COROUTINE_H_
#define __COROUTINE_H_

#include <cspace/cspace.h>

void yield();

void resume();
void set_resume(int id);

int start_coroutine(void (*task)(seL4_Word badge, int num_args), void *data);

seL4_Word get_routine_arg(int id, int i);
void set_routine_arg(int id, int i, seL4_Word arg);

#endif
