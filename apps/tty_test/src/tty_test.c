/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

/****************************************************************************
 *
 *      $Id:  $
 *
 *      Description: Simple milestone 0 test.
 *
 *      Author:			Godfrey van der Linden
 *      Original Author:	Ben Leslie
 *
 ****************************************************************************/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <sel4/sel4.h>


#include "ttyout.h"

// Block a thread forever
// we do this by making an unimplemented system call.
static void
thread_block(void){
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetTag(tag);
    seL4_SetMR(0, 1);
    seL4_Call(SYSCALL_ENDPOINT_SLOT, tag);
}

#define PAGE_SIZE_4K 4096
#define NPAGES 27
#define TEST_ADDRESS 0x20000000

/* called from pt_test */
static void
do_pt_test(char *buf)
{
    int i;

    /* set */
    for (int i = 0; i < NPAGES; i++) {
	    buf[i * PAGE_SIZE_4K] = i;
    }

    /* check */
    for (int i = 0; i < NPAGES; i++) {
	    assert(buf[i * PAGE_SIZE_4K] == i);
    }
}

void
pt_test( void )
{
    /* need a decent sized stack */
    char buf1[NPAGES * PAGE_SIZE_4K], *buf2 = NULL;

    /* check the stack is above phys mem */
    assert((void *) buf1 > (void *) TEST_ADDRESS);

    /* stack test */
    do_pt_test(buf1);

    /* heap test */
    buf2 = malloc(NPAGES * PAGE_SIZE_4K);
    assert(buf2);
    do_pt_test(buf2);
    free(buf2);
}

int main(void){
    /* initialise communication */
    ttyout_init();

    do {
        printf("task:\tHello world, I'm\ttty_test!\n");
        pt_test();
        thread_block();
        // sleep(1);	// Implement this as a syscall
    } while(1);

    return 0;
}
