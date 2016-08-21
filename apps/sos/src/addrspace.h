#ifndef _ADDRSPACE_H_
#define _ADDRSPACE_H_

#define PTE_VALID (1 << 3)
#define PTE_SWAP  (1 << 4)

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


/*
 *VFN|UNUSED|S|V|P|
 *S:Swap bit
 *V:Valid bit
 *P:Permission 3bits same as elf_permission
 *  seL4_CanWrite = 0x01,
 *  seL4_CanRead = 0x02,
 *  seL4_CanGrant = 0x04, 
 */
struct page_table_entry {
    seL4_CPtr sos_vaddr;
};

struct app_addrspace *as_new();

int as_define_region(struct app_addrspace *as,
                     seL4_Word vroot,
                     seL4_Word size,
                     seL4_Word permissions);

#endif /* _ADDRSPACE_H_ */
