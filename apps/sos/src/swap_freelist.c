#include <sys/panic.h>
#include <utils/page.h>

#include "swap_freelist.h"
#include "vnode.h"

#define FREELIST_SIZE 2048

extern struct PCB *curproc;
extern struct vnode *swap_vnode;

static struct freelist_page {
    uint32_t index;
    struct freelist_page *next;
};

/* List of indices of free pages */
static int freelist_indices[FREELIST_SIZE];
static int freelist_length = 0;

static struct freelist_page *freelist_page = NULL;

/* Last page of pagefile */
static int end_index = 0;

int get_swap_index() {
    if (freelist_length == 0) {
        /* Nothing left in current freelist */

        /* Check if freelist pages in pagefile */
        if (freelist_page != NULL) {
            struct uio uio = {
                .vaddr = freelist_indices,
                .uaddr = NULL,
                .size = PAGE_SIZE_4K,
                .offset = freelist_page->index * PAGE_SIZE_4K,
                .remaining = PAGE_SIZE_4K,
                .pcb = curproc
            };

            /* Read in 1 page (1024) of freelist indices */
            int err = swap_vnode->ops->vop_read(swap_vnode, &uio);
            conditional_panic(err, "Could not read\n");

            /* Update freelist data */
            struct freelist_page *next = freelist_page->next;
            free(freelist_page);
            freelist_page = next;

            freelist_length = PAGE_SIZE_4K / sizeof(int); /* 1024 */

            /* Return free index */
            return freelist_indices[--freelist_length];

        } else {
            /* No more freelists */
            return end_index++;
        }

    } else {
        /* Just use freelist */
        return freelist_indices[--freelist_length];
    }
}

/* Should be called by swap in, this can handle 4GB swap file */
void free_swap_index(uint32_t index) {
    if (freelist_length == FREELIST_SIZE) {
        /* Current freelist is already full */

        /* Note: Use the index we just got as the new freelist page index */
        /* Addr = Second half of freelist_indices */
        struct uio uio = {
            .vaddr = freelist_indices + ((PAGE_SIZE_4K / sizeof(int)) / 2),
            .uaddr = NULL,
            .size = PAGE_SIZE_4K,
            .offset = index * PAGE_SIZE_4K,
            .remaining = PAGE_SIZE_4K
        };

        /* Write out 1 page (1024) of freelist indices */
        int err = swap_vnode->ops->vop_write(swap_vnode, &uio);
        conditional_panic(err, "Could not write\n");

        /* Update freelist data */
        struct freelist_page *new_flp = malloc(sizeof(struct freelist_page));
        // TODO
        new_flp->index = index;
        new_flp->next = freelist_page;
        freelist_page = new_flp;

        freelist_length = PAGE_SIZE_4K / sizeof(int); /* 1024 */

    } else {
        /* Add to freelist */
        freelist_indices[freelist_length++] = index;
    }
}
