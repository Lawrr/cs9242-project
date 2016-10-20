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
#include <sys/panic.h>

#define PAGE_ENTRIES 1024

extern struct PCB *curproc;
extern struct oft_entry of_table[MAX_OPEN_FILE];

struct app_addrspace *as_new() {
    struct app_addrspace *as = malloc(sizeof(struct app_addrspace));
    if (as == NULL) {
        return NULL;
    }

    as->fd_table = malloc(sizeof(struct fdt_entry) * PROCESS_MAX_FILES);
    if (as->fd_table == NULL) {
        free(as);
        return NULL;
    }
    for (int i = 0 ; i < PROCESS_MAX_FILES; i++) {
        as->fd_table[i].ofd = -1;
    }

    /* Initialise addrspace variables */
    as->regions = NULL;
    as->page_table = NULL;
    as->page_count = 0;

    /* File descriptors */
    /* STDIN and STDERR */
    as->fd_count = 2;

    as->fd_table[STDOUT_FD].ofd = STDOUT_OFD; /* STDOUT */
    as->fd_table[STDERR_FD].ofd = STDOUT_OFD; /* STDERR */

    /*open file table increase ref count*/
    of_table[STDOUT_OFD].ref_count += 2;

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

int as_destroy(struct app_addrspace *as) {
    if (as == NULL) return -1;

    int err;

    /* Free page table and swap table */
    for (int i = 0; i < PAGE_ENTRIES; i++) {

        if (as->page_table[i] == NULL) continue;

        for (int j = 0; j < PAGE_ENTRIES; j++) {

            if (as->page_table[i][j].sos_vaddr & PTE_VALID) {
                if (as->page_table[i][j].sos_vaddr & PTE_SWAP) {
                    free_swap_index(as->swap_table[i][j].swap_index);
                } else {
                    seL4_Word sos_vaddr = PAGE_ALIGN_4K(as->page_table[i][j].sos_vaddr);
                    sos_unmap_page(sos_vaddr, as);
                    frame_free(sos_vaddr);
                    seL4_ARM_Page_Unify_Instruction(get_cap(sos_vaddr), 0, PAGE_SIZE_4K);
                }
            }
        }

        /* Note: We are guarenteed to already have the swap_table page allocated if the page_table page exist */
        frame_free(PAGE_ALIGN_4K((seL4_Word) as->swap_table[i]));
        frame_free(PAGE_ALIGN_4K((seL4_Word) as->page_table[i]));
    }
    frame_free(PAGE_ALIGN_4K((seL4_Word) as->swap_table));
    frame_free(PAGE_ALIGN_4K((seL4_Word) as->page_table));

    /* Free regions */
    struct region *curr = as->regions;
    while (curr != NULL) {
        struct region *to_free = curr;
        curr = curr->next;
        free(to_free);
    }

    /* Close files */
    for (int fd = 0; fd < PROCESS_MAX_FILES; fd++) {
        seL4_Word ofd = as->fd_table[fd].ofd;
        if (ofd == -1) continue;
        of_close(ofd);
    }
    free(as->fd_table);

    /* Free actual addrspace */
    free(as);

    return 0;
}
