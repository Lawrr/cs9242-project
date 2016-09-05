#include <string.h>
#include <cspace/cspace.h>
#include <clock/clock.h>
#include <utils/page.h>
#include <fcntl.h>

#include "addrspace.h"
#include "sos_syscall.h"
#include "mapping.h"
#include "sos.h"
#include "file.h"
#include "vmem_layout.h"
#include "process.h"
#include "vnode.h"

extern struct PCB tty_test_process;
extern struct oft_entry of_table[MAX_OPEN_FILE];
extern seL4_Word ofd_count;
extern seL4_Word curr_free_ofd;

/* Checks that user pointer range is a valid in userspace */
static int legal_uaddr(seL4_Word base, uint32_t size) {
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

void send_reply(seL4_CPtr reply_cap){
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

static void send_err(seL4_CPtr reply_cap, int err) {
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, err); 
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

static int validate_buffer_size(seL4_CPtr reply_cap, int32_t size) {
    if (size <= 0) {
        send_err(reply_cap, ERR_INVALID_ARGUMENT);
        return 1;
    }
    return 0;
}

static int validate_uaddr(seL4_CPtr reply_cap, char *uaddr, int32_t size) {
    if (!legal_uaddr(uaddr, size)) {
        send_err(reply_cap, ERR_ILLEGAL_USERADDR);
        return 1;
    }
    return 0;
}

static int validate_fd(seL4_CPtr reply_cap, int fd) {
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        send_err(reply_cap, ERR_INVALID_FD);
        return 1;
    }
    return 0;
}

static int validate_ofd(seL4_CPtr reply_cap, int ofd) {
    if (ofd == -1) {
        send_err(reply_cap, ERR_INVALID_FD);
        return 1;
    }
    return 0;
}

static int validate_ofd_mode(seL4_CPtr reply_cap, int ofd, int mode) {
    if (!(of_table[ofd].file_info.st_fmode & mode)) {
        send_err(reply_cap, ERR_ILLEGAL_ACCESS_MODE);
        return 1;
    }
    return 0;
}

static int validate_max_fd(seL4_CPtr reply_cap, int fd_count) {
    if (fd_count == PROCESS_MAX_FILES) {
        send_err(reply_cap, ERR_MAX_FILE);
        return 1;
    }
    return 0;
}

static int validate_max_ofd(seL4_CPtr reply_cap, int ofd_count) {
    if (ofd_count == MAX_OPEN_FILE) {
        send_err(reply_cap, ERR_MAX_SYSTEM_FILE);
        return 1;
    }
    return 0;
}

static int get_safe_path(char *dst, seL4_Word uaddr,
        seL4_Word sos_vaddr, uint32_t max_len) {
    /* Get safe path */
    seL4_Word uaddr_next_page = PAGE_ALIGN_4K(uaddr) + 0x1000;
    seL4_Word safe_len = uaddr_next_page - uaddr;
    if (safe_len < max_len) {
        int len = strnlen(sos_vaddr, safe_len);
        if (len == safe_len) {
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
            len = strnlen(sos_vaddr_next, max_len - safe_len);
            if (len == max_len - safe_len) {
                /* Doesn't have terminator */
                return 1;
            } else {
                strncpy(dst, sos_vaddr, safe_len);
                strcpy(dst + safe_len, sos_vaddr_next);    
            }
        } else {
            strcpy(dst, sos_vaddr);
        }
    } else {
        int len = strnlen(sos_vaddr, max_len);
        if (len == max_len) {
            /* Doesn't have terminator */
            return 1;
        } else {
            strcpy(dst, sos_vaddr);
        }
    }

    return 0;
}

void syscall_brk(seL4_CPtr reply_cap) {
    uintptr_t newbrk = seL4_GetMR(1);

    /* Find heap region */
    struct region *curr_region = tty_test_process.addrspace->regions;
    while (curr_region != NULL &&
            curr_region->baseaddr != PROCESS_HEAP_START) {
        curr_region = curr_region->next;
    }

    /* Check that newbrk is within heap (and that we actually have a heap region) */
    if (curr_region == NULL || newbrk >= PROCESS_HEAP_END || newbrk < PROCESS_HEAP_START) {
        /* Set error */
        send_err(reply_cap,1);
    }

    /* Set new heap region */
    curr_region->size = newbrk - PROCESS_HEAP_START;

    /* Reply */
    seL4_SetMR(0, 0);
    send_reply(reply_cap);
}

static void uwakeup(uint32_t id, void *reply_cap) {
    /* Wake up and reply back to application */
    seL4_SetMR(0, 0);
    send_reply(reply_cap);
}

void syscall_usleep(seL4_CPtr reply_cap) {
    int msec = seL4_GetMR(1);

    /* Make sure sec is positive else reply */
    if (msec < 0) {
        seL4_SetMR(0, -1);
        send_reply(reply_cap);	    
    } else {
        register_timer(msec * 1000, &uwakeup, (void *) reply_cap);
    }
}

void syscall_time_stamp(seL4_CPtr reply_cap) {
    timestamp_t timestamp = time_stamp();
    seL4_SetMR(0, timestamp); 
    send_reply(reply_cap);
}

void syscall_getdirent(seL4_CPtr reply_cap) {
    int pos = seL4_GetMR(1);
    seL4_Word uaddr = seL4_GetMR(2);
    size_t nbyte = seL4_GetMR(3);

    if (validate_uaddr(reply_cap, uaddr, 0)) return;
    //TODO check pos and nbyte valid values

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

    err = get_safe_path(path_sos_vaddr, uaddr, sos_vaddr, MAX_PATH_LEN);
    if (err) {
        send_err(reply_cap, ERR_ILLEGAL_USERADDR);
        return;
    }

    //TODO call getdirent
    //set proper MR return

    /* Reply */
    seL4_SetMR(0, 0);
    send_reply(reply_cap); 
}

void syscall_stat(seL4_CPtr reply_cap) {
    seL4_Word uaddr = seL4_GetMR(1);
    seL4_Word ustat_buf = seL4_GetMR(2);

    if (validate_uaddr(reply_cap, uaddr, 0)) return;
    if (validate_uaddr(reply_cap, ustat_buf, 0)) return;

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

    err = get_safe_path(path_sos_vaddr, uaddr, sos_vaddr, MAX_PATH_LEN);
    if (err) {
        send_err(reply_cap, ERR_ILLEGAL_USERADDR);
        return;
    }

    struct vnode *ret_vnode;
    err = vfs_open((char *) path_sos_vaddr, FM_READ, &ret_vnode);
    if (err) {
        send_err(reply_cap, -1);
        return;
    }
    
    ret_vnode->ops->vop_stat(ret_vnode,(sos_stat_t*) ustat_buf);    
    ret_vnode->ops->vop_close(ret_vnode);

    //TODO make sure ustat_buf is mapped
    //     (need to know sizeof(sos_stat_t))
    //TODO call stat

    /* Reply */
    seL4_SetMR(0, 0);
    send_reply(reply_cap);
}

void syscall_write(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word uaddr = seL4_GetMR(2);
    seL4_Word ubuf_size = seL4_GetMR(3);
    int ofd = tty_test_process.addrspace->fd_table[fd].ofd;

    if (validate_buffer_size(reply_cap, ubuf_size)) return;
    if (validate_uaddr(reply_cap, uaddr, ubuf_size)) return;
    if (validate_fd(reply_cap, fd)) return;
    if (validate_ofd(reply_cap, ofd)) return;
    if (validate_ofd_mode(reply_cap, ofd, FM_WRITE)) return;

    struct oft_entry *entry = &of_table[ofd];

    struct uio uio = {
        .addr = uaddr,
        .size = ubuf_size,
        .remaining = ubuf_size,
        .offset = entry->offset
    };

    int err = of_table[ofd].vnode->ops->vop_write(of_table[ofd].vnode, &uio);
    entry->offset = uio.offset;
    if (err) {
        send_err(reply_cap, ERR_INTERNAL_ERROR);
    }



    /* Reply */
    seL4_SetMR(0, uio.size - uio.remaining);
    send_reply(reply_cap);
}

void syscall_read(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word uaddr = seL4_GetMR(2);
    seL4_Word ubuf_size = seL4_GetMR(3);
    int ofd = tty_test_process.addrspace->fd_table[fd].ofd;

    if (validate_buffer_size(reply_cap, ubuf_size)) return;
    if (validate_uaddr(reply_cap, uaddr, ubuf_size)) return;
    if (validate_fd(reply_cap, fd)) return;
    if (validate_ofd(reply_cap, ofd)) return;
    if (validate_ofd_mode(reply_cap, ofd, FM_READ)) return;

    struct oft_entry *entry = &of_table[ofd];

    struct uio uio = {
        .addr = uaddr,
        .size = ubuf_size,
        .remaining = ubuf_size,
        .offset = entry->offset
    };

    struct page_table_entry **page_table = tty_test_process.addrspace->page_table;

    /* Set page table pointer and reply cap as data */
    seL4_Word *data = malloc(2 * sizeof(seL4_Word));
    data[0] = (seL4_Word) reply_cap;
    data[1] = (seL4_Word) page_table;
    of_table[ofd].vnode->data = (void *) data;

    of_table[ofd].vnode->ops->vop_read(of_table[ofd].vnode, &uio);
    entry->offset = uio.offset;

    seL4_SetMR(0, uio.size - uio.remaining);
    send_reply(reply_cap); 
}




void syscall_open(seL4_CPtr reply_cap) {
    seL4_Word fdt_status = tty_test_process.addrspace->fdt_status;			  
    seL4_Word free_fd = fdt_status & LOWER_TWO_BYTE_MASK;
    seL4_Word fd_count = fdt_status >> TWO_BYTE_BITS;

    seL4_Word uaddr = seL4_GetMR(1);
    fmode_t access_mode = seL4_GetMR(2);

    if (validate_max_fd(reply_cap, fd_count)) return;
    if (validate_max_ofd(reply_cap, ofd_count)) return;
    if (validate_uaddr(reply_cap, uaddr, 0)) return;

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

    err = get_safe_path(path_sos_vaddr, uaddr, sos_vaddr, MAX_PATH_LEN);
    if (err) {
        send_err(reply_cap, ERR_ILLEGAL_USERADDR);
        return;
    }

    /* Access mode */
    int sos_access_mode = 0;
    if (access_mode == O_RDWR) {
        sos_access_mode = FM_READ | FM_WRITE;
    } else if (access_mode == O_RDONLY) {
        sos_access_mode = FM_READ;
    } else {
        sos_access_mode = FM_WRITE;
    }

    struct vnode *ret_vnode;
    err = vfs_open((char *) path_sos_vaddr, sos_access_mode, &ret_vnode);

    if (err) {
        seL4_SetMR(0, -1);
    } else {
        of_table[curr_free_ofd].vnode = ret_vnode;

        of_table[curr_free_ofd].file_info.st_fmode = sos_access_mode;
        of_table[curr_free_ofd].ref_count++;

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
    }
    /* Reply */
    send_reply(reply_cap);
}

void syscall_close(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word ofd = tty_test_process.addrspace->fd_table[fd].ofd;

    if (validate_fd(reply_cap, fd)) return;
    if (validate_ofd(reply_cap, ofd)) return;

    tty_test_process.addrspace->fd_table[fd].ofd = -1;
    of_table[ofd].ref_count--;

    if (of_table[ofd].ref_count == 0) {
        vfs_close(of_table[ofd].vnode, of_table[ofd].file_info.st_fmode);
        printf("syscall_close1.1\n");  
        of_table[ofd].file_info.st_fmode = 0;
        printf("syscall_close1.2\n");
        of_table[ofd].vnode = NULL;
    }
    printf("syscall_close1.3\n");
    /* Reply */
    seL4_SetMR(0, 0);
    send_reply(reply_cap);
}
