#ifndef _ADDRSPACE_H_
#define _ADDRSPACE_H_

#define PTE_VALID (1 << 3)
#define PTE_SWAP (1 << 4)

struct app_addrspace {
    seL4_Word fd_count;
    seL4_Word page_count;
    struct region *regions;
    struct fdt_entry *fd_table;
    struct page_table_entry **page_table;
    struct swap_table_entry **swap_table;
};

struct region {
    seL4_Word baseaddr;
    seL4_Word size;
    seL4_Word permissions;
    struct region *next;
};

struct fdt_entry {
    seL4_Word ofd;
};

struct swap_table_entry {
    seL4_Word swap_index;
};

/*
 *VFN|UNUSED|S|V|P|
 *S:Swap bit
 *V:Valid bit
 *P:Permission 3bits same as elf_permission
 * seL4_CanWrite = 0x01,
 * seL4_CanRead = 0x02,
 * seL4_CanGrant = 0x04,
 */
struct page_table_entry {
    seL4_CPtr sos_vaddr;
};

struct app_addrspace *as_new();

int as_define_region(struct app_addrspace *as,
        seL4_Word vroot,
        seL4_Word size,
        seL4_Word permissions);

struct region *get_region(seL4_Word uaddr);

int as_destroy(struct app_addrspace *as);

#endif /* _ADDRSPACE_H_ */
