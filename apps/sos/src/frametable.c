#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <cspace/cspace.h>
#include <utils/page.h>
#include <fcntl.h>

#include "vmem_layout.h"
#include "ut_manager/ut.h"
#include "mapping.h"
#include "process.h"
#include "vnode.h"
#include "frametable.h"
#include "mapping.h"

#include <sys/panic.h>


/* Emulate limited frames */
//#define LIMIT_FRAMES

#ifdef LIMIT_FRAMES
#define MAX_FRAMES 500
int frames_to_alloc = 0;
#endif


#define PAGE_SIZE 4096lu /* In bytes */
#define INDEX_ADDR_OFFSET 12 /* Bits to shift to get change between index and address */
#define EMPTY_FREELIST -1 /* Free list is empty */

/* Frame table entry bits */
#define FRAME_VALID (1 << 0)
#define FRAME_SWAPPABLE (1 << 1)
#define FRAME_REFERENCE (1 << 2)
#define FRAME_PID_MASK (~7)
#define PID_SHIFT 3

extern struct PCB *curproc;

/* Name of swapfile */
const char *swapfile = "pagefile";

/* Static struct declarations */

/*
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
    seL4_CPtr cap;
    struct frame_table_cap *next;
};

/* Frame table */
static struct frame_entry *frame_table;
static uint64_t base_addr; /* Start of untyped region after end of frame table */
static int32_t low_addr; /* Low addr of memory */
static int32_t high_addr; /* High addr of memory */
static uint32_t num_frames; /* Frames in the frametable */
static struct frame_table_cap *frame_table_cap_head; /* Linked list of the actual frametable's caps */

/* Swapping */
struct vnode *swap_vnode;
static uint32_t swap_victim_index = 0;

/* 0 >= Index of free frame from freelist
   -1 = EMPTY_FREELIST = Nothing in freelist (Will allocate new memory) */
static int32_t free_index;

static void reset_frame_mask(uint32_t index);
static seL4_Word get_free_frame();

static inline uint32_t frame_vaddr_to_index(seL4_Word sos_vaddr);
static inline seL4_Word frame_index_to_vaddr(uint32_t index);

static inline seL4_Word frame_paddr_to_vaddr(seL4_Word paddr);
static inline uint32_t frame_paddr_to_index(seL4_Word paddr);


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
     * To find this, we solve for base_address using:
     * (base_address - low) / sizeof(entry) >= (high - base_address) / PAGE_SIZE
     */
    base_addr = (high64 * entry_size + PAGE_SIZE * low64) / (entry_size + PAGE_SIZE);
    low_addr = low;
    high_addr = high;

    /* Calculate frame table size */
    uint64_t frame_table_size = base_addr - low64;
    uint64_t num_entries = frame_table_size / entry_size;
    uint64_t num_pages = (high64 - base_addr) / PAGE_SIZE;

    /* Make sure num_entries >= num_pages (May need one extra frame entry) */
    num_frames = num_entries;
    printf("Base: %llu, oldbase: %llu\n", base_addr, low64 + frame_table_size);
    printf("entries: %llu, pages: %llu\n", num_entries, num_pages);
    if (num_entries < num_pages) {
        frame_table_size += entry_size;
    }
    printf("Base: %llu, newbase: %llu\n", base_addr, low64 + frame_table_size);
    base_addr = low;

    /* Allocated each section of the frame table */
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
        conditional_panic(err, "Failed to allocate initial frame table cap");

        /* Map to address space */
        err = map_page(cap,
                seL4_CapInitThreadPD,
                ft_section_vaddr,
                seL4_AllRights,
                seL4_ARM_Default_VMAttributes);
        conditional_panic(err, "Failed to map initial frame table");

        /* Set pointer to head of frame_table and keep track of caps */
        struct frame_table_cap *cap_holder = malloc(sizeof(struct frame_table_cap));
        conditional_panic(cap_holder == NULL, "Failed to allocate initial frame cap holder\n");
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
    free_index = EMPTY_FREELIST;
}

