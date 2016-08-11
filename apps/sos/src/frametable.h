void frame_init(seL4_Word low, seL4_Word high);

int32_t frame_alloc(seL4_Word *vaddr);

void frame_free(seL4_Word page);
