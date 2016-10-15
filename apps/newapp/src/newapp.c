/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <utils/time.h>

/* Your OS header file */
#include <sos.h>

int main(void) {
    for (int i = 0; i < 100; i++) {
        printf("\n[%d] Hello World!\n", sos_my_id());
        sleep(1);
    }
    /* sleep(10); */
    /* printf("\n[%d] Done\n", sos_my_id()); */
    return 0;
}
