#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <vmem_layout.h>
#include <cspace/cspace.h>

#include "ut_manager/ut.h"
#include "mapping.h"

#include <sys/panic.h>
#define PAGE_TABLE_MASK (0xFFF00000)
#define PAGE_SIZE 4096lu /* Bytes */
#define INDEX_ADDR_OFFSET 12 /* Bits to shift */

static struct frame_entry {
    seL4_CPtr cap;
    struct app_cap *app_cap_list;
    int32_t next_index;
};

static struct frame_table_cap {
    int ref;
    seL4_CPtr cap;
    struct frame_table_cap *next;
};

struct app_cap {
    struct page_table_entry *pte;
    seL4_CPtr cap;
    struct app_cap *next;
};

static struct frame_entry *frame_table;

/* Untyped region after end of frame table */
static uint64_t base_addr;

static int32_t low_addr;
static int32_t high_addr;

/* -1 no free_list but memory is not full
   -2 no free_list and memory is full (swapping later)*/
static int32_t free_index;

static struct frame_table_cap *frame_table_cap_head;

void frame_init(seL4_Word high, seL4_Word low) {
    int32_t err;
    uint64_t low64 = low;
    uint64_t high64 = high;
    uint64_t entry_size = sizeof(struct frame_entry);

    /* Set/calculate untyped region bounds */

    /* base_address is the address right above the frame table where
     * the untyped memory region which our frame table keeps track of begins.
     *
     * The number of frame table entries should correspond
     * with the number of pages starting from the base_address.
     *
     * To find this, solve for base_address using:
     * (base_address - low) / sizeof(entry) => (high - base_address) / PAGE_SIZE
     */
    base_addr = (high64 * entry_size + PAGE_SIZE * low64) / (entry_size + PAGE_SIZE);
    low_addr = low;
    high_addr = high;

    /* Calculate frame table size */
    uint64_t frame_table_size = base_addr - low64;
    uint64_t num_entries = frame_table_size / entry_size;
    uint64_t num_pages = high64 - base_addr / PAGE_SIZE;
    /* Make sure num_entries >= num_pages */
    if (num_entries < num_pages) {
        frame_table_size += entry_size;
    }
    base_addr = low;

    /* Allocated each section of frame table */
    seL4_Word ft_section_paddr;
    for (uint64_t i = 0; i < frame_table_size; i += PAGE_SIZE) {
        seL4_CPtr cap;

        ft_section_paddr = ut_alloc(seL4_PageBits);
        seL4_Word ft_section_vaddr = i + PROCESS_VMEM_START;

        err = cspace_ut_retype_addr(ft_section_paddr,
                seL4_ARM_SmallPageObject,
                seL4_PageBits,
                cur_cspace,
                &cap);
        conditional_panic(err, "Failed to allocate frame table cap");

        err = map_page(cap,
                seL4_CapInitThreadPD,
                ft_section_vaddr,
                seL4_AllRights,
                seL4_ARM_Default_VMAttributes);
        conditional_panic(err, "Failed to map frame table");

        /* Set pointer to head of frame_table and keep track of caps */
        struct frame_table_cap *cap_holder = malloc(sizeof(struct frame_table_cap));
        cap_holder->cap = cap;
        if (i == 0) {
            frame_table = ft_section_vaddr;
            cap_holder->next = NULL;
        } else {
            cap_holder->next = frame_table_cap_head;
        }
        frame_table_cap_head = cap_holder;

        /* Clear frame_table memory */
        memset(ft_section_vaddr, 0, PAGE_SIZE);
        base_addr += PAGE_SIZE;
    }
    /* Init free index */
    free_index = -1;
}

int32_t frame_alloc(seL4_Word *vaddr) {
    int err;
    seL4_Word frame_vaddr;
    if (free_index == -1) {
        /* Free list is empty but there is still memory */
        seL4_Word frame_cap;
        seL4_Word frame_paddr = ut_alloc(seL4_PageBits);
        if (frame_paddr == NULL) {
            *vaddr = NULL;
            return 1;
        }
        /* Retype to frame */
        err = cspace_ut_retype_addr(frame_paddr,
                seL4_ARM_SmallPageObject,
                seL4_PageBits,
                cur_cspace,
                &frame_cap);
        if (err) {
            ut_free(frame_paddr, seL4_PageBits);
            *vaddr = NULL;
            return 2;
        }

        /* Map to address space */
        frame_vaddr = frame_paddr - low_addr + PROCESS_VMEM_START;
        err = map_page(frame_cap,
                seL4_CapInitThreadPD,
                frame_vaddr,
                seL4_AllRights,
                seL4_ARM_Default_VMAttributes);
        if (err) {
            cspace_delete_cap(cur_cspace, frame_cap);
            ut_free(frame_paddr, seL4_PageBits);
            *vaddr = NULL;
            return 3;
        }

        /* Calculate index of frame in the frame table */
        int index = (frame_paddr - base_addr) >> INDEX_ADDR_OFFSET;

        frame_table[index].cap = frame_cap;
    } else {
        frame_vaddr = ((free_index << INDEX_ADDR_OFFSET) + base_addr - low_addr + PROCESS_VMEM_START);
        free_index = frame_table[free_index].next_index;
        memset(frame_vaddr, 0, PAGE_SIZE);
    }

    *vaddr = frame_vaddr;
    memset(frame_vaddr, 0, PAGE_SIZE);
    return 0;
}

