#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <vmem_layout.h>
#include <cspace/cspace.h>
#include <utils/page.h>
#include <fcntl.h>
#include <pthread.h>

#include "ut_manager/ut.h"
#include "mapping.h"
#include "process.h"
#include "vnode.h"
#include "frametable.h"
#include "mapping.h"

#include <sys/panic.h>


/* #define LIMIT_FRAMES */

#ifdef LIMIT_FRAMES
int frames_to_alloc = 0;
#endif


#define PAGE_TABLE_MASK (0xFFF00000)
#define PAGE_SIZE 4096lu /* Bytes */
#define INDEX_ADDR_OFFSET 12 /* Bits to shift */

#define FRAME_REFERENCE (1 << 2)
#define FRAME_SWAPPABLE (1 << 1)
#define FRAME_VALID (1 << 0)

extern struct PCB *curproc;

const char *swapfile = "pagefile";

/* Static function declarations */
static void reset_frame_mask(uint32_t index);

static seL4_Word get_free_frame();

static struct app_cap *app_cap_new(seL4_CPtr cap, struct app_addrspace *addrspace, seL4_Word uaddr);

/* Static struct declarations */

/* val swappable | valid
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

/* Frame table */
static struct frame_entry *frame_table;

static uint64_t base_addr; /* Untyped region after end of frame table */
static int32_t low_addr;
static int32_t high_addr;

static uint32_t num_frames;

static struct frame_table_cap *frame_table_cap_head;

/* -1 no free_list but memory is not full
   -2 no free_list and memory is full (swapping later)*/
static int32_t free_index;

/* Swapping */
struct vnode *swap_vnode;

uint32_t swap_victim_index = 0;

static pthread_spinlock_t ft_lock;

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
    seL4_Word frame_vaddr = frame_index_to_vaddr(free_index);

    /* Reset the new frame mask */
    reset_frame_mask(free_index);
    
    /* Update free index */
    free_index = frame_table[free_index].next_index;

    /* Clear frame */
    memset(frame_vaddr, 0, PAGE_SIZE);

    return frame_vaddr;
}

int32_t unswappable_alloc(seL4_Word *vaddr) {
    int err = frame_alloc(vaddr);
    if (err) return err;

    /* Set unswappable */
    uint32_t index = frame_vaddr_to_index(*vaddr);
    frame_table[index].mask &= (~FRAME_SWAPPABLE);

    return 0;
}

int32_t swap_out() {
    int victim = swap_victim_index;
    for (int i = victim; ; i = (i + 1) % num_frames) {
        if ((frame_table[i].mask & FRAME_VALID) &&
            (frame_table[i].mask & FRAME_SWAPPABLE)) {

            if (frame_table[i].mask & FRAME_REFERENCE) {
                /* Clear reference */
                frame_table[i].mask &= (~FRAME_REFERENCE);
            } else {
                victim = i;
                swap_victim_index = (victim + 1) % num_frames;
                break;
            }

        }
    }

    seL4_Word frame_vaddr = frame_index_to_vaddr(victim);

    if (swap_vnode == NULL) {
        /* First time opening swapfile */
        vfs_open(swapfile, FM_READ | FM_WRITE, &swap_vnode);
    }

    /* Get swap offset */
    int swap_offset = get_swap_index();

    struct uio uio = {
        .vaddr = PAGE_ALIGN_4K(frame_vaddr),
        .uaddr = NULL,
        .size = PAGE_SIZE_4K,
        .offset = swap_offset * PAGE_SIZE_4K,
        .remaining = PAGE_SIZE_4K
    };
    
	seL4_Word uaddr = frame_table[victim].app_caps.uaddr;
    
    printf("Swap out - uaddr: %p, vaddr: %p, swap_index: %d\n", uaddr, frame_vaddr, swap_offset);

    struct PCB *proc = curproc;
    int err = swap_vnode->ops->vop_write(swap_vnode, &uio);
    curproc = proc;
    conditional_panic(err, "Could not write\n");

    err = sos_unmap_page(frame_vaddr, curproc->addrspace);
    conditional_panic(err, "Could not unmap\n");

    int index1 = root_index(uaddr);
    int index2 = leaf_index(uaddr);
    struct app_addrspace *as = frame_table[victim].app_caps.addrspace;

    /* Mark it swapped */
    as->page_table[index1][index2].sos_vaddr |= PTE_SWAP;
    as->swap_table[index1][index2].swap_index = swap_offset;

    frame_free(frame_vaddr); 
	
	seL4_ARM_Page_Unify_Instruction(get_cap(frame_vaddr), 0, PAGE_SIZE_4K);
	
	return 0;
}

