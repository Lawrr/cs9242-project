#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <vmem_layout.h>
#include <cspace/cspace.h>
#include <utils/page.h>
#include <fcntl.h>

#include "ut_manager/ut.h"
#include "mapping.h"
#include "process.h"
#include "vnode.h"

#include <sys/panic.h>

#define PAGE_TABLE_MASK (0xFFF00000)
#define PAGE_SIZE 4096lu /* Bytes */
#define INDEX_ADDR_OFFSET 12 /* Bits to shift */

#define FRAME_REFERENCE (1 << 2)
#define FRAME_SWAPABLE (1 << 1)
#define FRAME_VALID (1 << 0)

extern struct PCB *curproc;

static struct app_cap {
    struct page_table_entry pte;
    struct swap_table_entry ste;
    seL4_CPtr cap;
    struct app_cap *next;
};

/* val swapable | valid
 * XXXXXX| R | S | V |
 * R:reference bit which used for second chance replacement
 * S:Swappable bit because some frame is allocated as coroutine stack
 * V:frame that is valid , which can be swaped if swap bit is on
 */
static struct frame_entry {
    seL4_CPtr cap;
    struct app_cap app_caps;
    int32_t next_index;
    uint32_t mask;
};

static struct frame_table_cap {
    int ref;
    seL4_CPtr cap;
    struct frame_table_cap *next;
};

static struct frame_entry *frame_table;

/* Untyped region after end of frame table */
static uint64_t base_addr;
static int32_t low_addr;
static int32_t high_addr;

static uint32_t num_frames;

