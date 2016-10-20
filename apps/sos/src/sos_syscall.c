#include <string.h>
#include <cspace/cspace.h>
#include <clock/clock.h>
#include <utils/page.h>
#include <fcntl.h>
#include <sos.h>

#include "addrspace.h"
#include "sos_syscall.h"
#include "mapping.h"
#include "file.h"
#include "vmem_layout.h"
#include "process.h"
#include "vnode.h"

extern struct PCB *curproc;
extern struct oft_entry of_table[MAX_OPEN_FILE];
extern seL4_Word ofd_count;
extern seL4_Word curr_free_ofd;
extern seL4_CPtr _sos_ipc_ep_cap;
extern seL4_Word curr_coroutine_id;

/* Checks that user pointer range is a valid in userspace */
static int legal_uaddr(seL4_Word base, uint32_t size) {
    /* Check valid region */
    struct region *curr = curproc->addrspace->regions;
    while (curr != NULL) {
        if (base >= curr->baseaddr &&
                base + size < curr->baseaddr + curr->size) {
            break;
        }
        curr = curr->next;
    }

    /* User pointers should be below IPC buffer */
    if (curr != NULL && base + size < PROCESS_IPC_BUFFER) {
        return -1;
    }

    return 0;
}

void send_reply(seL4_CPtr reply_cap) {
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

static int validate_buffer_size(seL4_CPtr reply_cap, uint32_t size) {
    if (size == 0) {
        send_err(reply_cap, 0);
        return -1;
    }
    return 0;
}

static int validate_uaddr(seL4_CPtr reply_cap, char *uaddr, int32_t size) {
    if (!legal_uaddr(uaddr, size)) {
        send_err(reply_cap, -1);
        return -1;
    }
    return 0;
}

static int validate_fd(seL4_CPtr reply_cap, int fd) {
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        send_err(reply_cap, -1);
        return -1;
    }
    return 0;
}

static int validate_ofd(seL4_CPtr reply_cap, int ofd) {
    if (ofd == -1) {
        send_err(reply_cap, -1);
        return -1;
    }
    return 0;
}

static int validate_ofd_mode(seL4_CPtr reply_cap, int ofd, int mode) {
    if (!(of_table[ofd].file_info.st_fmode & mode)) {
        send_err(reply_cap, -1);
        return -1;
    }
    return 0;
}

static int validate_max_fd(seL4_CPtr reply_cap, int fd_count) {
    if (fd_count == PROCESS_MAX_FILES) {
        send_err(reply_cap, -1);
        return -1;
    }
    return 0;
}

static int validate_max_ofd(seL4_CPtr reply_cap, int ofd_count) {
    if (ofd_count == MAX_OPEN_FILE) {
        send_err(reply_cap, -1);
        return -1;
    }
    return 0;
}

static int validate_pid(seL4_CPtr reply_cap, pid_t pid) {
    if (process_status(pid) == NULL) {
        send_err(reply_cap, -1);
        return -1;
    }
    return 0;
}

static int validate_self_destruct(seL4_CPtr reply_cap, pid_t pid) {
    struct PCB *pcb = process_status(pid);
    if (pcb == NULL || pcb->status == PROCESS_STATUS_SELF_DESTRUCT) {
        send_err(reply_cap, -1);
        return -1;
    }
    return 0;
}

static int get_safe_path(char *dst, seL4_Word uaddr,
        seL4_Word sos_vaddr, uint32_t max_len) {
    /* Get safe path */
    seL4_Word uaddr_next_page = PAGE_ALIGN_4K(uaddr) + PAGE_SIZE_4K;
    seL4_Word safe_len = uaddr_next_page - uaddr;
    if (safe_len < max_len) {
        int len = strnlen(sos_vaddr, safe_len);
        if (len == safe_len) {
            /* Make sure address is mapped */
            seL4_Word sos_vaddr_next;
            int err = sos_map_page(uaddr_next_page, &sos_vaddr_next, curproc);
            if (err && err != ERR_ALREADY_MAPPED) return -1;

            sos_vaddr_next = PAGE_ALIGN_4K(sos_vaddr_next);
            sos_vaddr_next |= (uaddr_next_page & PAGE_MASK_4K);
            len = strnlen(sos_vaddr_next, max_len - safe_len);

            if (len == max_len - safe_len) {
                /* Doesn't have terminator */
                return -1;
            } else {
                memcpy(dst, sos_vaddr, safe_len);
                strcpy(dst + safe_len, sos_vaddr_next);
            }
        } else {
            strcpy(dst, sos_vaddr);
        }
    } else {
        int len = strnlen(sos_vaddr, max_len);
        if (len == max_len) {
            /* Doesn't have terminator */
            return -1;
        } else {
            strcpy(dst, sos_vaddr);
        }
    }

    return 0;
}

