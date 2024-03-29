/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#include <elf/elf.h>

#include "mapping.h"

#include <ut_manager/ut.h>
#include <utils/page.h>

#include "vmem_layout.h"
#include "addrspace.h"
#include "frametable.h"
#include "process.h"

#include <sys/panic.h>
#include <sys/debug.h>
#include <cspace/cspace.h>

extern const seL4_BootInfo *_boot_info;
extern struct PCB *curproc;
extern uint32_t curr_swap_offset;

/**
 * Maps a page table into the root servers page directory
 * @param vaddr The virtual address of the mapping
 * @return 0 on success
 */
static int
_map_page_table(seL4_ARM_PageDirectory pd, seL4_Word vaddr) {
    seL4_Word pt_addr;
    seL4_ARM_PageTable pt_cap;
    int err;

    /* Allocate a PT object */
    pt_addr = ut_alloc(seL4_PageTableBits);
    if(pt_addr == 0) {
        return !0;
    }
    /* Create the frame cap */
    err = cspace_ut_retype_addr(pt_addr,
            seL4_ARM_PageTableObject,
            seL4_PageTableBits,
            cur_cspace,
            &pt_cap);
    if(err) {
        return !0;
    }
    /* Tell seL4 to map the PT in for us */
    err = seL4_ARM_PageTable_Map(pt_cap,
            pd,
            vaddr,
            seL4_ARM_Default_VMAttributes);
    return err;
}

int
map_page(seL4_CPtr frame_cap, seL4_ARM_PageDirectory pd, seL4_Word vaddr,
        seL4_CapRights rights, seL4_ARM_VMAttributes attr) {
    int err;

    /* Attempt the mapping */
    err = seL4_ARM_Page_Map(frame_cap, pd, vaddr, rights, attr);
    if(err == seL4_FailedLookup) {
        /* Assume the error was because we have no page table */
        err = _map_page_table(pd, vaddr);
        if(!err) {
            /* Try the mapping again */
            err = seL4_ARM_Page_Map(frame_cap, pd, vaddr, rights, attr);
        }
    }

    return err;
}

void*
map_device(void *paddr, int size) {
    static seL4_Word virt = DEVICE_START;
    seL4_Word phys = (seL4_Word)paddr;
    seL4_Word vstart = virt;

    while(virt - vstart < size) {
        seL4_Error err;
        seL4_ARM_Page frame_cap;
        /* Retype the untype to a frame */
        err = cspace_ut_retype_addr(phys,
                seL4_ARM_SmallPageObject,
                seL4_PageBits,
                cur_cspace,
                &frame_cap);
        conditional_panic(err, "Unable to retype device memory");
        /* Map in the page */
        err = map_page(frame_cap,
                seL4_CapInitThreadPD,
                virt,
                seL4_AllRights,
                0);
        conditional_panic(err, "Unable to map device");
        /* Next address */
        phys += (1 << seL4_PageBits);
        virt += (1 << seL4_PageBits);
    }
    return (void*)vstart;
}

int sos_unmap_page(seL4_Word vaddr, struct app_addrspace *as) {
    struct app_cap *cap;

    int err = get_app_cap(PAGE_ALIGN_4K(vaddr), as, &cap);
    if (err) return err;

    err = seL4_ARM_Page_Unmap(cap->cap);
    if (err) return err;

    err = cspace_delete_cap(cur_cspace, cap->cap);

    cap->cap = seL4_CapNull;
    return err;
}

