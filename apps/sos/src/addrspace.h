#ifndef _ADDRSPACE_H_
#define _ADDRSPACE_H_

struct app_addrspace {
    struct region *regions;
    struct page_table_entry **page_table;
   // seL4_CPtr root_cap;
   // seL4_CPtr *leaf_caps;
};

struct region {
    seL4_Word baseaddr;
    seL4_Word size;
    seL4_Word permissions;
    struct region *next;
};

struct page_table_entry {
    seL4_CPtr cap;
};

struct app_addrspace *as_new();

int as_define_region(struct app_addrspace *as,
                     seL4_Word vroot,
                     seL4_Word size,
                     seL4_Word permissions);

#endif /* _ADDRSPACE_H_ */