void syscall_brk(seL4_CPtr reply_cap) {
    uintptr_t newbrk = seL4_GetMR(1);

    /* Find heap region */
    struct region *curr_region = curproc->addrspace->regions;
    while (curr_region != NULL &&
            curr_region->baseaddr != PROCESS_HEAP_START) {
        curr_region = curr_region->next;
    }

    /* Check that newbrk is within heap (and that we actually have a heap region) */
    if (curr_region == NULL || newbrk >= PROCESS_HEAP_END || newbrk < PROCESS_HEAP_START) {
        /* Set error */
        send_err(reply_cap, -1);
        return;
    }

    /* Set new heap region */
    curr_region->size = newbrk - PROCESS_HEAP_START;

    /* Reply */
    seL4_SetMR(0, 0);
    send_reply(reply_cap);
}

static void uwakeup(uint32_t id, void *data) {
    /* Wake up and reply back to application */
    seL4_CPtr reply_cap = ((seL4_Word *) data)[0];
    pid_t pid = ((seL4_Word *) data)[1];
    seL4_Word stime = ((seL4_Word *) data)[2];

    /* Check if proc was deleted while sleeping */
    if (!is_still_valid_proc(pid, stime)) {
        free(data);
        return 0;
    }

    curproc = process_status(pid);
    seL4_SetMR(0, 0);
    send_reply(reply_cap);
    free(data);
}

