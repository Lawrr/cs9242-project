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
static uint64_t base_addr; /* Untyped region after end of frame table */
static int32_t low_addr;
static int32_t high_addr;
static int free_index; /*-1 no free_list but memory is not full
                         -2 no free_list and memory is full (swapping later) */
static seL4_CPtr frame_table_cap;

void frame_init(seL4_Word high,seL4_Word low) {
    int err;
    uint64_t low64 = low;
    uint64_t high64 = high;
    uint64_t entry_size = sizeof(struct frame_entry);

    /* Set/calculate untyped region bounds */
    base_addr = (high64 * entry_size + PAGE_SIZE * low64) / (entry_size + PAGE_SIZE); 
    low_addr = low;
    high_addr = high;

    /* Calculate frame table size */
    uint64_t frame_table_size = base_addr - low64;

    /* Allocated each section of frame table */
    seL4_Word ft_section_paddr;
    for (uint64_t i = 0; i < frame_table_size; i += PAGE_SIZE) {

        ft_section_paddr = ut_alloc(seL4_PageBits);
        /* ft_section_paddr - low = phys mem untyped region offset (range: 0-high)
         * + PROCESS_VMEM_START = virtual mem offset
         */
        seL4_Word ft_section_vaddr = ft_section_paddr - low + PROCESS_VMEM_START;

        /* Set pointer to head of frame_table */
        if (i == 0) {
            frame_table = ft_section_vaddr;
        }

        err = cspace_ut_retype_addr(ft_section_paddr,
                seL4_ARM_SmallPageObject,
                seL4_PageBits,
                cur_cspace,
                &frame_table_cap);
        conditional_panic(err, "Failed to allocate frame table cap");     

        err = map_page(frame_table_cap,
                seL4_CapInitThreadPD,
                ft_section_vaddr,
                seL4_AllRights,
                seL4_ARM_Default_VMAttributes);
        conditional_panic(err, "Failed to map frame table");

        /* Clear frame_table memory */
        memset(ft_section_vaddr, 0, PAGE_SIZE);
    }

    /* Init free index */
    free_index = -1;
}

int32_t frame_alloc(seL4_Word *vaddr) {
    int err;
    int ret_index;
    seL4_Word frame_vaddr;
    
    if (free_index == -1) {
        /* Free list is empty but there is still memory */

        seL4_Word frame_cap;
        seL4_Word frame_paddr = ut_alloc(seL4_PageBits);
        if (frame_paddr == NULL) {
            *vaddr = NULL;
            return -1;
        }

        /* Retype to frame */
        err = cspace_ut_retype_addr(frame_paddr,
                seL4_ARM_SmallPageObject,
                seL4_PageBits,
                cur_cspace,
                &frame_cap);
        if (err) {
            *vaddr = NULL;
            return -1;
        }

        /* Map to address space */
        /* frame_paddr - low = phys mem untyped region offset (range: 0-high)
         * + PROCESS_VMEM_START = virtual mem offset
         */
        frame_vaddr = frame_paddr - low_addr + PROCESS_VMEM_START;
        err = map_page(frame_cap,
                seL4_CapInitThreadPD,
                frame_vaddr,
                seL4_AllRights,
                seL4_ARM_Default_VMAttributes);
        if (err) {
            *vaddr = NULL;
            return -1;
        }

        *vaddr = frame_vaddr;

        /* Calculate index of frame in the frame table */
        ret_index = (frame_paddr - low_addr) >> 12;
        frame_table[ret_index].cap = frame_cap;
    } else {
        ret_index = free_index;
        frame_vaddr = ((ret_index << 12) + PROCESS_VMEM_START);
        free_index = frame_table[free_index].next;
    }
    
    memset(frame_vaddr, 0, PAGE_SIZE);

    return ret_index;
}

void frame_free(int32_t index) {
    frame_table[index].next = free_index;
    free_index = index;
}
