/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

#ifndef _LIBOS_ELF_H_
#define _LIBOS_ELF_H_

#include <sel4/sel4.h>
#include "addrspace.h"
#include "process.h"
#include "vnode.h"

int cpio_elf_load(seL4_ARM_PageDirectory dest_pd, struct PCB *pcb, char *elf_file);

int elf_load(seL4_ARM_PageDirectory dest_pd, struct PCB *pcb, char *elf_file, struct vnode *vnode);

#endif /* _LIBOS_ELF_H_ */