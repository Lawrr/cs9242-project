#ifndef _ADDRSPACE_H_
#define _ADDRSPACE_H_

struct app_addrspace;
struct region;

struct app_addrspace *as_new();

int as_define_region(struct app_addrspace *as,
                     seL4_Word vroot,
                     seL4_Word size,
                     seL4_Word permissions);

#endif /* _ADDRSPACE_H_ */
