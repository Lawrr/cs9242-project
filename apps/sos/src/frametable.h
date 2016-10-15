#ifndef _FRAMETABLE_H_
#define _FRAMETABLE_H_

#include "addrspace.h"

struct app_cap {
    struct app_addrspace *addrspace;
    seL4_Word uaddr;
    seL4_CPtr cap;
    struct app_cap *next;
};

void pin_frame_entry(seL4_Word uaddr, seL4_Word size);
void unpin_frame_entry(seL4_Word uaddr, seL4_Word size);
void frame_init();

int32_t frame_alloc(seL4_Word *vaddr);
int32_t unswappable_alloc(seL4_Word *vaddr);

int32_t frame_free(seL4_Word vaddr);

seL4_CPtr get_cap(seL4_Word vaddr);

int32_t insert_app_cap(seL4_Word vaddr, seL4_CPtr cap, struct app_addrspace *addrspace,seL4_Word uaddr);

int32_t get_app_cap(seL4_Word vaddr, struct app_addrspace *as, struct app_cap **cap_ret);

int32_t swap_in(seL4_Word uaddr, seL4_Word sos_vaddr);
int32_t swap_out();

#endif /* _FRAMETABLE_H_ */
