#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <vmem_layout.h>
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

static struct frame_entry *frame_table;
static seL4_Word base_addr;
static int free_index;
static seL4_CPtr frame_table_cap;


void frame_init(seL4_Word high,seL4_Word low) {
    int err;
    seL4_Word curr = DMA_VEND;

    uint64_t low64 = low;
    uint64_t high64 = high;
      
    uint64_t entry_size = sizeof(struct frame_entry);
    base_addr = (high64 * entry_size + PAGE_SIZE * low64) / (entry_size + PAGE_SIZE); 
    uint32_t frame_table_bits = 0;
    uint64_t frame_table_size = base_addr - low64;
    while (frame_table_size >>= 1) frame_table_bits++;

    ;
    frame_table_size  = base_addr -low64;
        printf("00000\n");
    for (uint64_t i = 0; i < frame_table_size;i+=PAGE_SIZE){
       printf("11111%d\n",i);
       frame_table = ut_alloc(seL4_PageBits);
        
       printf("22222%d\n",i);
       err = cspace_ut_retype_addr(frame_table,
                                seL4_ARM_SmallPageObject,
                                seL4_PageBits,
                                cur_cspace,
                                &frame_table_cap);
       conditional_panic(err, "Failed to allocate frame table cap");     
       err = map_page(frame_table_cap,
                   seL4_CapInitThreadPD,
                   curr,
                   seL4_AllRights,
       seL4_ARM_Default_VMAttributes);
       printf("------%d\n",err);
       conditional_panic(err, "Failed to map frame table");
       curr += PAGE_SIZE;
    }

    frame_table = (struct frame_entry*)DMA_VEND;
    memset(frame_table, 0, (1 << seL4_PageBits));
    free_index = 0;
}

int32_t frame_alloc(seL4_Word *vaddr) {
    int err;
    
    seL4_Word frame_paddr = ut_alloc(seL4_PageBits);
    seL4_Word frame_cap;
    /* Retype to frame */
    err = cspace_ut_retype_addr(frame_paddr,
                                seL4_ARM_SmallPageObject,
                                seL4_PageBits,
                                cur_cspace,
                                &frame_cap);
    if (err) {
        return -1;
    }

    /* Map to address space */
    err = map_page(frame_cap,
                   seL4_CapInitThreadPD,
                   vaddr,
                   seL4_AllRights,
                   seL4_ARM_Default_VMAttributes);
    if (err) {
        return -1;
    }

    frame_table[frame_paddr>>12].cap = frame_cap;
    return free_index++;
}

void frame_free(int32_t index) {

}