/* Allocate a frame which is unswappable */
int32_t unswappable_alloc(seL4_Word *vaddr) {
    int err = frame_alloc(vaddr);
    if (err) return err;
    
    /* Set unswappable */
    uint32_t index = frame_vaddr_to_index(*vaddr);
    frame_table[index].mask &= (~FRAME_SWAPPABLE);

    return 0;
}

/* Swap out a frame to backing store so it can be reused */
int32_t swap_out() {
    /* Find a victim to swap out */
    int victim = swap_victim_index;
    for (int i = victim; ; i = (i + 1) % num_frames) {
        if ((frame_table[i].mask & FRAME_VALID) &&
            (frame_table[i].mask & FRAME_SWAPPABLE)) {

            if (frame_table[i].mask & FRAME_REFERENCE) {
                /* Clear reference */
                frame_table[i].mask &= (~FRAME_REFERENCE);
            } else {
                /* Found victim */
                victim = i;
                swap_victim_index = (victim + 1) % num_frames;
                break;
            }

        }
    }

    seL4_Word frame_vaddr = frame_index_to_vaddr(victim);

    /* Temporarily mark frame as unswappable because it is being swapped out */
    frame_table[victim].mask &= (~FRAME_SWAPPABLE);

    if (swap_vnode == NULL) {
        /* First time opening swapfile */
        int err = vfs_open(swapfile, FM_READ | FM_WRITE, &swap_vnode);
        if (err) return -1;
    }

    /* Get swap offset */
    int swap_offset = get_swap_index();
    if (swap_offset < 0) {
        return -1;
    }

	seL4_Word uaddr = frame_table[victim].app_caps.uaddr;

    printf("Swap out - uaddr: %p, vaddr: %p, swap_index: %d\n", uaddr, frame_vaddr, swap_offset);

    /* Update details of old addrspace */
    int index1 = root_index(uaddr);
    int index2 = leaf_index(uaddr);

    struct PCB *pcb = frame_table[victim].app_caps.pcb;
    int pid = pcb->pid;
    struct app_addrspace *as = pcb->addrspace;

    /* Mark it as swapped out */
    as->page_table[index1][index2].sos_vaddr |= PTE_SWAP;
    as->page_table[index1][index2].sos_vaddr |= PTE_BEINGSWAPPED;
    as->swap_table[index1][index2].swap_index = swap_offset;
    
    sos_unmap_page(frame_vaddr, as);

    struct uio uio = {
        .vaddr = PAGE_ALIGN_4K(frame_vaddr),
        .uaddr = NULL,
        .size = PAGE_SIZE_4K,
        .offset = swap_offset * PAGE_SIZE_4K,
        .remaining = PAGE_SIZE_4K
    };

    unsigned int stime = pcb->stime;

    /* Swap frame out */
    int err = swap_vnode->ops->vop_write(swap_vnode, &uio);
    if (err) {
        as->page_table[index1][index2].sos_vaddr &= (~PTE_SWAP);
        as->page_table[index1][index2].sos_vaddr &= (~PTE_BEINGSWAPPED);

        seL4_ARM_PageDirectory pd = pcb->vroot;
        struct app_addrspace *as = pcb->addrspace;
        seL4_CPtr cap = get_cap(frame_vaddr);
        seL4_CPtr copied_cap;
        copied_cap = cspace_copy_cap(cur_cspace,
                cur_cspace,
                cap,
                seL4_AllRights);

        struct region *curr_region = as->regions;
        while (curr_region != NULL) {
            if (uaddr >= curr_region->baseaddr &&
                    uaddr < curr_region->baseaddr + curr_region->size) {
                break;
            }
            curr_region = curr_region->next;
        }
        err = map_page(copied_cap,
                pd,
                uaddr,
                curr_region->permissions,
                seL4_ARM_Default_VMAttributes);

        /* Book keeping the copied caps */
        insert_app_cap(PAGE_ALIGN_4K(frame_vaddr),
                copied_cap,
                pcb,
                uaddr);

        return -1;
    }

    /* Remark frame as swappable */
    frame_table[victim].mask |= FRAME_SWAPPABLE;

    if (!is_still_valid_proc(pid, stime)) {
        /* Process was destroyed */
        frame_free(frame_vaddr);
        seL4_ARM_Page_Unify_Instruction(get_cap(frame_vaddr), 0, PAGE_SIZE_4K);
        return 0;
    }

    if (as->page_table[index1][index2].sos_vaddr & PTE_BEINGSWAPPED) {
        as->page_table[index1][index2].sos_vaddr &= (~PTE_BEINGSWAPPED);
    } else {
        int pid = frame_table[victim].mask >> PID_SHIFT;
        struct PCB *pcb = process_status(pid);
        set_resume(pcb->coroutine_id);
    }

    frame_free(frame_vaddr); 
	
	seL4_ARM_Page_Unify_Instruction(get_cap(frame_vaddr), 0, PAGE_SIZE_4K);
	
	return 0;
}

