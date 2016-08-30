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
#include "vnode.h"
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

int validate_buffer_size(seL4_CPtr reply_cap, int32_t size) {
    if (size <= 0) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_ARGUMENT); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return 1;
    }
    return 0;
}

int validate_uaddr(seL4_CPtr reply_cap, char *uaddr) {
    if (!legal_uaddr(uaddr)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_USERADDR); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return 1;
    }
    return 0;
}

int validate_uaddr_range(seL4_CPtr reply_cap, char *uaddr, int32_t size) {
    if (!legal_uaddr_range(uaddr, size)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_USERADDR); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return 1;
    }
    return 0;
}

int validate_fd(seL4_CPtr reply_cap, int fd) {
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_FD);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return 1;
    }
    return 0;
}

int validate_ofd(seL4_CPtr reply_cap, int ofd) {
    if (ofd == -1) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_FD); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
}

int validate_ofd_mode(seL4_CPtr reply_cap, int ofd, int mode) {
    if (!(of_table[ofd].file_info.st_fmode & mode)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_ACCESS_MODE); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return 1;
    }
    return 0;
}

int validate_max_fd(seL4_CPtr reply_cap, int fd_count) {
    if (fd_count == PROCESS_MAX_FILES) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_MAX_FILE);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return 1;
    }
    return 0;
}

int validate_max_ofd(seL4_CPtr reply_cap, int ofd_count) {
    if (ofd_count == MAX_OPEN_FILE) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_MAX_SYSTEM_FILE);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return 1;
    }
    return 0;
}

void syscall_write(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word uaddr = seL4_GetMR(2);
    seL4_Word ubuf_size = seL4_GetMR(3);

    if (validate_buffer_size(reply_cap, ubuf_size)) return;
    if (validate_uaddr_range(reply_cap, uaddr, ubuf_size)) return;
    if (validate_fd(reply_cap, fd)) return;

    seL4_Word ofd = tty_test_process.addrspace->fd_table[fd].ofd;
    if (validate_ofd(reply_cap, ofd)) return;
    if (validate_ofd_mode(reply_cap, ofd, FM_WRITE)) return;

    int bytes_sent = 0;

    struct uio uio;
    uio.fileOffset = 0;

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

        uio.bufAddr = sos_vaddr;
        uio.bufSize = size;
        uio.remaining = size;

        of_table[ofd].vnode->vn_ops->vop_write(of_table[ofd].vnode, &uio);

        bytes_sent += size - uio.remaining;

        ubuf_size -= size;
        uaddr = uaddr_next;
        uio.fileOffset += size;
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
    
    if (validate_buffer_size(reply_cap, ubuf_size)) return;
    if (validate_uaddr_range(reply_cap, uaddr, ubuf_size)) return;
    if (validate_fd(reply_cap, fd)) return;

    seL4_Word ofd = tty_test_process.addrspace->fd_table[fd].ofd;
    if (validate_ofd(reply_cap, ofd)) return;
    if (validate_ofd_mode(reply_cap, ofd, FM_READ)) return;

    /* Make sure address is mapped */
    seL4_Word end_uaddr = uaddr + ubuf_size;
    seL4_Word curr_uaddr = uaddr;
    seL4_Word curr_size = ubuf_size;
    while (curr_size > 0) {
        seL4_Word uaddr_next = PAGE_ALIGN_4K(curr_uaddr) + 0x1000;
        seL4_Word size;
        if (end_uaddr >= uaddr_next) {
            size = uaddr_next-curr_uaddr;
        } else {
            size = curr_size;
        }

        seL4_CPtr app_cap;
        seL4_CPtr sos_vaddr;
        int err = sos_map_page(curr_uaddr,
                               tty_test_process.vroot,
                               tty_test_process.addrspace,
                               &sos_vaddr,
                               &app_cap);

        curr_size -= size;
        curr_uaddr = uaddr_next;
    }

    struct uio uio = {
        .bufAddr = uaddr,
        .bufSize = ubuf_size,
        .remaining = ubuf_size,
        .fileOffset = 0
    };

    struct page_table_entry **page_table = tty_test_process.addrspace->page_table;
    /* Page table pointer and reply cap */
    seL4_Word *data = malloc(2 * sizeof(seL4_Word));
    data[0] = (seL4_Word) reply_cap;
    data[1] = (seL4_Word) page_table;
    of_table[ofd].vnode->vn_data = (void *) data; 
    of_table[ofd].vnode->vn_ops->vop_read(of_table[ofd].vnode, &uio);
}