/* Swapping */
static struct vnode *swap_vnode;
static uint32_t curr_swap_offset = 0;

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
    uint64_t num_pages = (high64 - base_addr) / PAGE_SIZE;
    /* Make sure num_entries >= num_pages */
    num_frames = num_entries;
    printf("Base: %llu, oldbase: %llu\n", base_addr, low64 + frame_table_size);
    printf("entries: %llu, pages: %llu\n", num_entries, num_pages);
    if (num_entries < num_pages) {
        frame_table_size += entry_size;
    }
    printf("Base: %llu, newbase: %llu\n", base_addr, low64 + frame_table_size);
    base_addr = low;

    /* Allocated each section of frame table */
    seL4_Word ft_section_paddr;
    for (uint64_t i = 0; i < frame_table_size; i += PAGE_SIZE) {
        seL4_CPtr cap;

        /* Get untyped memory */
        ft_section_paddr = ut_alloc(seL4_PageBits);
        seL4_Word ft_section_vaddr = i + PROCESS_VMEM_START;

        /* Retype to frame */
        err = cspace_ut_retype_addr(ft_section_paddr,
                seL4_ARM_SmallPageObject,
                seL4_PageBits,
                cur_cspace,
                &cap);
        conditional_panic(err, "Failed to allocate frame table cap");

        /* Map to address space */
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

/* get free frame from list */
static seL4_Word get_free_frame() {
    seL4_Word frame_vaddr = ((free_index << INDEX_ADDR_OFFSET) + base_addr - low_addr + PROCESS_VMEM_START);
    frame_table[free_index].mask = FRAME_SWAPABLE | FRAME_VALID | FRAME_REFERENCE;
    free_index = frame_table[free_index].next_index;
    memset(frame_vaddr, 0, PAGE_SIZE);
    return frame_vaddr;
}

int32_t stack_alloc(seL4_Word *vaddr) {
    int err = frame_alloc(vaddr);
    if (err) return err;
    int index = (*vaddr + low_addr - PROCESS_VMEM_START) >> INDEX_ADDR_OFFSET;
    frame_table[index].mask &= (~FRAME_SWAPABLE);
    return err;
}

int32_t stack_free(seL4_Word *vaddr) {
    return frame_free(vaddr);
}

int32_t swap_out() {
    //TODO unmap
    for (int i = 0; i < num_frames; i++) {
        if ((frame_table[i].mask & FRAME_VALID) && (frame_table[i].mask & FRAME_SWAPABLE)) {
            if (frame_table[i].mask & FRAME_REFERENCE) {
                //clear reference
                frame_table[i].mask &= (~FRAME_REFERENCE);
            } else {
                seL4_Word frame_vaddr = (i << INDEX_ADDR_OFFSET) + base_addr - low_addr + PROCESS_VMEM_START;
                if (swap_vnode == NULL) {
                    vfs_open("swpf", FM_READ|FM_WRITE, &swap_vnode);
                }
                struct uio uio = {
                    .offset = curr_swap_offset,
                    .addr = frame_vaddr
                };

                int err = swap_vnode->ops->vop_write(swap_vnode, &uio);
                if (err) return err;
                free_index = i;
                return err;
            }
        }
    }
}

int32_t swap_in(int file_offset) {
    seL4_Word sos_vaddr;
    int err = frame_alloc(&sos_vaddr);
    if (err) return err;
    struct uio uio = {
        .offset = curr_swap_offset,
        .addr = PAGE_ALIGN_4K(sos_vaddr)
    };
    err = swap_vnode->ops->vop_read(swap_vnode, &uio);
    //TODO remap
    return err;
}

int32_t frame_alloc(seL4_Word *vaddr) {
    int err;
    seL4_Word frame_vaddr;

    if (free_index == -1) {
        /* Free list is empty but there is still memory */

        /* Get untyped memory */
        seL4_Word frame_paddr = ut_alloc(seL4_PageBits);
        if (frame_paddr == NULL) {
            *vaddr = NULL;
            swap_out();
            *vaddr = get_free_frame();
            return 0;
        }

        /* Retype to frame */
        seL4_Word frame_cap;
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

        frame_table[index].mask = FRAME_SWAPABLE | FRAME_VALID | FRAME_REFERENCE;
        frame_table[index].cap = frame_cap;

    } else {
        /* Reuse a frame in the free list */
        frame_vaddr = get_free_frame();
    }

    /* Clear frame */
    memset(frame_vaddr, 0, PAGE_SIZE);

    *vaddr = frame_vaddr;
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

    /* Check that the frame was previously allocated */
    if (frame_table[index].cap == seL4_CapNull) return -1;

    /* Set free list index */
    frame_table[index].mask = 0;
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
    if (new_app_cap == NULL) {
        return NULL;
    }

    /* Initialise variables */
    new_app_cap->next = NULL;
    new_app_cap->pte = *pte;
    new_app_cap->ste = (struct swap_table_entry){0};
    new_app_cap->cap = cap;

    return new_app_cap;
}

int32_t insert_app_cap(seL4_Word vaddr, seL4_CPtr cap, struct page_table_entry *pte) {
    uint32_t index = (vaddr - PROCESS_VMEM_START + low_addr - base_addr) >> INDEX_ADDR_OFFSET;

    /* Check that the frame exists */
    if (frame_table[index].cap == seL4_CapNull) return -1;

    struct app_cap *copied_cap;
    /* Note: First app cap is not malloc'd */
    if (frame_table[index].app_caps.cap == seL4_CapNull) {
        /* First app cap */
        copied_cap = &frame_table[index].app_caps;
        copied_cap->next = NULL;
        copied_cap->pte = *pte;
        copied_cap->ste = (struct swap_table_entry){0};
        copied_cap->cap = cap;
    } else {
        /* Create new app cap */
        copied_cap = app_cap_new(cap, pte);
        if (copied_cap == NULL) return -1;

        /* Insert into list of app caps for the frame */
        //TODO just insert to head
        struct app_cap *curr_cap = copied_cap;
        while (curr_cap->next != NULL) {
            curr_cap = curr_cap->next;
        }
        curr_cap->next = copied_cap;
    }

    return 0;
}

int32_t get_app_cap(seL4_Word vaddr, struct app_cap **cap_ret) {
    struct page_table_entry **page_table = curproc->addrspace->page_table;

    uint32_t index = (vaddr - PROCESS_VMEM_START + low_addr - base_addr) >> INDEX_ADDR_OFFSET;
    if (frame_table[index].cap == seL4_CapNull) return -1;

    struct app_cap *curr_cap = &frame_table[index].app_caps;
    while (curr_cap != NULL) {
        if ((curr_cap->pte.sos_vaddr & PAGE_TABLE_MASK) == page_table) break;
        printf("%x----%x\n", curr_cap->pte.sos_vaddr, page_table);
        curr_cap = curr_cap->next;
    }
    if (curr_cap == NULL) {
        return -1;
    } else {
        *cap_ret = curr_cap;
        return 0;
    }
}