int
sos_map_page(seL4_Word uaddr_unaligned, seL4_Word *sos_vaddr_ret, struct PCB *pcb) {
    seL4_ARM_PageDirectory pd = pcb->vroot;
    struct app_addrspace *as = pcb->addrspace;
    int err;

    seL4_Word uaddr = PAGE_ALIGN_4K(uaddr_unaligned);
    /* Get the addr to simplify later implementation */
    struct page_table_entry ***page_table = &(as->page_table);
    struct swap_table_entry ***swap_table = &(as->swap_table);
    /* Invalid mapping NULL */
    if (((void *) uaddr) == NULL) {
        return ERR_INVALID_ADDR;
    }

    int index1 = root_index(uaddr);
    int index2 = leaf_index(uaddr);

    /* Checking with the region the check the permission */
    struct region *curr_region = as->regions;
    while (curr_region != NULL) {
        if (uaddr_unaligned >= curr_region->baseaddr &&
                uaddr_unaligned < curr_region->baseaddr + curr_region->size) {
            break;
        }
        curr_region = curr_region->next;
    }

    /* Can't find the region that contains this uaddr */
    if (curr_region == NULL) {
        return ERR_INVALID_REGION;
    }

    /* No page table yet */
    if (*page_table == NULL) {
        /* First level */
        err = unswappable_alloc((seL4_Word *) page_table);
        if (err) return ERR_NO_MEMORY;

        err = unswappable_alloc((seL4_Word *) swap_table);
        if (err) {
            frame_free(page_table);
            return ERR_NO_MEMORY;
        }

        /* Second level */
        err = unswappable_alloc((seL4_Word *) &(*page_table)[index1]);

        if (err) {
            frame_free(page_table);
            frame_free(swap_table);
            return ERR_NO_MEMORY;
        }

        err = unswappable_alloc((seL4_Word *) &(*swap_table)[index1]);
        if (err) {
            frame_free(page_table);
            frame_free(swap_table);
            frame_free(&(*page_table)[index1]);
            return ERR_NO_MEMORY;
        }

    } else if ((*page_table)[index1] == NULL) {
        /* Second level */
        err = unswappable_alloc((seL4_Word *) &(*page_table)[index1]);
        if (err) return ERR_NO_MEMORY;

        err = unswappable_alloc((seL4_Word *) &(*swap_table)[index1]);
        if (err) {
            frame_free(&(*page_table)[index1]);
            return ERR_NO_MEMORY;
        }
    }

    seL4_Word curr_sos_vaddr = (*page_table)[index1][index2].sos_vaddr;
    if ((seL4_Word *) curr_sos_vaddr != NULL) {
        if ((curr_sos_vaddr & PTE_SWAP) == 0) {
            /* Already mapped */
            *sos_vaddr_ret = (*page_table)[index1][index2].sos_vaddr;
            return ERR_ALREADY_MAPPED;
        }
    }

    /* Call the internal kernel page mapping */
    seL4_Word new_frame_vaddr;
    if (uaddr >= PROCESS_IPC_BUFFER) {
        err = unswappable_alloc(&new_frame_vaddr);
    } else {
        err = frame_alloc(&new_frame_vaddr);
    }
    if (err) {
        return ERR_NO_MEMORY;
    }

    seL4_CPtr cap = get_cap(new_frame_vaddr);
    seL4_CPtr copied_cap;
    copied_cap = cspace_copy_cap(cur_cspace,
            cur_cspace,
            cap,
            seL4_AllRights);

    err = map_page(copied_cap,
            pd,
            uaddr,
            curr_region->permissions,
            seL4_ARM_Default_VMAttributes);
    if (err) {
        cspace_delete_cap(cur_cspace, copied_cap);
        frame_free(new_frame_vaddr);
        return ERR_INTERNAL_MAP_ERROR;
    }

    /* Book keeping the copied caps */
    insert_app_cap(PAGE_ALIGN_4K(new_frame_vaddr),
            copied_cap,
            pcb,
            uaddr);
   
    if ((*page_table)[index1][index2].sos_vaddr & PTE_BEINGSWAPPED) {
        set_fe_pid(PAGE_ALIGN_4K(curr_sos_vaddr),pcb->pid); 
        as->page_table[index1][index2].sos_vaddr &= (~PTE_BEINGSWAPPED);
        yield();
    }


    /* Reassign in case mask changed during alloc (BEINGSWAPPED) */
    curr_sos_vaddr = (*page_table)[index1][index2].sos_vaddr;

    /* Book keeping in our own page table */
    int mask = (curr_sos_vaddr << 20) >> 20;
    if (mask == 0) {
        mask = (curr_region->permissions | PTE_VALID);
    }
    struct page_table_entry pte = {PAGE_ALIGN_4K(new_frame_vaddr) | mask};
    (*page_table)[index1][index2] = pte;

    if (pte.sos_vaddr & PTE_SWAP) {
        swap_in(uaddr, PAGE_ALIGN_4K(new_frame_vaddr));
    }

    *sos_vaddr_ret = new_frame_vaddr;
    pcb->addrspace->page_count += 1;
    return 0;
}

inline seL4_Word uaddr_to_sos_vaddr(seL4_Word uaddr) {
    int index1 = root_index(uaddr);
    int index2 = leaf_index(uaddr);
    struct app_addrspace *as = curproc->addrspace;

    if (as->page_table[index1] == NULL) return 0;

    if (as->page_table[index1][index2].sos_vaddr | PTE_VALID) {
        return as->page_table[index1][index2].sos_vaddr;
    } else {
        return 0;
    }
}
