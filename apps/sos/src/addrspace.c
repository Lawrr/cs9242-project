#include <stdio.h>
#include <stdlib.h>
#include <cspace/cspace.h>
#include <utils/page.h>
#include "addrspace.h"
#include "ut_manager/ut.h"
#include "frametable.h"
#include "process.h"
#include "file.h"
#include "sos.h"

extern struct PCB *curproc;
extern struct oft_entry * of_table;
extern seL4_Word curr_free_ofd;
extern seL4_Word ofd_count;
struct app_addrspace *as_new() {
    struct app_addrspace *as = malloc(sizeof(struct app_addrspace));
    if (as == NULL) {
        return NULL;
    }

    /* Initialise addrspace variables */
    as->regions = NULL;
    as->page_table = CSPACE_NULL;

    /* File descriptors */
    as->fdt_status = 3 << TWO_BYTE_BITS | 3;

    as->fd_table = malloc(sizeof(struct fdt_entry) * PROCESS_MAX_FILES);
    for (int i = 0 ; i < PROCESS_MAX_FILES; i++) {
        as->fd_table[i].ofd = -1;
    }

    as->fd_table[0].ofd = 0; /* STDIN */
    as->fd_table[1].ofd = 1; /* STDOUT */
    as->fd_table[2].ofd = 1; /* STDERR */

    return as;
}

int as_define_region(struct app_addrspace *as,
        seL4_Word baseaddr,
        seL4_Word size,
        seL4_Word permissions) {
    struct region *new_region = malloc(sizeof(struct region));
    if (new_region == NULL) {
        return -1;
    }

    printf("Region defined: %x - %x\n", baseaddr, baseaddr + size);

    /* Init region variables */
    new_region->baseaddr = baseaddr;
    new_region->size = size;
    new_region->permissions = permissions;

    /* Add to addrspace region list */
    if (as->regions == NULL) {
        as->regions = new_region;
        new_region->next = NULL;
    } else {
        new_region->next = as->regions;
        as->regions = new_region;
    }

    return 0;
}

int as_free(struct app_addrspace *as) {
    /* Free frames in addrspace */
    for (int i = 0; i < 1024; i++) {
        if (as->page_table[i] == NULL) continue;

        /* Free leaf frames */
        for (int j = 0; j < 1024; j++) {
            if (as->page_table[i][j].sos_vaddr & PTE_VALID) {
                frame_free(as->page_table[i][j].sos_vaddr);
            }
        }

        /* Free root frame */
        frame_free((seL4_Word) as->page_table[i]);
    }

    /* Free regions */
    struct region *curr_region = as->regions;
    while (curr_region != NULL) {
        struct region *region_to_free = curr_region;
        curr_region = curr_region->next;
        free(region_to_free);
    }

    return 0;
}

struct region *get_region(seL4_Word uaddr) {
    struct region *curr_region = curproc->addrspace->regions;
    while (curr_region != NULL) {
        if (curr_region->baseaddr <= uaddr &&
                curr_region->baseaddr + curr_region->size > uaddr) {
            return curr_region;
        }
        curr_region = curr_region->next;
    }

    return NULL;
}

int as_destroy(struct app_addrspace ** addrspace){
    for (int i = 0; i < 1024; i++){
		if ((*addrspace)->page_table[i] != NULL){
			for (int j = 0; j < 1024; j++){
				if ((*addrspace)->page_table[i][j].sos_vaddr & PTE_VALID){
					seL4_Word sos_vaddr = PAGE_ALIGN_4K((*addrspace)->page_table[i][j].sos_vaddr);
					sos_unmap_page((i << 20) | (j << 10),*addrspace);
					seL4_ARM_Page_Unify_Instruction(get_cap(sos_vaddr), 0, PAGE_SIZE_4K);
					frame_free(sos_vaddr);
				} 
				
				if ((*addrspace)->page_table[i][j].sos_vaddr & PTE_SWAP){
		            free_swap_index((*addrspace)->swap_table[i][j].swap_index); 	
				}
			}
            
			/* In our design, we definitely have the swap_table page allocated if the page_table page exist*/
			frame_free(PAGE_ALIGN_4K((seL4_Word)(*addrspace)->swap_table[i]));
			frame_free(PAGE_ALIGN_4K((seL4_Word)(*addrspace)->page_table[i]));
		}	
	}
	frame_free(PAGE_ALIGN_4K((seL4_Word)(*addrspace)->swap_table));
	frame_free(PAGE_ALIGN_4K((seL4_Word)(*addrspace)->page_table));

	struct region * curr = (*addrspace)->regions;
	while (curr != NULL){
		struct region* to_free = curr;
		curr = curr -> next;
        free(to_free);
	}

    
	/*close unclose file*/
	
	for (int fd = 0;(*addrspace)->fd_table[fd].ofd != -1 && fd <PROCESS_MAX_FILES;fd++){
		seL4_Word ofd = (*addrspace)->fd_table[fd].ofd;


		(*addrspace)->fd_table[fd].ofd = -1;

		seL4_Word fdt_status = (*addrspace)->fdt_status;
		seL4_Word fd_count = (fdt_status >> TWO_BYTE_BITS) - 1;
		(*addrspace)->fdt_status = (fd_count << TWO_BYTE_BITS) | fd;

		of_table[ofd].ref_count--;

		if (of_table[ofd].ref_count == 0) {
			vfs_close(of_table[ofd].vnode, of_table[ofd].file_info.st_fmode);
			of_table[ofd].file_info.st_fmode = 0;
			of_table[ofd].vnode = NULL;
			ofd_count--;
			of_table[ofd].offset = 0;
			curr_free_ofd = ofd;
		}
	}
	free(*addrspace);
}