/* Swap in a frame from backing store */
int32_t swap_in(seL4_Word uaddr, seL4_Word sos_vaddr) {
    int index1 = root_index(uaddr);
    int index2 = leaf_index(uaddr);
    seL4_Word frame_index = frame_vaddr_to_index(sos_vaddr);

    /* Write page back in from swapfile */
    struct app_addrspace *as = curproc->addrspace;
    uint32_t swap_index = as->swap_table[index1][index2].swap_index;

    struct uio uio = {
        .vaddr = PAGE_ALIGN_4K(sos_vaddr),
        .uaddr = NULL,
        .size = PAGE_SIZE_4K,
        .offset = swap_index * PAGE_SIZE_4K,
        .remaining = PAGE_SIZE_4K,
        .pcb = curproc
    };

    if (as->page_table[index1][index2].sos_vaddr & PTE_BEINGSWAPPED) {
        frame_table[frame_index].mask &= (~FRAME_PID_MASK);
        frame_table[frame_index].mask |= (curproc->pid << PID_SHIFT);
        as->page_table[index1][index2].sos_vaddr &= (~PTE_BEINGSWAPPED);
        yield();
    }

    /* Swap in */
    int err = swap_vnode->ops->vop_read(swap_vnode, &uio);
    if (err) {
        return -1;
    }

    printf("Swap in - uaddr: %p, vaddr: %p, swap_index: %d\n", uaddr, sos_vaddr, swap_index);

    /* Mark it unswapped */
	seL4_Word mask = as->page_table[index1][index2].sos_vaddr & PAGE_MASK_4K;
    as->page_table[index1][index2].sos_vaddr = (sos_vaddr | PTE_VALID | mask) & (~PTE_SWAP);

    /* Mark page in swapfile as free */
    free_swap_index(swap_index);

    seL4_ARM_Page_Unify_Instruction(get_cap(sos_vaddr), 0, PAGE_SIZE_4K);

    return 0;
}