int32_t swap_in(seL4_Word uaddr, seL4_Word sos_vaddr) {
    int index1 = root_index(uaddr);
    int index2 = leaf_index(uaddr);
    seL4_Word frame_index = frame_vaddr_to_index(sos_vaddr);

    /* Write page back in from pagefile */
    struct app_addrspace *as = curproc->addrspace;
    uint32_t swap_index = as->swap_table[index1][index2].swap_index;
    struct uio uio = {
        .vaddr = PAGE_ALIGN_4K(sos_vaddr),
        .uaddr = NULL,
        .size = PAGE_SIZE_4K,
        .offset = swap_index * PAGE_SIZE_4K,
        .remaining = PAGE_SIZE_4K
    };

    struct PCB *proc = curproc;
    int err = swap_vnode->ops->vop_read(swap_vnode, &uio);
    curproc = proc;
    conditional_panic(err, "Could not read\n");

    /* Mark page in pagefile as free */
    free_swap_index(swap_index);

    printf("Swap in - uaddr: %p, vaddr: %p, swap_index: %d\n", uaddr, sos_vaddr, swap_index);
		
    /* Mark it unswapped */
	seL4_Word mask = as->page_table[index1][index2].sos_vaddr & PAGE_MASK_4K;
    as->page_table[index1][index2].sos_vaddr = (sos_vaddr | PTE_VALID | mask) & (~PTE_SWAP);

    seL4_ARM_Page_Unify_Instruction(get_cap(sos_vaddr), 0, PAGE_SIZE_4K);

    return err;
}

