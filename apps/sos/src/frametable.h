#ifndef _FRAMETABLE_H_
#define _FRAMETABLE_H_

#include "addrspace.h"

void frame_init();

int32_t frame_alloc(seL4_Word *vaddr);

int32_t frame_free(seL4_Word vaddr);

seL4_CPtr get_cap(seL4_Word vaddr);

int32_t insert_app_cap(seL4_Word vaddr, seL4_CPtr cap, struct app_addrspace *addrspace,seL4_Word uaddr);

int32_t swap_in(seL4_Word uaddr);
int32_t swap_out();

struct app_cap {
    struct app_addrspace *addrspace;
    seL4_Word uaddr;
    seL4_CPtr cap;
    struct app_cap *next;
};

#endif /* _FRAMETABLE_H_ */
