#ifndef _FRAMETABLE_H_
#define _FRAMETABLE_H_

void frame_init();

int32_t frame_alloc(seL4_Word *vaddr);

void frame_free(seL4_Word vaddr);

seL4_CPtr get_cap(seL4_Word vaddr);
#endif /* _FRAMETABLE_H_ */
