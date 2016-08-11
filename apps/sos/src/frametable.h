void frame_init();

int32_t frame_alloc(seL4_Word *vaddr);

void frame_free(seL4_Word page);
