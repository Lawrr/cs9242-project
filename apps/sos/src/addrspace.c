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
    as->page_count = 0;

    /* File descriptors */
    /* STDIN and STDERR */
    as->fd_count = 2;

    as->fd_table = malloc(sizeof(struct fdt_entry) * PROCESS_MAX_FILES);
    for (int i = 0 ; i < PROCESS_MAX_FILES; i++) {
        as->fd_table[i].ofd = -1;
    }

    /* Note: Below line is not needed. Client must explicitly open STDIN */
    // as->fd_table[0].ofd = 0; /* STDIN */

    // TODO is it possible STDOUT is not 0?? (maybe no more references)
    as->fd_table[1].ofd = STDOUT; /* STDOUT */
    as->fd_table[2].ofd = STDOUT; /* STDERR */

    /*open file table increase ref count*/
    of_table[STDOUT].ref_count += 2;

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
    for (int i = 0; i < PAGE_ENTRIES; i++) {
        if (as->page_table[i] == NULL) continue;

        /* Free leaf frames */
        for (int j = 0; j < PAGE_ENTRIES; j++) {
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

int as_destroy(struct PCB *pcb) {
    if (pcb == NULL) return  -1;
    struct app_addrspace * as = pcb->addrspace;
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
                    err = sos_unmap_page(sos_vaddr, pcb);
                    conditional_panic(err, "Could not unmap\n");

                    frame_free(sos_vaddr);

                    seL4_ARM_Page_Unify_Instruction(get_cap(sos_vaddr), 0, PAGE_SIZE_4K);
                }
            }
        }

        /* In our design, we definitely have the swap_table page allocated if the page_table page exist */
        frame_free(PAGE_ALIGN_4K((seL4_Word) as->swap_table[i]));
        frame_free(PAGE_ALIGN_4K((seL4_Word) as->page_table[i]));
    }
    frame_free(PAGE_ALIGN_4K((seL4_Word) as->swap_table));
    frame_free(PAGE_ALIGN_4K((seL4_Word) as->page_table));

    /* Free regions */
    struct region * curr = as->regions;
    while (curr != NULL) {
        struct region *to_free = curr;
        curr = curr->next;
        free(to_free);
    }

    /* Close files */
    for (int fd = 0; fd < PROCESS_MAX_FILES; fd++) {
        seL4_Word ofd = as->fd_table[fd].ofd;

        if (ofd == -1) continue;

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
    free(as->fd_table);

    /* Free actual addrspace */
    free(as);

    return 0;
}

