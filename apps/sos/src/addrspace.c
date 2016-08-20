#include <stdio.h>
#include <stdlib.h>
#include <cspace/cspace.h>
#include "addrspace.h"
#include "ut_manager/ut.h"
#include "frametable.h"

struct app_addrspace *as_new() {
    struct app_addrspace *as = malloc(sizeof(struct app_addrspace));
    if (as == NULL) {
        return NULL;
    }
    as->regions = NULL;
    as->page_table = CSPACE_NULL;
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

    /* Init region */
    new_region->baseaddr = baseaddr;
    new_region->size = size;
    new_region->permissions = permissions;

    if (as->regions == NULL) {
        as->regions = new_region;
        new_region->next = NULL;
    } else {
        new_region->next = as->regions;
        as->regions = new_region;
    }

    return 0;
}

int as_free(struct app_addrspace *as){
    for (int i = 0; i < 1024; i++){
        if (as->page_table[i] == NULL) continue;
	for (int j = 0;j < 1024; j++){
            if (as->page_table[i][j].sos_vaddr &PTE_VALID){
		frame_free(as->page_table[i][j].sos_vaddr);
            }
	}
        frame_free((seL4_Word)as->page_table[i]);
    }

    struct region *curr_region = as -> regions;
    while (curr_region !=NULL){
	struct region *region_to_free = curr_region;
	curr_region = curr_region -> next;
	free(region_to_free);
    }
    return 0;
}
