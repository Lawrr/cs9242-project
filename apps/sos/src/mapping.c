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
#include "vmem_layout.h"
#include "addrspace.h"
#include "frametable.h"

#define verbose 0
#include <sys/panic.h>
#include <sys/debug.h>
#include <cspace/cspace.h>

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

int 
sos_map_page(seL4_Word vaddr, seL4_ARM_PageDirectory pd, struct app_addrspace *as, seL4_Word *sos_vaddr_ret) {
    int err;
    // Get the addr to simplify later implementation
    struct page_table_entry ***page_table_vaddr = &(as->page_table);
    
    // Invalid mapping NULL 
    if (vaddr == NULL) {
        conditional_panic(-1, "Mapping NULL virtual address");
    }

    seL4_Word index1 = vaddr >> 22;
    seL4_Word index2 = (vaddr << 10) >> 22;

    // Checking with the region the check the permission
    struct region *curr_region = as->regions;
    while (curr_region != NULL) {
        if (vaddr >= curr_region->baseaddr &&
            vaddr < curr_region->baseaddr + curr_region->size) {
	    break;
          
        }

        curr_region = curr_region->next;
    } 

    // Can't find the region that contains thisvaddr
    if (curr_region == NULL) {
        // Simply conditional panic for now
        conditional_panic(-1, "No region contains this vaddr");
    }

    // No page table yet
    if (*page_table_vaddr == NULL) {
        // First level
        err = frame_alloc(page_table_vaddr);
        conditional_panic(err, "No memory for new Shadow Page Directory");

        // Second level
        err = frame_alloc(&(*page_table_vaddr)[index1]);
        conditional_panic(err, "No memory for new Shadow Page Directory");

    } else if ((*page_table_vaddr)[index1] == NULL) {
        // Second level
        err = frame_alloc(&(*page_table_vaddr)[index1]);
        conditional_panic(err,"No memory for new Shadow Page Directory");

    }

    // Call the internal kernel page mapping 
    seL4_Word sos_vaddr;
    err = frame_alloc(&sos_vaddr);	
    conditional_panic(err, "Probably insufficient memory");
    *sos_vaddr_ret = sos_vaddr;

    //This function would not fail if it pass the conditional paninc above. No need to check.
    printf("Mapping %x to %x\n", sos_vaddr, (vaddr >> 12) << 12);
    seL4_Word cap = get_cap(sos_vaddr);
    seL4_Word copied_cap = cspace_copy_cap(cur_cspace,
                                           cur_cspace,
                                           cap,
                                           seL4_AllRights);

    err = map_page(copied_cap,
		           pd,
		           (vaddr >> 12) << 12,
		           curr_region->permissions,
		           seL4_ARM_Default_VMAttributes);
    if (err) printf("Error: %d\n", err);
    conditional_panic(err, "Internal map_page fail");

    insert_app_cap((sos_vaddr >> 12)<<12, copied_cap);
    struct page_table_entry pte = {((sos_vaddr>>12) <<12)|curr_region -> permissions|PTE_VALID};
    (*page_table_vaddr)[index1][index2] = pte;
    return err;
}
