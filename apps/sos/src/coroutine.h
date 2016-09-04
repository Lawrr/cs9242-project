#ifndef __COROUTINE_H_
#define __COROUTINE_H_

#include <cspace/cspace.h>

void yield();

void resume();
void resume_coroutine(int id);
int start_coroutine(void (*task)(seL4_Word badge, int num_args), void *data);

#endif
