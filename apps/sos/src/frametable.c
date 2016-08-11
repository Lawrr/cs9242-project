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
    seL4_Word entry;
    seL4_CPtr cap;
    seL4_Word addr;
    int32_t next;
};

static struct frame_entry *frame_table;
static seL4_CPtr frame_table_cap;
static int free_index;

static uint64_t base_addr;

void frame_init(seL4_Word low, seL4_Word high) {
    int err;
    uint64_t low64 = low;
    uint64_t high64 = high;
    
    uint64_t entry_size = sizeof(struct frame_entry);
    base_addr = (high64 * entry_size + PAGE_SIZE * low64) / (entry_size + PAGE_SIZE);

    uint64_t frame_table_size = base_addr - low64;

    /* Get num bits for frame_table */
    uint32_t frame_table_bits = 0;
    while (frame_table_size >>= 1) {
        frame_table_bits++;
    }
    /* Reset frame_table_size value */
    frame_table_size = base_addr - low64;

    for (uint64_t i = 0; i <= frame_table_size / FRAME_SIZE; i++) {
        seL4_Word *ft_addr;
        seL4_CPtr cap;
        if (i == 0) {
            frame_table = ut_alloc(seL4_PageDirBits);
            ft_addr = frame_table;
        } else {
            ft_addr = ut_alloc(seL4_PageDirBits);
        }
        err = cspace_ut_retype_addr(ft_addr,
                                    seL4_ARM_PageDirectoryObject,
                                    seL4_PageDirBits,
                                    cur_cspace,
                                    &cap);
        conditional_panic(err, "Failed to allocate frame table cap");

        /* Map to address space */
        err = map_page(cap,
                       seL4_CapInitThreadPD,
                       ft_addr,
                       seL4_AllRights,
                       seL4_ARM_Default_VMAttributes);
        conditional_panic(err, "Failed to map frame table");
    }
    free_index = 0;
}

int32_t frame_alloc(seL4_Word *vaddr) {
    int err;

    /* Get frame */
    struct frame_entry *frame = &frame_table[free_index];

    /* Allocated untyped */
    frame->addr = ut_alloc(seL4_PageBits);

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