void syscall_usleep(seL4_CPtr reply_cap) {
    int msec = seL4_GetMR(1);

    /* Make sure sec is positive else reply */
    if (msec < 0) {
        send_err(reply_cap, -1);
        return;
    } else {
        seL4_Word *data = malloc(sizeof(seL4_Word) * 3);
        data[0] = reply_cap;
        data[1] = curproc->pid;
        data[2] = curproc->stime;
        register_timer(msec * 1000, &uwakeup, (void *) data);
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
    if (pos < 0 || nbyte == 0) {
        send_err(reply_cap, -1);
        return;
    }

    struct vnode *vnode;
    int err = vfs_get("", &vnode);
    if (err) {
        send_err(reply_cap, -1);
        return;
    }

    struct uio uio = {
        .uaddr = uaddr,
        .vaddr = NULL,
        .size = nbyte,
        .remaining = nbyte,
        .offset = pos
    };

    if (vnode->ops->vop_getdirent == NULL) {
        err = -1;
    } else {
        pin_frame_entry(uaddr, nbyte);
        err = vnode->ops->vop_getdirent(vnode, &uio);
        unpin_frame_entry(uaddr, nbyte);
    }
    if (err) {
        send_err(reply_cap, -1);
        return;
    }

    /* Reply */
    seL4_SetMR(0, uio.size - uio.remaining);
    send_reply(reply_cap);
}

void syscall_stat(seL4_CPtr reply_cap) {
    seL4_Word uaddr = seL4_GetMR(1);
    seL4_Word ustat_buf = seL4_GetMR(2);

    if (validate_uaddr(reply_cap, uaddr, 0)) return;
    if (validate_uaddr(reply_cap, ustat_buf, 0)) return;

    char path_sos_vaddr[MAX_PATH_LEN];
    /* Make sure path address is mapped */
    seL4_Word sos_vaddr;
    int err = sos_map_page(uaddr, &sos_vaddr, curproc);
    if (err && err != ERR_ALREADY_MAPPED) {
        send_err(reply_cap, -1);
        return;
    }

    sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
    sos_vaddr |= (uaddr & PAGE_MASK_4K);

    pin_frame_entry(ustat_buf, sizeof(sos_stat_t));
    pin_frame_entry(uaddr, MAX_PATH_LEN);
    err = get_safe_path(path_sos_vaddr, uaddr, sos_vaddr, MAX_PATH_LEN);
    unpin_frame_entry(uaddr, MAX_PATH_LEN);

    if (err) {
        unpin_frame_entry(ustat_buf, sizeof(sos_stat_t));
        send_err(reply_cap, -1);
        return;
    }

    /* Get vnode */
    struct vnode *vnode;
    err = vfs_get(path_sos_vaddr, &vnode);
    if (err) {
        unpin_frame_entry(ustat_buf, sizeof(sos_stat_t));
        send_err(reply_cap, -1);
        return;
    }


    if (vnode->ops->vop_stat == NULL) {
        err = -1;
    } else {
        err = vnode->ops->vop_stat(vnode, ustat_buf);
    }
    unpin_frame_entry(ustat_buf, sizeof(sos_stat_t));
    if (err) {
        send_err(reply_cap, -1);
        return;
    }

    err = vfs_close(vnode, 0);
    if (err) {
        send_err(reply_cap, -1);
        return;
    }

    /* Reply */
    seL4_SetMR(0, 0);
    send_reply(reply_cap);
}

void syscall_write(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word uaddr = seL4_GetMR(2);
    seL4_Word ubuf_size = seL4_GetMR(3);
    int ofd = curproc->addrspace->fd_table[fd].ofd;

    if (validate_buffer_size(reply_cap, ubuf_size)) return;
    if (validate_uaddr(reply_cap, uaddr, ubuf_size)) return;
    if (validate_fd(reply_cap, fd)) return;
    if (validate_ofd(reply_cap, ofd)) return;
    if (validate_ofd_mode(reply_cap, ofd, FM_WRITE)) return;

    struct oft_entry *entry = &of_table[ofd];
    struct vnode *vnode = of_table[ofd].vnode;

    struct uio uio = {
        .uaddr = uaddr,
        .vaddr = NULL,
        .size = ubuf_size,
        .remaining = ubuf_size,
        .offset = entry->offset
    };

    int err;
    if (vnode->ops->vop_write == NULL) {
        send_err(reply_cap, -1);
        return;
    } else {
        pin_frame_entry(uaddr, ubuf_size);
        err = vnode->ops->vop_write(vnode, &uio);
        unpin_frame_entry(uaddr, ubuf_size);
        
        if (err) {
            send_err(reply_cap, -1);
            return;
        }

        entry->offset = uio.offset;
    }

    /* Reply */
    seL4_SetMR(0, uio.size - uio.remaining);
    send_reply(reply_cap);
}

void syscall_read(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word uaddr = seL4_GetMR(2);
    seL4_Word ubuf_size = seL4_GetMR(3);
    int ofd = curproc->addrspace->fd_table[fd].ofd;

    if (validate_buffer_size(reply_cap, ubuf_size)) return;
    if (validate_uaddr(reply_cap, uaddr, ubuf_size)) return;
    if (validate_fd(reply_cap, fd)) return;
    if (validate_ofd(reply_cap, ofd)) return;
    if (validate_ofd_mode(reply_cap, ofd, FM_READ)) return;

    struct oft_entry *entry = &of_table[ofd];
    struct vnode *vnode = of_table[ofd].vnode;

    struct uio uio = {
        .uaddr = uaddr,
        .vaddr = NULL,
        .size = ubuf_size,
        .remaining = ubuf_size,
        .offset = entry->offset,
        .pcb = curproc
    };

    int err;
    if (vnode->ops->vop_read == NULL) {
        send_err(reply_cap, -1);
        return;
    } else {
        pin_frame_entry(uaddr, ubuf_size);
        err = vnode->ops->vop_read(vnode, &uio);
        unpin_frame_entry(uaddr, ubuf_size);

        if (err) {
            send_err(reply_cap, -1);
            return;
        }

        entry->offset = uio.offset;
    }

    seL4_SetMR(0, uio.size - uio.remaining);
    send_reply(reply_cap);
}

void syscall_open(seL4_CPtr reply_cap) {
    seL4_Word fd_count = curproc->addrspace->fd_count;

    seL4_Word uaddr = seL4_GetMR(1);
    fmode_t access_mode = seL4_GetMR(2);

    if (validate_max_fd(reply_cap, fd_count)) return;
    if (validate_max_ofd(reply_cap, ofd_count)) return;
    if (validate_uaddr(reply_cap, uaddr, 0)) return;

    char path_sos_vaddr[MAX_PATH_LEN];
    /* Make sure address is mapped */
    seL4_Word sos_vaddr;
    int err = sos_map_page(uaddr, &sos_vaddr, curproc);
    if (err && err != ERR_ALREADY_MAPPED) {
        send_err(reply_cap, -1);
        return;
    }

    sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
    sos_vaddr |= (uaddr & PAGE_MASK_4K);

    pin_frame_entry(uaddr, MAX_PATH_LEN);
    err = get_safe_path(path_sos_vaddr, uaddr, sos_vaddr, MAX_PATH_LEN);
    unpin_frame_entry(uaddr, MAX_PATH_LEN);

    if (err) {
        send_err(reply_cap, -1);
        return;
    }

    /* Access mode */
    int sos_access_mode = 0;
    if (access_mode == O_RDWR) {
        sos_access_mode = FM_READ | FM_WRITE;
    } else if (access_mode == O_RDONLY) {
        sos_access_mode = FM_READ;
    } else if (access_mode == O_WRONLY) {
        sos_access_mode = FM_WRITE;
    } else {
        /* Invalid access mode */
        send_err(reply_cap, -1);
        return;
    }

    struct vnode *ret_vnode;

    err = vfs_open((char *) path_sos_vaddr, sos_access_mode, &ret_vnode);

    if (err) {
        send_err(reply_cap, -1);
        return;
    } else {
        /* FD Table */
        int free_fd = 0;
        for (int i = 0; i < PROCESS_MAX_FILES; i++) {
            if (curproc->addrspace->fd_table[i].ofd == -1) {
                free_fd = i;
                break;
            }
        }

        /* Set FD */
        curproc->addrspace->fd_count++;
        seL4_SetMR(0, free_fd);

        /* OF Table */
        curproc->addrspace->fd_table[free_fd].ofd = curr_free_ofd;
        of_table[curr_free_ofd].vnode = ret_vnode;

        of_table[curr_free_ofd].file_info.st_fmode = sos_access_mode;
        of_table[curr_free_ofd].ref_count++;

        ofd_count++;
        if (ofd_count != MAX_OPEN_FILE) {
            while (of_table[curr_free_ofd].vnode != NULL) {
                curr_free_ofd = (curr_free_ofd + 1) % MAX_OPEN_FILE;
            }
        } else {
            curr_free_ofd = -1;
        }
    }

    /* Reply */
    send_reply(reply_cap);
}

void syscall_close(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word ofd = curproc->addrspace->fd_table[fd].ofd;

    if (validate_fd(reply_cap, fd)) return;
    if (validate_ofd(reply_cap, ofd)) return;

    curproc->addrspace->fd_table[fd].ofd = -1;
    curproc->addrspace->fd_count--;
    of_close(ofd);

    /* Reply */
    seL4_SetMR(0, 0);
    send_reply(reply_cap);
}

void syscall_process_create(seL4_CPtr reply_cap, seL4_Word badge) {
    seL4_Word path_uaddr = seL4_GetMR(1);

    if (validate_uaddr(reply_cap, path_uaddr, 0)) return;

    curproc->status = PROCESS_STATUS_BUSY;

    char path_sos_vaddr[MAX_PATH_LEN];
    /* Make sure address is mapped */
    seL4_Word sos_vaddr;
    int err = sos_map_page(path_uaddr, &sos_vaddr, curproc);
    if (err && err != ERR_ALREADY_MAPPED) {
        send_err(reply_cap, -1);
        return;
    }

    sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
    sos_vaddr |= (path_uaddr & PAGE_MASK_4K);

    pin_frame_entry(path_uaddr, MAX_PATH_LEN);
    err = get_safe_path(path_sos_vaddr, path_uaddr, sos_vaddr, MAX_PATH_LEN);
    unpin_frame_entry(path_uaddr, MAX_PATH_LEN);
    if (err) {
        send_err(reply_cap, -1);
        return;
    }

    int new_pid = process_new(path_sos_vaddr, _sos_ipc_ep_cap, badge);

    if (curproc->status == PROCESS_STATUS_BUSY) {
        curproc->status = PROCESS_STATUS_NOT_BUSY;
    }

    /* new_pid is -1 on error */
    seL4_SetMR(0, new_pid);
    send_reply(reply_cap);

    return;
}

void syscall_process_delete(seL4_CPtr reply_cap, seL4_Word badge) {
    pid_t pid = seL4_GetMR(1);

    if (validate_pid(reply_cap, pid)) return;
    if (validate_self_destruct(reply_cap, pid)) return;

    /* Mark process to self destruct */
    struct PCB *pcb = process_status(pid);

    if (pcb->status == PROCESS_STATUS_BUSY) {
        /* The process is in the middle of deleting/creating a process, wait for it to finish and then self destruct */
        pcb->status = PROCESS_STATUS_SELF_DESTRUCT;
    } else {
        curproc->status = PROCESS_STATUS_BUSY;

        process_destroy(pid);

        if (curproc->status == PROCESS_STATUS_BUSY) {
            curproc->status = PROCESS_STATUS_NOT_BUSY;
        }
    }

    if (pid != badge) {
        seL4_SetMR(0, 0);
        send_reply(reply_cap);
    }

    return;
}

void syscall_process_id(seL4_CPtr reply_cap, seL4_Word badge) {
    seL4_SetMR(0, badge);
    send_reply(reply_cap);
    return;
}

void syscall_process_wait(seL4_CPtr reply_cap, seL4_Word badge) {
    pid_t pid = seL4_GetMR(1);

    if (validate_pid(reply_cap, pid)) return;
    if (validate_self_destruct(reply_cap, pid)) return;

    struct PCB *child = process_status(pid);

    if (pid == PROCESS_WAIT_ANY || child != NULL) {
        /* If not PROCESS_WAIT_ANY, make sure pid is an actual child */
        if (pid != PROCESS_WAIT_ANY && child->parent != badge) {
            send_err(reply_cap, -1);
            return;
        }
        curproc->wait = pid;
        yield();
        pid = curproc->wait;
        curproc->wait = PROCESS_WAIT_NONE;
    }

    seL4_SetMR(0, pid);
    send_reply(reply_cap);
    return;
}

void syscall_process_status(seL4_CPtr reply_cap) {
    sos_process_t *uaddr = seL4_GetMR(1);
    seL4_Word max_req_procs = seL4_GetMR(2);

    seL4_Word size = sizeof(sos_process_t) * max_req_procs;
    pid_t pid = 0;
    int procs = 0;

    if (validate_uaddr(reply_cap, uaddr, size)) return;

    pin_frame_entry(uaddr, size);
    for (procs = 0; procs < max_req_procs && pid < MAX_PROCESSES; procs++) {
        struct PCB *pcb;
        /* Find a valid process */
        do {
            pcb = process_status(pid++);
        } while ((pcb == NULL || pcb->status == PROCESS_STATUS_SELF_DESTRUCT) && pid < MAX_PROCESSES);
        if (pcb == NULL || pcb->status == PROCESS_STATUS_SELF_DESTRUCT) break;

        /* Set buffer data */
        sos_process_t buffer;
        buffer.pid = pid - 1;
        buffer.size = pcb->addrspace->page_count;
        buffer.stime = pcb->stime;
        strcpy(buffer.command, pcb->app_name);

        seL4_Word sos_vaddr;
        int err = sos_map_page(&uaddr[procs], &sos_vaddr, curproc);
        if (err && err != ERR_ALREADY_MAPPED) {
            unpin_frame_entry(uaddr, size);
            send_err(reply_cap, procs);
            return;
        }

        /* Add offset */
        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        seL4_Word cast_uaddr = (seL4_Word) (&uaddr[procs]);
        sos_vaddr |= (cast_uaddr & PAGE_MASK_4K);

        if (PAGE_ALIGN_4K(cast_uaddr + sizeof(sos_process_t)) != PAGE_ALIGN_4K(cast_uaddr)) {
            seL4_Word sos_vaddr_next;
            int err = sos_map_page(PAGE_ALIGN_4K(cast_uaddr + sizeof(sos_process_t)), &sos_vaddr_next, curproc);
            if (err && err != ERR_ALREADY_MAPPED) {
                unpin_frame_entry(uaddr, size);
                send_err(reply_cap, procs);
                return;
            }

            sos_vaddr_next = PAGE_ALIGN_4K(sos_vaddr);

            seL4_Word first_half = (PAGE_ALIGN_4K(cast_uaddr + sizeof(sos_process_t)) - (seL4_Word) cast_uaddr);

            /* Boundary */
            memcpy(sos_vaddr, &buffer, first_half);
            memcpy(sos_vaddr_next, &buffer + first_half, sizeof(sos_process_t) - first_half);
        } else {
            memcpy(sos_vaddr, &buffer, sizeof(sos_process_t));
        }
    }
    unpin_frame_entry(uaddr, size);

    seL4_SetMR(0, procs);
    send_reply(reply_cap);
}