void syscall_open(seL4_CPtr reply_cap) {
    seL4_Word fdt_status = tty_test_process.addrspace->fdt_status;			  
    seL4_Word free_fd = fdt_status & LOWER_TWO_BYTE_MASK;
    seL4_Word fd_count = fdt_status >> TWO_BYTE_BITS;

    seL4_Word uaddr = seL4_GetMR(1);
    fmode_t access_mode = seL4_GetMR(2); 
    
    if (validate_max_fd(reply_cap, fd_count)) return;
    if (validate_max_ofd(reply_cap, ofd_count)) return;
    if (validate_uaddr(reply_cap, uaddr)) return;

    char path_sos_vaddr[MAX_PATH_LEN];
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

    seL4_Word uaddr_next_page = PAGE_ALIGN_4K(uaddr)+0x1000;
    seL4_Word safe_len = uaddr_next_page - uaddr;
    if (safe_len < MAX_PATH_LEN){
        int len = strnlen(sos_vaddr,safe_len);
        if (len == safe_len){
            /* Make sure address is mapped */
            seL4_CPtr app_cap_next;
            seL4_Word sos_vaddr_next;
            int err = sos_map_page(uaddr_next_page,
                    tty_test_process.vroot,
                    tty_test_process.addrspace,
                    &sos_vaddr_next,
                    &app_cap_next);

            sos_vaddr_next = PAGE_ALIGN_4K(sos_vaddr_next);
            sos_vaddr_next |= (uaddr_next_page & PAGE_MASK_4K);
            len = strnlen(sos_vaddr_next,MAX_PATH_LEN - safe_len);
            if (len == MAX_PATH_LEN - safe_len){
                //Doesn't have terminator
                seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
                seL4_SetMR(0, ERR_ILLEGAL_USERADDR); 
                seL4_Send(reply_cap, reply);
                cspace_free_slot(cur_cspace, reply_cap);
                return;
            }  else{
                strncpy(path_sos_vaddr,sos_vaddr,safe_len);
                strcpy(path_sos_vaddr+safe_len,sos_vaddr_next);    
            }
        }  else{
            strcpy(path_sos_vaddr,sos_vaddr);
        }
    }  else{
        int len = strnlen(sos_vaddr,MAX_PATH_LEN);
        if (len == MAX_PATH_LEN){
            //Doesn't have terminator
            seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
            seL4_SetMR(0, ERR_ILLEGAL_USERADDR); 
            seL4_Send(reply_cap, reply);
            cspace_free_slot(cur_cspace, reply_cap);
            return;
        }  else{
            strcpy(path_sos_vaddr,sos_vaddr);
        }
    }    
    struct vnode *ret_vn;
    err = vfs_open((char*)path_sos_vaddr, &ret_vn);

    of_table[curr_free_ofd].vnode = ret_vn;

    /* Set access mode and add ref count */
    int sos_access_mode;
    if (access_mode == O_RDWR) {
        sos_access_mode = FM_READ | FM_WRITE;
    } else if (sos_access_mode == O_RDONLY) {
        sos_access_mode = FM_READ;
    } else {
        sos_access_mode = FM_WRITE;
    }

    of_table[curr_free_ofd].file_info.st_fmode = sos_access_mode;
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

    if (validate_fd(reply_cap, fd)) return;
    if (validate_ofd(reply_cap, ofd)) return;

    tty_test_process.addrspace->fd_table[fd].ofd = -1;
    of_table[ofd].ref--;

    if (of_table[ofd].ref == 0) {
        of_table[ofd].vnode->vn_ops->vop_close(of_table[ofd].vnode);
        of_table[ofd].vnode = NULL;
        of_table[ofd].file_info.st_fmode = 0;
    }

    /* Reply */
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, 0);
    seL4_Send(reply_cap, reply);       	
    cspace_free_slot(cur_cspace, reply_cap);
}
