#ifndef _SWAP_FREELIST_H_
#define _SWAP_FREELIST_H_

#include <cspace/cspace.h>

void freelist_init(void *swap_list_page);

int get_swap_index();

int free_swap_index(uint32_t index);

#endif
