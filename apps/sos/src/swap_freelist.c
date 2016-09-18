#include <sys/panic.h>
#include <utils/page.h>

#include "swap_freelist.h"

int *list1;
/* int *list2; */
int list1_free_index;
/* list2_free_index */

/* Bump pointer */
int free_swap_index;

void free_list_init(void *swap_list_page) {
    list1 = (int *) swap_list_page;
    for (int i = 0; i < 1024; i++) {
        list1[i] = -1;
        //list2[i] = -1;
    }
    list1_free_index = -1;
    //list2_free_index = -1;
}

int get_swap_offset() {
    if (list1_free_index == -1) {
        conditional_panic(free_swap_index >= 511, "We can't allow you to swap more pages");
        return free_swap_index++;
    } else if (list1_free_index > 0) {
        int ret = list1_free_index;
        list1_free_index = list1[ret];
        return ret;
    }
}

/* Should be called by swap in, this can handle 4GB swap file */
void free_swap_offset(int offset) {
    conditional_panic(offset > 511, "We can't handle more than 4GB for now");
    if (list1_free_index == -1) {
        list1[0] = offset;
        list1_free_index = 0;
    } else if (list1_free_index >= 0) {
        list1[offset] = list1_free_index;
        list1_free_index = offset;
    }

}
