#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <cspace/cspace.h>

#include "ut_manager/ut.h"

#include <sys/panic.h>

#define FRAME_SIZE 1024llu /* Bytes */
#define PAGE_SIZE 4096lu /* Bytes */

static struct frame_entry {
    /* Reserve 3 bits for type */
    /* 000 for 'valid' */
    //seL4_Word entry;
    seL4_CPtr cap;
    seL4_Word addr;
    int32_t next;
};

static struct frame_entry **frame_table;
static seL4_CPtr frame_table_cap;
static int free_index;

static int32_t create_second_level(uint32_t index) {
    int err;

    seL4_CPtr cap;
    frame_table[index] = ut_alloc(seL4_PageBits);
    err = cspace_ut_retype_addr(frame_table[index],
                                seL4_ARM_SmallPageObject,
                                seL4_PageBits,
                                cur_cspace,
                                &cap);
    if (err) {
        return -1;
    }

    err = map_page(cap,
                   seL4_CapInitThreadPD,
                   frame_table[index],
                   seL4_AllRights,
                   seL4_ARM_Default_VMAttributes);
    if (err) {
        return -1;
    }

    return 0;
}

void frame_init() {
    int err;

    frame_table = ut_alloc(seL4_PageBits);
    err = cspace_ut_retype_addr(frame_table,
                                seL4_ARM_SmallPageObject,
                                seL4_PageBits,
                                cur_cspace,
                                &frame_table_cap);
    conditional_panic(err, "Failed to allocate frame table cap");

    err = map_page(frame_table_cap,
                   seL4_CapInitThreadPD,
                   frame_table,
                   seL4_AllRights,
                   seL4_ARM_Default_VMAttributes);
    conditional_panic(err, "Failed to map frame table");

    memset(frame_table, 0, (1 << seL4_PageBits));

    free_index = 0;
}

int32_t frame_alloc(seL4_Word *vaddr) {
    int err;
    
    seL4_Word frame_paddr = ut_alloc(seL4_PageBits);
    int index1 = frame_paddr >> 22;
    int index2 = (frame_paddr << 10) >> 22;
    
    /* Get frame */
    if (frame_table[index1] == NULL) {
        create_second_level(index1);
    }

    struct frame_entry *frame = &frame_table[index1][index2];

    /* Allocated untyped */
    frame->addr = frame_paddr;

    /* Retype to frame */
    err = cspace_ut_retype_addr(frame->addr,
                                seL4_ARM_SmallPageObject,
                                seL4_PageBits,
                                cur_cspace,
                                &frame->cap);
    if (err) {
        return -1;
    }

    /* Map to address space */
    err = map_page(frame->cap,
                   seL4_CapInitThreadPD,
                   frame->addr,
                   seL4_AllRights,
                   seL4_ARM_Default_VMAttributes);
    if (err) {
        return -1;
    }

    *vaddr = frame->addr;
    return free_index++;
}

void frame_free(int32_t index) {

}
