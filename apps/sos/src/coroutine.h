#ifndef __COROUTINE_H_
#define __COROUTINE_H_
#include <cspace/cspace.h>
void yield(void);
void resume(void);
int start_coroutine(void (*function)(seL4_Word badge,int numargs),void* data);
#endif
