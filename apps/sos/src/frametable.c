#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <cspace/cspace.h>

#include "ut_manager/ut.h"

#define PAGE_SIZE 4096lu /* Bytes */

static struct frame_entry {
    /* Reserve 3 bits for type */
    /* 000 for 'valid' */
    seL4_Word entry;
};

static struct frame_entry *frame_table;

static uint64_t base_addr;

void frame_init(seL4_Word low, seL4_Word high) {
    uint64_t low_64 = low;
    uint64_t high_64 = high;
    
    printf("low_64 %llu\n",low_64);
    printf("high_64 %llu\n", high_64);
    
    uint64_t entry_size = sizeof(struct frame_entry);
    base_addr = (high_64 * entry_size + PAGE_SIZE * low_64) / (entry_size + PAGE_SIZE);
    //ut_alloc(base_addr 
    uint32_t frame_table_bits = 0;
    uint64_t frame_table_size = base_addr - low_64;
    printf("low: %llu\nhigh: %llu\nbase: %llu\nsize:%llu\n", low_64, high_64, base_addr, frame_table_size);
    while (frame_table_size >>= 1) frame_table_bits++;
    printf("bits %d\n",frame_table_bits);
    //frame_table = ut_alloc(frame_table_bits);
    
    printf("%lx base addr - low\n", base_addr - low);
}

void frame_alloc(seL4_Word vaddr) {
    //ut_alloc(
}

void frame_free(seL4_Word page) {

}