int32_t frame_alloc(seL4_Word *vaddr) {
    int err;
    seL4_Word frame_vaddr;

    /* Initially set default return value as NULL */
    *vaddr = NULL;

    if (free_index == EMPTY_FREELIST) {
        /* Get untyped memory */
        seL4_Word frame_paddr = ut_alloc(seL4_PageBits);

#ifdef LIMIT_FRAMES
        /* Emulate limited frames up to MAX_FRAMES */
        num_frames = MAX_FRAMES;
        frames_to_alloc++;
        if (frames_to_alloc > num_frames || frame_paddr == NULL) {
#endif

#ifndef LIMIT_FRAMES
        if (frame_paddr == NULL) {
#endif
            /* Out of memory, swap out a frame */
            err = swap_out();
            if (err) return -1;
            
            /* Note: get_free_frame() will not return NULL
             *       since swap_out frees a frame
             */
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
            return -1;
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
            return -1;
        }

        /* Update frame details */
        uint32_t index = frame_paddr_to_index(frame_paddr);
        reset_frame_mask(index);
        frame_table[index].cap = frame_cap;

    } else {
        /* Reuse a frame in the freelist */
        frame_vaddr = get_free_frame();
    }

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
    frame_table[index].next_index = free_index;
    free_index = index;

    return 0;
}

/* get free frame from free list */
static seL4_Word get_free_frame() {
    if (free_index == EMPTY_FREELIST) return NULL;

    seL4_Word frame_vaddr = frame_index_to_vaddr(free_index);

    /* Reset the new frame mask */
    reset_frame_mask(free_index);
    
    /* Update free index */
    free_index = frame_table[free_index].next_index;

    /* Clear frame */
    memset(frame_vaddr, 0, PAGE_SIZE);

    return frame_vaddr;
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

        if (as->page_table[index1] == NULL) continue;

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

        if (as->page_table[index1] == NULL) continue;

        sos_vaddr = as->page_table[index1][index2].sos_vaddr;
        if ((sos_vaddr & PTE_SWAP) || (sos_vaddr & PTE_VALID) == 0) continue;

        frame_index = frame_vaddr_to_index(sos_vaddr);
        if ((frame_table[frame_index].mask & FRAME_VALID)) {
            frame_table[frame_index].mask |= FRAME_SWAPPABLE;
        }
    }
}


seL4_CPtr get_cap(seL4_Word vaddr) {
    uint32_t index = frame_vaddr_to_index(PAGE_ALIGN_4K(vaddr));
    return frame_table[index].cap;
}

int32_t insert_app_cap(seL4_Word vaddr, seL4_CPtr cap,
        struct PCB *pcb, seL4_Word uaddr) {

    uint32_t index = frame_vaddr_to_index(vaddr);

    /* Check that the frame exists */
    if (frame_table[index].cap == seL4_CapNull) return -1;

    struct app_cap *copied_cap;

    /* Note: First app cap is not malloc'd */
    if (frame_table[index].app_caps.cap == seL4_CapNull) {
        /* First app cap */
        copied_cap = &frame_table[index].app_caps;
        copied_cap->next = NULL;
        copied_cap->pcb = pcb;
        copied_cap->uaddr = uaddr;
        copied_cap->cap = cap;
    } else {
        conditional_panic(1, "Does not currently support shared pages\n");
    }

    return 0;
}

int32_t get_app_cap(seL4_Word vaddr,
        struct app_addrspace *as,
        struct app_cap **cap_ret) {

    struct page_table_entry **page_table = as->page_table;

    uint32_t index = frame_vaddr_to_index(vaddr);
    if (frame_table[index].cap == seL4_CapNull) return -1;

    struct app_cap *curr_cap = &frame_table[index].app_caps;

    if (curr_cap->cap == seL4_CapNull) {
        return -1;
    } else {
        *cap_ret = curr_cap;
        return 0;
    }
}



static void reset_frame_mask(uint32_t index) {
    frame_table[index].mask = FRAME_SWAPPABLE | FRAME_VALID | FRAME_REFERENCE;
}

static inline uint32_t frame_vaddr_to_index(seL4_Word sos_vaddr) {
    return ((sos_vaddr - PROCESS_VMEM_START + low_addr - base_addr) >> INDEX_ADDR_OFFSET);
}

static inline seL4_Word frame_index_to_vaddr(uint32_t index) {
    return ((index << INDEX_ADDR_OFFSET) + base_addr - low_addr + PROCESS_VMEM_START);
}

static inline seL4_Word frame_paddr_to_vaddr(seL4_Word paddr) {
    return (paddr - low_addr + PROCESS_VMEM_START);
}

static inline uint32_t frame_paddr_to_index(seL4_Word paddr) {
    return ((paddr - base_addr) >> INDEX_ADDR_OFFSET);
}
