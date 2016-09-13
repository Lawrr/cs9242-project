#include <stdio.h>
#include <stdlib.h>
#include <cspace/cspace.h>
#include "addrspace.h"
#include "ut_manager/ut.h"
#include "frametable.h"
#include "process.h"
#include "sos.h"

extern struct PCB *curproc;

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
