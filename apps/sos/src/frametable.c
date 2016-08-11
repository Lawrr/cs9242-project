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
    seL4_Word entry;
    int32_t next;
};

static struct frame_entry *frame_table;
static uint64_t base_addr;
static int32_t low_addr;
static int32_t high_addr;
static int free_index;/* -1 no free_list but memory is not full
                         -2 no free_list and memory is full(swaping later)*/
static seL4_CPtr frame_table_cap;


void frame_init(seL4_Word high,seL4_Word low) {
    low_addr = low;
    high_addr = high;
    int err;

    uint64_t low64 = low;
    uint64_t high64 = high;
      
    uint64_t entry_size = sizeof(struct frame_entry);
    base_addr = (high64 * entry_size + PAGE_SIZE * low64) / (entry_size + PAGE_SIZE); 
    uint32_t frame_table_bits = 0;
    uint64_t frame_table_size = base_addr - low64;
    while (frame_table_size >>= 1) frame_table_bits++;

    ;
    frame_table_size  = base_addr -low64;
    printf("%llu\n",base_addr);
    printf("%llu\n",low64);
    printf("%llu\n",frame_table_size);

    seL4_Word ft_section;
    printf("Hi\n");
    for (uint64_t i = 0; i < frame_table_size;i+=PAGE_SIZE){

        ft_section = ut_alloc(seL4_PageBits);
        seL4_Word virtual = ft_section - low + PROCESS_VMEM_START;

        if (i == 0) {
            frame_table = virtual;
        }
        
        err = cspace_ut_retype_addr(ft_section,
                seL4_ARM_SmallPageObject,
                seL4_PageBits,
                cur_cspace,
                &frame_table_cap);
        conditional_panic(err, "Failed to allocate frame table cap");     

        //printf("%x - %x + %x\n", ft_section, base_addr, PROCESS_VMEM_START);
        printf("Phys: %x -> %x\n", ft_section, virtual);

        err = map_page(frame_table_cap,
                seL4_CapInitThreadPD,
                virtual,
                seL4_AllRights,
                seL4_ARM_Default_VMAttributes);
        conditional_panic(err, "Failed to map frame table");
    }
    printf("Bye\n");

    memset(frame_table, 0, (1 << seL4_PageBits));
    printf("Bye2\n");

    free_index = -1;
}

int32_t frame_alloc(seL4_Word *vaddr) {
    printf("11111111\n");
    int err;
    int ret_index;
    if  (free_index == -1){
        printf("2.1\n");
    	seL4_Word frame_paddr = ut_alloc(seL4_PageBits);
    	seL4_Word frame_cap;
    	/* Retype to frame */
    	err = cspace_ut_retype_addr(frame_paddr,
                                seL4_ARM_SmallPageObject,
                                seL4_PageBits,
                                cur_cspace,
                                &frame_cap);
        printf("3.1\n");
    	if (err) {
        	return -1;
    	}
        printf("4.1\n");

    /* Map to address space */
        seL4_Word virtual = frame_paddr - low_addr + PROCESS_VMEM_START;
        printf("Virtual=%x paddr=%x\n", virtual, frame_paddr);
    	err = map_page(frame_cap,
                   seL4_CapInitThreadPD,
                   virtual,
                   seL4_AllRights,
                   seL4_ARM_Default_VMAttributes);
    	if (err) {
        	return -1;
    	}

        *vaddr = virtual;
    	ret_index = (frame_paddr - low_addr)>>12;
    	frame_table[ret_index].cap = frame_cap;
    }   else {
        printf("2.2\n");
        ret_index = free_index;
        free_index = frame_table[free_index].next;
        printf("3.2\n");
    }
    return ret_index;
}

void frame_free(int32_t index) {

}