int32_t frame_alloc(seL4_Word *vaddr) {
    int err;
    seL4_Word frame_vaddr;

    pthread_spin_lock(&ft_lock);
    if (free_index == -1) {
        /* Free list is empty but there is still memory */

        /* Get untyped memory */
        seL4_Word frame_paddr = ut_alloc(seL4_PageBits);

#ifdef LIMIT_FRAMES
        num_frames = 500;
        frames_to_alloc++;
        if (frames_to_alloc > num_frames || frame_paddr == NULL) {
#endif
#ifndef LIMIT_FRAMES
        if (frame_paddr == NULL) {
#endif
            /* Swapping */
            *vaddr = NULL;
            err = swap_out();
            conditional_panic(err, "Swap out failed\n");
            *vaddr = get_free_frame();
            pthread_spin_unlock(&ft_lock);
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
            pthread_spin_unlock(&ft_lock);
            return 2;
        }

        /* Map to address space */
        frame_vaddr = frame_paddr_to_vaddr(frame_paddr);
        err = map_page(frame_cap,
                seL4_CapInitThreadPD,
                frame_vaddr,
                seL4_AllRights,
                seL4_ARM_Default_VMAttributes);
        if (err) {
            cspace_delete_cap(cur_cspace, frame_cap);
            ut_free(frame_paddr, seL4_PageBits);
            *vaddr = NULL;
            pthread_spin_unlock(&ft_lock);
            return 3;
        }

        /* Calculate index of frame in the frame table */
        uint32_t index = frame_paddr_to_index(frame_paddr);
        reset_frame_mask(index);
        frame_table[index].cap = frame_cap;

    } else {
        /* Reuse a frame in the free list */
        frame_vaddr = get_free_frame();
    }
    pthread_spin_unlock(&ft_lock);

    /* Clear frame */
    memset(frame_vaddr, 0, PAGE_SIZE);

    *vaddr = frame_vaddr;
    return 0;
}

int32_t frame_free(seL4_Word vaddr) {
    uint32_t index = frame_vaddr_to_index(vaddr);

    /* Check that the frame was previously allocated */
    if (frame_table[index].cap == seL4_CapNull) return -1;

    /* Set free list index */
    frame_table[index].mask = 0;
    pthread_spin_lock(&ft_lock);
    frame_table[index].next_index = free_index;
    free_index = index;
    pthread_spin_unlock(&ft_lock);

    return 0;
}

seL4_CPtr get_cap(seL4_Word vaddr) {
    uint32_t index = frame_vaddr_to_index(PAGE_ALIGN_4K(vaddr));
    return frame_table[index].cap;
}

static struct app_cap *app_cap_new(seL4_CPtr cap, struct app_addrspace *addrspace, seL4_Word uaddr) {
    struct app_cap *new_app_cap = malloc(sizeof(struct app_cap));
    if (new_app_cap == NULL) {
        return NULL;
    }

    /* Initialise variables */
    new_app_cap->next = NULL;
    new_app_cap->addrspace = addrspace;
    new_app_cap->uaddr = uaddr;
    new_app_cap->cap = cap;

    return new_app_cap;
}

int32_t insert_app_cap(seL4_Word vaddr, seL4_CPtr cap, struct app_addrspace *addrspace, seL4_Word uaddr) {
    uint32_t index = frame_vaddr_to_index(vaddr);

    /* Check that the frame exists */
    if (frame_table[index].cap == seL4_CapNull) return -1;

    struct app_cap *copied_cap;

    /* Note: First app cap is not malloc'd */
    if (frame_table[index].app_caps.cap == seL4_CapNull) {
        /* First app cap */
        copied_cap = &frame_table[index].app_caps;
        copied_cap->next = NULL;
        copied_cap->addrspace = addrspace;
        copied_cap->uaddr = uaddr;
        copied_cap->cap = cap;
    } else {
        conditional_panic(1, "Does not currently support shared pages\n");
        /* /1 *Create new app cap *1/ */
        /* copied_cap = app_cap_new(cap, addrspace, uaddr); */
        /* if (copied_cap == NULL) return -1; */

        /* /1 *Insert into list of app caps for the frame *1/ */
        /* //TODO just insert to head */
        /* struct app_cap *curr_cap = copied_cap; */
        /* while (curr_cap->next != NULL) { */
        /* curr_cap = curr_cap->next; */
        /* } */
        /* curr_cap->next = copied_cap; */
    }

    return 0;
}

int32_t get_app_cap(seL4_Word vaddr,
                    struct app_addrspace *as,
                    struct app_cap **cap_ret) {
    struct page_table_entry **page_table = as->page_table;

    uint32_t index = frame_vaddr_to_index(vaddr);
    if (frame_table[index].cap == seL4_CapNull) {
        return -1;
    }

    struct app_cap *curr_cap = &frame_table[index].app_caps;
    /*Doesn't support shared pages right now
     * while (curr_cap != NULL) {
     if ((curr_cap->pte.sos_vaddr & PAGE_TABLE_MASK) == page_table) breaki;
     printf("%x----%x\n", curr_cap->pte.sos_vaddr, page_table);
     curr_cap = curr_cap->next;
     }
     */
    if (curr_cap == NULL) {
        return -1;
    } else {
        *cap_ret = curr_cap;
        return 0;
    }
}

/* Set reference bit as well as make it unswappable */ 
void pin_frame_entry(seL4_Word uaddr, seL4_Word size) {
    if (size <= 0) return;

    int index1;
    int index2;
    seL4_Word sos_vaddr;
    uint32_t frame_index;
    struct app_addrspace *as = curproc->addrspace;

    int internal_offset = uaddr - PAGE_ALIGN_4K(uaddr);
    int end_frame_size = size + internal_offset;

    for (int i = 0; i < end_frame_size; i += PAGE_SIZE_4K) {
        index1 = root_index(uaddr + i);
        index2 = leaf_index(uaddr + i);
        sos_vaddr = as->page_table[index1][index2].sos_vaddr;
        if ((sos_vaddr & PTE_SWAP) || (sos_vaddr & PTE_VALID) == 0) continue;
        frame_index = frame_vaddr_to_index(sos_vaddr);

        if ((frame_table[frame_index].mask & FRAME_VALID) &&
            (frame_table[frame_index].mask & FRAME_SWAPPABLE)) {
            frame_table[frame_index].mask |= FRAME_REFERENCE;
			frame_table[frame_index].mask &= (~FRAME_SWAPPABLE);
        }
    }
}

void unpin_frame_entry(seL4_Word uaddr, seL4_Word size) {
    if (size <= 0) return;

    int index1;
    int index2;
    seL4_Word sos_vaddr;
    uint32_t frame_index;
    struct app_addrspace *as = curproc->addrspace;

    int internal_offset = uaddr - PAGE_ALIGN_4K(uaddr);
    int end_frame_size = size + internal_offset;

    for (int i = 0; i < end_frame_size; i += PAGE_SIZE_4K) {
        index1 = root_index(uaddr + i);
        index2 = leaf_index(uaddr + i);
        sos_vaddr = as->page_table[index1][index2].sos_vaddr;
        if ((sos_vaddr & PTE_SWAP) || (sos_vaddr & PTE_VALID) == 0) continue;
        frame_index = frame_vaddr_to_index(sos_vaddr);

        if ((frame_table[frame_index].mask & FRAME_VALID)) {
            frame_table[frame_index].mask |= FRAME_SWAPPABLE;
        }
    }
}



static void reset_frame_mask(uint32_t index) {
    frame_table[index].mask = FRAME_SWAPPABLE | FRAME_VALID | FRAME_REFERENCE;
}

inline int root_index(seL4_Word uaddr) {
    return (uaddr >> 22);
}

inline int leaf_index(seL4_Word uaddr) {
    return ((uaddr << 10) >> 22);
}

inline uint32_t frame_vaddr_to_index(seL4_Word sos_vaddr) {
    return ((sos_vaddr - PROCESS_VMEM_START + low_addr - base_addr) >> INDEX_ADDR_OFFSET);
}

inline seL4_Word frame_index_to_vaddr(uint32_t index) {
    return ((index << INDEX_ADDR_OFFSET) + base_addr - low_addr + PROCESS_VMEM_START);
}

inline seL4_Word frame_paddr_to_vaddr(seL4_Word paddr) {
    return (paddr - low_addr + PROCESS_VMEM_START);
}

inline uint32_t frame_paddr_to_index(seL4_Word paddr) {
    return ((paddr - base_addr) >> INDEX_ADDR_OFFSET);
}
