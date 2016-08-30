#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <cspace/cspace.h>

#include <cpio/cpio.h>
#include <nfs/nfs.h>
#include <elf/elf.h>
#include <serial/serial.h>
#include <clock/clock.h>
#include <utils/page.h>
#include <fcntl.h>

#include "addrspace.h"
#include "frametable.h"
#include "network.h"
#include "elf.h"
#include "sos_syscall.h"
#include "ut_manager/ut.h"
#include "vmem_layout.h"
#include "mapping.h"
#include "sos.h"
#include "file.h"
#include "process.h"
#include <autoconf.h>

#define verbose 5
#include <sys/debug.h>
#include <sys/panic.h>

#define READ_DELAY 10000 /* Microseconds */

extern struct PCB tty_test_process;
extern struct oft_entry of_table[MAX_OPEN_FILE];
extern seL4_Word ofd_count;
extern seL4_Word curr_free_ofd;

/* Checks that user pointer is a valid address in userspace */
static int legal_uaddr(seL4_Word uaddr) {
    /* Check valid region */
    struct region *curr = tty_test_process.addrspace->regions;
    while (curr != NULL) {
        if (uaddr >= curr->baseaddr && uaddr < curr->baseaddr + curr->size) {
            break;
        }
        curr = curr->next;
    }

    /* User pointers should be below IPC buffer */
    if (curr != NULL && uaddr < PROCESS_IPC_BUFFER) {
        return 1;
    }
    return 0;
}

/* Checks that user pointer range is a valid in userspace */
static int legal_uaddr_range(seL4_Word base, uint32_t size) {
    /* Check valid region */
    struct region *curr = tty_test_process.addrspace->regions;
    while (curr != NULL) {
        if (base >= curr->baseaddr &&
            base + size < curr->baseaddr + curr->size) {
            break;
        }
        curr = curr->next;
    }

    /* User pointers should be below IPC buffer */
    if (curr != NULL && base + size < PROCESS_IPC_BUFFER) {
        return 1;
    }
    return 0;
}

void syscall_brk(seL4_CPtr reply_cap) {
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    uintptr_t newbrk = seL4_GetMR(1);

    /* Find heap region */
    struct region *curr_region = tty_test_process.addrspace->regions;
    while (curr_region != NULL &&
           curr_region->baseaddr != PROCESS_HEAP_START) {
        curr_region = curr_region->next;
    }

    /* Check that newbrk is before HEAP_END (and that we even have a heap region...) */
    if (curr_region == NULL || newbrk >= PROCESS_HEAP_END || newbrk < PROCESS_HEAP_START) {
        /* Set error */
        seL4_SetMR(0, 1);
        seL4_Send((seL4_CPtr) reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
    }

    /* Set new heap region */
    curr_region->size = newbrk - PROCESS_HEAP_START;

    /* Reply */
    seL4_SetMR(0, 0);
    seL4_Send(reply_cap, reply);
}

static void uwakeup(uint32_t id, void *reply_cap) {
    /* Wake up and reply back to application */
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 0);
    seL4_Send((seL4_CPtr) reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

void syscall_usleep(seL4_CPtr reply_cap) {
    int msec = seL4_GetMR(1);

    /* Make sure sec is positive */
    if (msec < 0) {
        msec = 0;
    }

    register_timer(msec * 1000, &uwakeup, (void *) reply_cap);
}

void syscall_time_stamp(seL4_CPtr reply_cap) {
    timestamp_t timestamp = time_stamp();

    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, timestamp); 
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

void syscall_write(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word uaddr = seL4_GetMR(2);
    seL4_Word ubuf_size = seL4_GetMR(3);

    /* Check ubuf_size */
    if (ubuf_size <= 0) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_ARGUMENT); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    /* Check user address */
    if (!legal_uaddr_range(uaddr, ubuf_size)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_USERADDR); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    /* Check fd */
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_FD);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    seL4_Word ofd = tty_test_process.addrspace->fd_table[fd].ofd;
    /* Check ofd */
    if (ofd == -1) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_FD);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /* Check access right */
    if (!(of_table[ofd].file_info.st_fmode & FM_WRITE)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_ACCESS_MODE); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    int bytes_sent = 0;

    // TODO page by page
    struct uio uio = {
        .bufAddr = sos_vaddr,
        .bufSize = size,
        .fileOffset = offset
    };

    // TODO finish
    seL4_Word end_uaddr = uaddr + ubuf_size;
    while (ubuf_size > 0) {
        seL4_Word uaddr_next = PAGE_ALIGN_4K(uaddr) + 0x1000;
        seL4_Word size;
        if (end_uaddr >= uaddr_next) {
            size = uaddr_next-uaddr;
        } else {
            size = ubuf_size;
        }

        /* Though we can assume the buffer is mapped because it is a write operation,
         * we still use sos_map_page to find the mapping address if it is already mapped */
        seL4_CPtr app_cap;
        seL4_CPtr sos_vaddr;
        int err = sos_map_page(uaddr,
                tty_test_process.vroot,
                tty_test_process.addrspace,
                &sos_vaddr,
                &app_cap);
        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        /* Add offset */
        sos_vaddr |= (uaddr & PAGE_MASK_4K);

        of_table[ofd].vnode->vn_ops->vop_write(of_table[ofd].vnode, &uio);

        ubuf_size -= size;
        uaddr = uaddr_next;
    }

    /* Reply */
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, bytes_sent);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

