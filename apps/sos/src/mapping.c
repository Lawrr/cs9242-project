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

#define verbose 0
#include <sys/panic.h>
#include <sys/debug.h>
#include <cspace/cspace.h>
#define INDEX1_SHIFT 22 
#define INDEX2_MASK (1023 << PAGE_BITS_4K)

extern const seL4_BootInfo* _boot_info;

/**
 * Maps a page table into the root servers page directory
 * @param vaddr The virtual address of the mapping
 * @return 0 on success
 */
static int 
_map_page_table(seL4_ARM_PageDirectory pd, seL4_Word vaddr){
    seL4_Word pt_addr;
    seL4_ARM_PageTable pt_cap;
    int err;

    /* Allocate a PT object */
    pt_addr = ut_alloc(seL4_PageTableBits);
    if(pt_addr == 0){
        return !0;
    }
    /* Create the frame cap */
    err =  cspace_ut_retype_addr(pt_addr, 
                                 seL4_ARM_PageTableObject,
                                 seL4_PageTableBits,
                                 cur_cspace,
                                 &pt_cap);
    if(err){
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
                seL4_CapRights rights, seL4_ARM_VMAttributes attr){
    int err;

    /* Attempt the mapping */
    err = seL4_ARM_Page_Map(frame_cap, pd, vaddr, rights, attr);
    if(err == seL4_FailedLookup){
        /* Assume the error was because we have no page table */
        err = _map_page_table(pd, vaddr);
        if(!err){
            /* Try the mapping again */
            err = seL4_ARM_Page_Map(frame_cap, pd, vaddr, rights, attr);
        }
    }

    return err;
}

void* 
map_device(void* paddr, int size){
    static seL4_Word virt = DEVICE_START;
    seL4_Word phys = (seL4_Word)paddr;
    seL4_Word vstart = virt;

    dprintf(1, "Mapping device memory 0x%x -> 0x%x (0x%x bytes)\n",
                phys, vstart, size);
    while(virt - vstart < size){
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
/*
int sos_ummap_page(seL4_Word vaddr, seL4_Word asid) {
    seL4_CPtr cap;
    int err = get_app_cap((vaddr>>PAGE_BITS_4K) << PAGE_BITS_4K, asid, &cap); 
    if (err != 0) return err;
    err = seL4_ARM_Page_Unmap(cap);
    if (err != 0) return err;
    cspace_delete_cap(cur_cspace, cap);
    return err;
}*/

int 
sos_map_page(seL4_Word vaddr_unaligned, seL4_ARM_PageDirectory pd, struct app_addrspace *as, seL4_Word *sos_vaddr_ret,seL4_CPtr*app_cap) {
    int err;
    seL4_Word vaddr = PAGE_ALIGN_4K(vaddr_unaligned);
    /* Get the addr to simplify later implementation */
    struct page_table_entry ***page_table_vaddr = &(as->page_table);
    
    /* Invalid mapping NULL */
    if (((void *) vaddr) == NULL) {
        return -1;
    }

    seL4_Word index1 = vaddr >> INDEX1_SHIFT;
    seL4_Word index2 = (vaddr & INDEX2_MASK) >> PAGE_BITS_4K;

    /* Checking with the region the check the permission */
    struct region *curr_region = as->regions;
    while (curr_region != NULL) {
        if (vaddr_unaligned >= curr_region->baseaddr &&
            vaddr_unaligned < curr_region->baseaddr + curr_region->size) {
            break;
        }
        curr_region = curr_region->next;
    }

    /* Can't find the region that contains thisvaddr */
    if (curr_region == NULL) {
        printf("%x\n", vaddr);
        return -1;
    }

    /* No page table yet */
    if (*page_table_vaddr == NULL) {
        /* First level */
        err = frame_alloc(page_table_vaddr);
        if (err) {
            return err;
        }

        /* Second level */
        err = frame_alloc(&(*page_table_vaddr)[index1]);
        if (err) {
            return err;
        }

    } else if ((*page_table_vaddr)[index1] == NULL) {
        /* Second level */
        err = frame_alloc(&(*page_table_vaddr)[index1]);
        if (err) {
            return err;
        }
    }

    if ((*page_table_vaddr)[index1][index2].sos_vaddr != NULL) {
        return -1;
    }

    /* Call the internal kernel page mapping */
    seL4_Word sos_vaddr;
    err = frame_alloc(&sos_vaddr);	
    if (err) {
        return err;
    }

    seL4_Word cap = get_cap(sos_vaddr);

    seL4_Word copied_cap = cspace_copy_cap(cur_cspace,
                                           cur_cspace,
                                           cap,
                                           seL4_AllRights);

    err = map_page(copied_cap,
		           pd,
		           (vaddr >> PAGE_BITS_4K) << PAGE_BITS_4K,
		           curr_region->permissions,
		           seL4_ARM_Default_VMAttributes);
    if (err) {
        cspace_delete_cap(cur_cspace, copied_cap);
        frame_free(sos_vaddr);
        return err;
    }
    
    /* Book keeping the copied caps */
    insert_app_cap(PAGE_ALIGN_4K(sos_vaddr), copied_cap, &(*page_table_vaddr)[index1][index2]);
    
    /* Book keeping in our own page table */
    struct page_table_entry pte = {PAGE_ALIGN_4K(sos_vaddr) |
                                   (curr_region->permissions | PTE_VALID)};
    (*page_table_vaddr)[index1][index2] = pte;
    *app_cap = copied_cap;
    *sos_vaddr_ret = sos_vaddr;
    return 0;
}