/*
   int32_t stack_alloc(seL4_Word *stack_vaddr) {
   seL4_Word stack_cap;
   seL4_Word stack_paddr = ut_alloc(seL4_PageBits);
   if (frame_paddr == NULL) {
 *vaddr = NULL;
 return 1;
 }
// Retype to stack
err = cspace_ut_retype_addr(stack_paddr,
seL4_ARM_SmallPageObject,
seL4_PageBits,
cur_cspace,
&stack_cap);
if (err) {
ut_free(frame_paddr, seL4_PageBits);
 *vaddr = NULL;
 return 2;
 }

// Map to address space
stack_vaddr = stack_paddr - low_addr + PROCESS_VMEM_START;
err = map_page(frame_cap,
seL4_CapInitThreadPD,
frame_vaddr,
seL4_AllRights,
seL4_ARM_Default_VMAttributes);
if (err) {
cspace_delete_cap(cur_cspace, stack_cap);
ut_free(stack_paddr, seL4_PageBits);
 *vaddr = NULL;
 return 3;
 }
 *vaddr = frame_vaddr;
 int index = (stack_paddr - base_addr) >> INDEX_ADDR_OFFSET;
 frame_table[index].cap = frame_cap;
 }*/


int32_t frame_free(seL4_Word vaddr) {
    uint32_t index = (vaddr - PROCESS_VMEM_START + low_addr - base_addr) >> INDEX_ADDR_OFFSET;

    //The frame is not yet allocated
    if (frame_table[index].cap == seL4_CapNull) return -1;

    frame_table[index].next_index = free_index;
    free_index = index;

    return 0;
}

seL4_CPtr get_cap(seL4_Word vaddr) {
    uint32_t index = (vaddr - PROCESS_VMEM_START + low_addr - base_addr) >> INDEX_ADDR_OFFSET;
    return frame_table[index].cap;
}

static struct app_cap *app_cap_new(seL4_CPtr cap, struct page_table_entry *pte) {
    struct app_cap *new_app_cap = malloc(sizeof(struct app_cap));
    if (new_app_cap == NULL) return NULL;
    new_app_cap->next = NULL;
    new_app_cap->pte = pte;
    new_app_cap->cap = cap;
    return new_app_cap;
}

int32_t insert_app_cap(seL4_Word vaddr, seL4_CPtr cap, struct page_table_entry *pte) {
    uint32_t index = (vaddr - PROCESS_VMEM_START + low_addr - base_addr) >> INDEX_ADDR_OFFSET;
    if (frame_table[index].cap == seL4_CapNull) return -1;

    struct app_cap *copied_cap = app_cap_new(cap, pte);
    if (copied_cap == NULL) return -1;

    copied_cap->next = frame_table[index].app_cap_list;
    frame_table[index].app_cap_list = copied_cap;
    return 0;
}

//Assume we don't need to traverse the list to find the corrsponding app_cap
/*
   int32_t get_app_cap(seL4_Word vaddr, struct page_table_entry **page_table, seL4_CPtr *cap_ret) {
   uint32_t index = (vaddr - PROCESS_VMEM_START + low_addr - base_addr) >> INDEX_ADDR_OFFSET;
   if (frame_table[index].cap == seL4_CapNull) return -1;

   struct app_cap *curr_cap = frame_table[index].app_cap_list;
   while (curr_cap != NULL) {
   if ((curr_cap->pte->sos_vaddr & PAGE_TABLE_MASK) == page_table) break;
   printf("%x----%x\n", curr_cap->pte->sos_vaddr, page_table);
   curr_cap = curr_cap->next;
   }
   if (curr_cap == NULL) {
   return -1;
   } else {
 *cap_ret = curr_cap->cap;
 return 0;
 }
 }*/