void syscall_read(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word uaddr = seL4_GetMR(2);
    seL4_Word ubuf_size = seL4_GetMR(3);
    
    /* Check ubuf_size */
    if (ubuf_size <= 0) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_ARGUMENT); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    /* Check user address */
    if (!legal_uaddr_range(uaddr, ubuf_size)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_USERADDR); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    /* Check fd */
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0,ERR_INVALID_FD); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /* Make sure address is mapped */
    seL4_CPtr app_cap;
    seL4_CPtr sos_vaddr;
    int err = sos_map_page(uaddr,
            tty_test_process.vroot,
            tty_test_process.addrspace,
            &sos_vaddr,
            &app_cap);

    sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
     /* Add offset */
    sos_vaddr |= (uaddr & PAGE_MASK_4K);

    seL4_Word ofd = tty_test_process.addrspace->fd_table[fd].ofd;
    /* Check ofd */
    if (ofd == -1) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_FD); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /* Check access right */
    if (!(of_table[ofd].file_info.st_fmode & FM_READ)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_ACCESS_MODE);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    // TODO page by page
    struct uio uio = {
        .bufAddr = sos_vaddr,
        .bufSize = size,
        .fileOffset = offset
    };

    of_table[ofd].vnode->vn_ops->vop_read(of_table[ofd].vnode, &uio);
}

void syscall_open(seL4_CPtr reply_cap) {
    seL4_Word fdt_status = tty_test_process.addrspace->fdt_status;			  
    seL4_Word free_fd = fdt_status & LOWER_TWO_BYTE_MASK;
    seL4_Word fd_count = fdt_status >> TWO_BYTE_BITS;

    seL4_Word uaddr = seL4_GetMR(1);
    fmode_t access_mode = seL4_GetMR(2); 

    if (fd_count == PROCESS_MAX_FILES) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_MAX_FILE);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    } else if (ofd_count == MAX_OPEN_FILE) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_MAX_SYSTEM_FILE);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    /* Check user address */
    if (!legal_uaddr(uaddr)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_USERADDR); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /* Make sure address is mapped */
    seL4_CPtr app_cap;
    seL4_Word sos_vaddr;
    int err = sos_map_page(uaddr,
                           tty_test_process.vroot,
                           tty_test_process.addrspace,
                           &sos_vaddr,
                           &app_cap);

    sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
    sos_vaddr |= (uaddr & PAGE_MASK_4K);

    // TODO path length stuff?!?!
    struct vnode *ret_vn;
    int err = vfs_open(path, &ret_vn);

    of_table[curr_free_ofd].vnode = ret_vn;

    /* Set access mode and add ref count */
    of_table[curr_free_ofd].file_info.st_fmode = access_mode;
    of_table[curr_free_ofd].ref++;
    
    tty_test_process.addrspace->fd_table[free_fd].ofd = curr_free_ofd;

    ofd_count++;
    while (of_table[curr_free_ofd].vnode != NULL) {
        curr_free_ofd = (curr_free_ofd + 1) % MAX_OPEN_FILE;  
    }

    /* Compute free_fd */
    fd_count++;

    seL4_SetMR(0, free_fd);

    while (tty_test_process.addrspace->fd_table[free_fd].ofd != -1) {
        free_fd = (free_fd + 1) % PROCESS_MAX_FILES;
    }

    tty_test_process.addrspace->fdt_status = (fd_count << TWO_BYTE_BITS) |
                                             free_fd;

    /* Reply */
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

void syscall_close(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word ofd = tty_test_process.addrspace->fd_table[fd].ofd;

    /* Check fd */
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_FD); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    /* Check ofd */	
    if (ofd == -1 || of_table[ofd].ref == 0) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_FD); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /* TODO Close actual file
     *
     *
     *
     *
     * */

    tty_test_process.addrspace->fd_table[fd].ofd = -1;
    of_table[ofd].ref--;

    if (ofd == STD_IN || ofd == STD_OUT || ofd == STD_INOUT) {
        /* Console related */
    } else {
        if (of_table[curr_free_ofd].ref == 0) {
            of_table[curr_free_ofd].ptr = NULL;
            of_table[curr_free_ofd].file_info.st_fmode = 0;
        }
    }

    /* Reply */
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, 0);
    seL4_Send(reply_cap, reply);       	
    cspace_free_slot(cur_cspace, reply_cap);
}
