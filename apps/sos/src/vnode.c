#include <string.h>
#include <utils/page.h>
#include <nfs/nfs.h>
#include <clock/clock.h>

#include "vnode.h"
#include "hashtable.h"
#include "process.h"
#include "coroutine.h"
#include "mapping.h"

#include <sys/panic.h>
#include <sys/stat.h>

#define VNODE_TABLE_SLOTS 64
#define NUM_ARG 4
#define MAX_WRITE_SIZE 1024

/* Externs */
extern struct PCB *curproc;
extern int curr_coroutine_id;
extern fhandle_t mnt_point;
extern const char *swapfile;

/* Vnode */
static struct vnode *vnode_new(char *path);

/* Default Vops */
static int file_create(struct vnode *vnode);

static int vnode_open(struct vnode *vnode, int mode);
static int vnode_close(struct vnode *vnode);
static int vnode_read(struct vnode *vnode, struct uio *uio);
static int vnode_write(struct vnode *vnode, struct uio *uio);
static int vnode_stat(struct vnode *vnode, sos_stat_t *stat);
static int vnode_getdirent(struct vnode *vnode, struct uio *uio);

/* Callbacks */
static void vnode_write_cb(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count);

static void vnode_create_cb(uintptr_t token, nfs_stat_t status, fhandle_t *fh, fattr_t *fattr);

static void vnode_open_cb(uintptr_t token, nfs_stat_t status, fhandle_t *fh, fattr_t *fattr);

static void vnode_read_cb(uintptr_t token, nfs_stat_t status, fattr_t *fattr, int count, void *data);

static void vnode_stat_cb(uintptr_t token, nfs_stat_t status, fhandle_t *fh, fattr_t *fattr);

static void vnode_readdir_cb(uintptr_t token, enum nfs_stat status, int num_files, char *file_names[], nfscookie_t nfscookie);

/* Devices */
static void dev_list_init();
static int is_dev(char *dev);

/* Default vops */
static const struct vnode_ops default_ops = {
    &vnode_open,
    &vnode_close,
    &vnode_read,
    &vnode_write,
    &vnode_stat,
    &vnode_getdirent
};

/* Variables */
static struct dev dev_list[MAX_DEV_NUM];
static struct hashtable *vnode_table;

/*
 * =======================================================
 * DEVICES
 * =======================================================
 */
static void dev_list_init() {
    for (int i = 0 ; i < MAX_DEV_NUM; i++) {
        dev_list[i].name = NULL;
        dev_list[i].ops = NULL;
    }
}

int dev_add(char *name, struct vnode_ops *ops) {
    int len = strnlen(name, MAX_DEV_NAME);
    if (len == MAX_DEV_NAME) {
        return -1;
    }

    /* Find empty device slot */
    for (int i = 0; i < MAX_DEV_NUM; i++) {
        if (dev_list[i].name == NULL) {
            /* Add new device */
            dev_list[i].name = malloc(len + 1);
            if (dev_list[i].name == NULL) return -1;
            strcpy(dev_list[i].name, name);
            dev_list[i].ops = ops;
            return 0;
        }
    }

    /* No free device slot */
    return -1;
}

int dev_remove(char *name) {
    for (int i = 0; i < MAX_DEV_NUM; i++) {
        if (dev_list[i].name == NULL) continue;

        if (!strcmp(dev_list[i].name, name)) {
            /* Found device */
            free(dev_list[i].name);
            dev_list[i].name = NULL;
            return 0;
        }
    }

    /* Could not find device */
    return -1;
}

static int is_dev(char *dev) {
    for (int i = 0; i < MAX_DEV_NUM; i++) {
        if (dev_list[i].name == NULL) continue;

        if (!strcmp(dev_list[i].name, dev)) {
            /* Found device */
            return i;
        }
    }

    /* Could not find device */
    return -1;
}


/*
 * =======================================================
 * VFS
 * =======================================================
 */
int vfs_init() {
    vnode_table = hashtable_new(VNODE_TABLE_SLOTS);
    if (vnode_table == NULL) {
        return 1;
    }

    dev_list_init();

    return 0;
}

int vfs_get(char *path, struct vnode **ret_vnode) {
    struct vnode *vnode;

    /* Check if vnode for path already exists */
    struct hashtable_entry *entry = hashtable_get(vnode_table, path);
    if (entry == NULL) {
        /* New vnode */
        vnode = vnode_new(path);
        if (vnode == NULL) return -1;

        int err = hashtable_insert(vnode_table, vnode->path, vnode);
        if (err) return -1;
    } else {
        /* Already exists */
        vnode = (struct vnode *) entry->value;
    }

    *ret_vnode = vnode;
    return 0;
}

int vfs_open(char *path, int mode, struct vnode **ret_vnode) {
    /* Get vnode */
    struct vnode *vnode;

    int err = vfs_get(path, &vnode);
    if (err) return err;

    /* Open the vnode */
    err = vnode->ops->vop_open(vnode, mode);
    /* Check for errors, include single read */
    if (err) return err;

    /* Inc ref counts */
    if ((mode & FM_READ) != 0) {
        vnode->read_count++;
    }
    if ((mode & FM_WRITE) != 0) {
        vnode->write_count++;
    }

    *ret_vnode = vnode;
    return 0;
}

int vfs_close(struct vnode *vnode, int mode) {
    int err = vnode->ops->vop_close(vnode);
    if (err) return -1;

    /* Dec ref counts */
    if ((mode & FM_READ) != 0) {
        vnode->read_count--;
    }
    if ((mode & FM_WRITE) != 0) {
        vnode->write_count--;
    }

    if (vnode->read_count + vnode->write_count == 0) {
        /* No more references left - Remove vnode */
        hashtable_remove(vnode_table, vnode->path);

        if (vnode->fh != NULL) free(vnode->fh);
        if (vnode->fattr != NULL) free(vnode->fattr);
        if (vnode->data != NULL) free(vnode->data);

        free(vnode->path);
        free(vnode);
    }

    return 0;
}


/*
 * =======================================================
 * GENERAL VNODE FUNCTIONS
 * =======================================================
 */
static struct vnode *vnode_new(char *path) {
    struct vnode *vnode = malloc(sizeof(struct vnode));
    if (vnode == NULL) {
        return NULL;
    }

    int dev_id = is_dev(path);

    /* Initialise variables */
    vnode->path = malloc(strlen(path) + 1);
    if (vnode->path == NULL) {
        return NULL;
    }

    strcpy(vnode->path, path);
    vnode->read_count = 0;
    vnode->write_count = 0;
    vnode->data = NULL;
    vnode->fh = NULL;
    vnode->fattr = NULL;

    if (dev_id != -1) {
        /* Handle device */
        vnode->ops = dev_list[dev_id].ops;
    } else {
        /* Handle file */
        vnode->ops = &default_ops;
    }

    return vnode;
}


/*
 * =======================================================
 * CLOSE
 * =======================================================
 */
static int vnode_close(struct vnode *vnode) {
    return 0;
}


/*
 * =======================================================
 * GETDIRENT
 * =======================================================
 */
static int vnode_getdirent(struct vnode *vnode, struct uio *uio) {
    seL4_Word cookies = 0;

    do {
        set_routine_arg(curr_coroutine_id, 0, uio);
        set_routine_arg(curr_coroutine_id, 1, curproc);

        seL4_Word *token = malloc(sizeof(seL4_Word) * 3);
        if (token == NULL) return -1;

        token[0] = curproc->pid;
        token[1] = curproc->stime;
        token[2] = curr_coroutine_id;

        nfs_readdir(&mnt_point, cookies, vnode_readdir_cb, token);
        yield();

        int status = get_routine_arg(curr_coroutine_id, 0);
        cookies = get_routine_arg(curr_coroutine_id, 1);
        int err = get_routine_arg(curr_coroutine_id, 2);
        if (err) return -1;

    } while (uio->offset > 0 && cookies != 0);

    /* Error - reached over the end */
    if (uio->offset > 0 && cookies == 0) {
        return -1;
    }

    return 0;
}

static void vnode_readdir_cb(uintptr_t token_ptr, enum nfs_stat status, int num_files, char *file_names[], nfscookie_t nfscookie) {
    seL4_Word *token = (seL4_Word *) token_ptr;
    seL4_Word pid = token[0];
    seL4_Word stime = token[1];
    seL4_Word coroutine_id = token[2];
    free(token);

    /* Check if proc was deleted */
    if (!is_still_valid_proc(pid, stime)) {
        return;
    }

    struct uio *uio = get_routine_arg((int) coroutine_id, 0);
    struct PCB *pcb = get_routine_arg((int) coroutine_id, 1);

    set_resume(coroutine_id);

    set_routine_arg(coroutine_id, 0, status);
    set_routine_arg(coroutine_id, 1, nfscookie);
    set_routine_arg(coroutine_id, 2, 0);

    if (uio->offset == 0 && num_files == 0) {
        /* Valid next pos NULL */
        uio->uaddr = "\0";

    } else if (uio->offset < num_files) {
        /* Check if we need to map a second page */
        int len = strlen(file_names[uio->offset]) + 1;
        if (len > uio->size) {
            /* Hard set to stop it continuing */
            set_routine_arg(coroutine_id, 1, 0);
        }

        seL4_Word uaddr = uio->uaddr;
        seL4_Word uaddr_end = uio->uaddr + len;

        seL4_Word sos_vaddr;

        int err = sos_map_page(uaddr, &sos_vaddr, pcb);
        if (err && err != ERR_ALREADY_MAPPED) {
            set_routine_arg(coroutine_id, 2, -1);
            return;
        }

        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        sos_vaddr |= (uaddr & PAGE_MASK_4K);
        if (PAGE_ALIGN_4K(uaddr) != PAGE_ALIGN_4K(uaddr_end)) {
            seL4_Word uaddr_next = PAGE_ALIGN_4K(uaddr) + PAGE_SIZE_4K;

            seL4_Word sos_vaddr_next;
            err = sos_map_page(uaddr_next, &sos_vaddr_next, pcb);
            if (err && err != ERR_ALREADY_MAPPED) {
                set_routine_arg(coroutine_id, 2, -1);
                return;
            }

            sos_vaddr_next = PAGE_ALIGN_4K(sos_vaddr_next);

            /* Boundary write */
            memcpy(sos_vaddr, file_names[uio->offset], uaddr_next - uaddr);
            /* Write rest of next page */
            strcpy(sos_vaddr_next, ((char *) file_names[uio->offset]) + uaddr_next - uaddr);
        } else {
            /* All on same page */
            /* Note: safe to use strcpy since file name is
             * guaranteed to be null terminated and we already
             * know it is all on the same page */
            strcpy(sos_vaddr, file_names[uio->offset]);
        }

        uio->remaining = uio->size - len;
        uio->offset = 0;

    } else {
        uio->offset -= num_files;
    }
}


/*
 * =======================================================
 * STAT
 * =======================================================
 */
static int vnode_stat(struct vnode *vnode, sos_stat_t *stat) {
    seL4_Word *token = malloc(sizeof(seL4_Word) * 3);
    if (token == NULL) return -1;

    token[0] = curproc->pid;
    token[1] = curproc->stime;
    token[2] = curr_coroutine_id;

    nfs_lookup(&mnt_point, vnode->path, (nfs_lookup_cb_t) vnode_stat_cb, token);
    yield();

    int status = get_routine_arg(curr_coroutine_id, 0);
    fattr_t *fattr = (fattr_t *) get_routine_arg(curr_coroutine_id, 1);
    int err = get_routine_arg(curr_coroutine_id, 2);
    if (err) {
        if (fattr != NULL) free(fattr);
        return -1;
    }

    int ret = 0;
    if (status == NFS_OK) {
        seL4_Word sos_vaddr;
        seL4_Word uaddr = (seL4_Word) stat;
        err = sos_map_page(uaddr, &sos_vaddr, curproc);
        if (err && err != ERR_ALREADY_MAPPED) {
            free(fattr);
            return -1;
        }

        /* Add offset */
        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        sos_vaddr |= (uaddr & PAGE_MASK_4K);

        seL4_Word *buffer;

        if (PAGE_ALIGN_4K(uaddr + sizeof(stat)) != PAGE_ALIGN_4K(uaddr)) {
            buffer = malloc(sizeof(sos_stat_t));
            if (buffer == NULL) {
                free(fattr);
                return -1;
            }
        } else {
            buffer = (seL4_Word *) sos_vaddr;
        }

        buffer[0] = fattr->type;
        buffer[1] = fattr->mode;
        buffer[2] = fattr->size;
        buffer[3] = fattr->ctime.seconds * 1000 + fattr->ctime.useconds / 1000;
        buffer[4] = fattr->atime.seconds * 1000 + fattr->atime.useconds / 1000;

        if (PAGE_ALIGN_4K(uaddr + sizeof(stat)) != PAGE_ALIGN_4K(uaddr)) {
            seL4_Word sos_vaddr_next;
            err = sos_map_page(PAGE_ALIGN_4K(uaddr + sizeof(stat)), &sos_vaddr_next, curproc);
            if (err && err != ERR_ALREADY_MAPPED) {
                free(buffer);
                free(fattr);
                return -1;
            }

            sos_vaddr_next = PAGE_ALIGN_4K(sos_vaddr);

            seL4_Word first_half = (PAGE_ALIGN_4K(uaddr + sizeof(stat)) - (seL4_Word) stat);

            /* Boundary */
            memcpy(sos_vaddr, buffer, first_half);
            memcpy(sos_vaddr_next, buffer + first_half, sizeof(stat) - first_half);

            free(buffer);
        }

    } else {
        ret = -1;
    }

    free(fattr);

    return ret;
}

static void vnode_stat_cb(uintptr_t token_ptr, nfs_stat_t status, fhandle_t *fh, fattr_t *fattr) {
    seL4_Word *token = (seL4_Word *) token_ptr;
    seL4_Word pid = token[0];
    seL4_Word stime = token[1];
    seL4_Word coroutine_id = token[2];

    free(token);

    /* Check if proc was deleted */
    if (!is_still_valid_proc(pid, stime)) {
        return;
    }

    set_resume(coroutine_id);

    set_routine_arg(coroutine_id, 0, status);

    fattr_t *fattr_ptr = malloc(sizeof(fattr_t));
    if (fattr_ptr == NULL) {
        set_routine_arg(coroutine_id, 1, NULL);
        set_routine_arg(coroutine_id, 2, -1);
        return;
    }
    set_routine_arg(coroutine_id, 1, fattr_ptr);
    memcpy(fattr_ptr, fattr, sizeof(fattr_t));

    set_routine_arg(coroutine_id, 2, 0);
}


/*
 * =======================================================
 * CREATE
 * =======================================================
 */
static int file_create(struct vnode *vnode) {
    uint64_t timestamp = time_stamp();
    timeval_t curr_time;
    curr_time.seconds = timestamp / 1000;
    curr_time.useconds = (timestamp - curr_time.seconds * 1000) / 10000000;

    sattr_t sattr = {
        .mode = S_IROTH | S_IWOTH,
        .uid = 1000,
        .gid = 1000,
        .size = 0,
        .atime = curr_time,
        .mtime = curr_time
    };

    seL4_Word *token = malloc(sizeof(seL4_Word) * 3);
    if (token == NULL) return -1;

    token[0] = curproc->pid;
    token[1] = curproc->stime;
    token[2] = curr_coroutine_id;

    nfs_create(&mnt_point, vnode->path, &sattr, (nfs_create_cb_t) vnode_create_cb, token);

    /* Mask */
    set_routine_arg(curr_coroutine_id, 0, 1);

    yield();

    return 0;
}

static void vnode_create_cb(uintptr_t token_ptr, nfs_stat_t status, fhandle_t *fh, fattr_t *fattr) {
    seL4_Word *token = (seL4_Word *) token_ptr;
    seL4_Word pid = token[0];
    seL4_Word stime = token[1];
    seL4_Word coroutine_id = token[2];

    free(token);

    /* Check if proc was deleted */
    if (!is_still_valid_proc(pid, stime)) {
        return;
    }

    set_resume(coroutine_id);

    /* Mask */
    set_routine_arg(coroutine_id, 0, 0);

    set_routine_arg(coroutine_id, 1, status);

    fhandle_t *fhandle_ptr = malloc(sizeof(fhandle_t));
    if (fhandle_ptr == NULL) {
        set_routine_arg(coroutine_id, 2, NULL);
        set_routine_arg(coroutine_id, 3, NULL);
        set_routine_arg(coroutine_id, 4, -1);
        return;
    }
    set_routine_arg(coroutine_id, 2, fhandle_ptr);
    memcpy(fhandle_ptr, fh, sizeof(fhandle_t));

    fattr_t *fattr_ptr = malloc(sizeof(fattr_t));
    if (fattr_ptr == NULL) {
        set_routine_arg(coroutine_id, 3, NULL);
        set_routine_arg(coroutine_id, 4, -1);
        return;
    }
    set_routine_arg(coroutine_id, 3, fattr_ptr);
    memcpy(fattr_ptr, fattr, sizeof(fattr_t));

    set_routine_arg(coroutine_id, 4, 0);
}


/*
 * =======================================================
 * OPEN
 * =======================================================
 */
static int vnode_open(struct vnode *vnode, fmode_t mode) {
    if (vnode->fh != NULL) return 0;

    seL4_Word *token = malloc(sizeof(seL4_Word) * 3);
    if (token == NULL) return -1;

    token[0] = curproc->pid;
    token[1] = curproc->stime;
    token[2] = curr_coroutine_id;

    nfs_lookup(&mnt_point, vnode->path, (nfs_lookup_cb_t) vnode_open_cb, token);
    set_routine_arg(curr_coroutine_id, 0, 1); /* Mask */
    yield();

    int status = get_routine_arg(curr_coroutine_id, 1);
    fhandle_t *fhandle_ptr = (fhandle_t *) get_routine_arg(curr_coroutine_id, 2);
    fattr_t *fattr_ptr = (fattr_t *) get_routine_arg(curr_coroutine_id, 3);
    int err = get_routine_arg(curr_coroutine_id, 4);
    if (err) {
        set_routine_arg(curr_coroutine_id, 0, -1);
        if (fhandle_ptr != NULL) free(fhandle_ptr);
        if (fattr_ptr != NULL) free(fattr_ptr);
        return -1;
    }

    if (status == NFS_OK) {
        /* Check permissions */
        int valid_mode = 1;
        int fattr_mode = fattr_ptr->mode;
        if (mode & FM_READ) {
            if ((fattr_mode & S_IROTH) == 0) valid_mode = 0;
        }
        if (mode & FM_WRITE) {
            if ((fattr_mode & S_IWOTH) == 0) valid_mode = 0;
        }
        if (!valid_mode) {
            set_routine_arg(curr_coroutine_id, 0, -1);
            free(fhandle_ptr);
            free(fattr_ptr);
            return -1;
        }

        vnode->fh = fhandle_ptr;
        vnode->fattr = fattr_ptr;

    } else if (status == NFSERR_NOENT) {
        free(fhandle_ptr);
        free(fattr_ptr);

        /* Create new file */
        file_create(vnode);

        /* Note: Permissions should be fine since we create with RW access */

        status = get_routine_arg(curr_coroutine_id, 1);
        fhandle_ptr = (fhandle_t *) get_routine_arg(curr_coroutine_id, 2);
        fattr_ptr = (fattr_t *) get_routine_arg(curr_coroutine_id, 3);
        err = get_routine_arg(curr_coroutine_id, 4);
        if (err) {
            set_routine_arg(curr_coroutine_id, 0, -1);
            if (fhandle_ptr != NULL) free(fhandle_ptr);
            if (fattr_ptr != NULL) free(fattr_ptr);
            return -1;
        }

        if (status == NFS_OK) {
            vnode->fh = fhandle_ptr;
            vnode->fattr = fattr_ptr;
        } else {
            set_routine_arg(curr_coroutine_id, 0, -1);
            free(fhandle_ptr);
            free(fattr_ptr);
            return -1;
        }

    } else {
        set_routine_arg(curr_coroutine_id, 0, -1);
        free(fhandle_ptr);
        free(fattr_ptr);
        return -1;
    }

    return 0;
}

static void vnode_open_cb(uintptr_t token_ptr, nfs_stat_t status, fhandle_t *fh, fattr_t *fattr) {
    seL4_Word *token = (seL4_Word *) token_ptr;
    seL4_Word pid = token[0];
    seL4_Word stime = token[1];
    seL4_Word coroutine_id = token[2];

    free(token);

    /* Check if proc was deleted */
    if (!is_still_valid_proc(pid, stime)) {
        return;
    }

    set_resume(coroutine_id);

    set_routine_arg(coroutine_id, 0, 0);

    set_routine_arg(coroutine_id, 1, status);

    fhandle_t *fhandle_ptr = malloc(sizeof(fhandle_t));
    if (fhandle_ptr == NULL) {
        set_routine_arg(coroutine_id, 2, NULL);
        set_routine_arg(coroutine_id, 3, NULL);
        set_routine_arg(coroutine_id, 4, -1);
        return;
    }
    set_routine_arg(coroutine_id, 2, fhandle_ptr);
    memcpy(fhandle_ptr, fh, sizeof(fhandle_t));

    fattr_t *fattr_ptr = malloc(sizeof(fattr_t));
    if (fattr_ptr == NULL) {
        set_routine_arg(coroutine_id, 3, NULL);
        set_routine_arg(coroutine_id, 4, -1);
        return;
    }
    set_routine_arg(coroutine_id, 3, fattr_ptr);
    memcpy(fattr_ptr, fattr, sizeof(fattr_t));

    set_routine_arg(coroutine_id, 4, 0);
}


/*
 * =======================================================
 * READ
 * =======================================================
 */
static int vnode_read(struct vnode *vnode, struct uio *uio) {
    int err;
    seL4_Word sos_vaddr;
    seL4_Word buf_size = uio->size;
    struct PCB *proc = uio->pcb;
    char *uaddr = NULL;

    if (uio->uaddr != NULL) {
        uaddr = uio->uaddr;
    }

    while (buf_size > 0) {
        seL4_Word size;
        seL4_Word end_uaddr, uaddr_next;

        if (uio->uaddr != NULL) {
            /* uaddr */

            /* We must find the sos_vaddrs for each page */
            end_uaddr = (seL4_Word) uaddr + buf_size;
            uaddr_next = PAGE_ALIGN_4K((seL4_Word) uaddr) + PAGE_SIZE_4K;
            if (end_uaddr >= uaddr_next) {
                size = uaddr_next - (seL4_Word) uaddr;
            } else {
                size = buf_size;
            }
        } else {
            /* vaddr */
            size = buf_size;
        }

        /* Set sos_vaddr */
        if (uio->uaddr != NULL) {
            /* uaddr */
            err = sos_map_page((seL4_Word) uaddr, &sos_vaddr, proc);
            if (err && err != ERR_ALREADY_MAPPED) {
                return -1;
            }

            sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
            sos_vaddr |= ((seL4_Word) uaddr & PAGE_MASK_4K);
        } else {
            /* vaddr */
            sos_vaddr = uio->vaddr;
        }

        seL4_Word *token = malloc(sizeof(seL4_Word) * 3);
        if (token == NULL) return -1;

        token[0] = proc->pid;
        token[1] = proc->stime;
        token[2] = curr_coroutine_id;

        err = nfs_read(vnode->fh, uio->offset, size, (nfs_read_cb_t) vnode_read_cb, token);
        if (err) {
            free(token);
            return -1;
        }

        set_routine_arg(curr_coroutine_id, 0, 1);
        set_routine_arg(curr_coroutine_id, 1, sos_vaddr);

        yield();

        int status = get_routine_arg(curr_coroutine_id, 1);
        seL4_Word count = get_routine_arg(curr_coroutine_id, 2);
        if (status != NFS_OK) return -1;

        buf_size -= count;
        if (uio->uaddr != NULL) uaddr += count;

        uio->remaining -= count;
        uio->offset += count;

        if (uio->offset >= vnode->fattr->size) {
            return 0;
        }
    }

    return 0;
}

static void vnode_read_cb(uintptr_t token_ptr, nfs_stat_t status, fattr_t *fattr, int count, void *data) {
    seL4_Word *token = (seL4_Word *) token_ptr;
    seL4_Word pid = token[0];
    seL4_Word stime = token[1];
    seL4_Word coroutine_id = token[2];

    free(token);

    /* Check if proc was deleted */
    if (!is_still_valid_proc(pid, stime)) {
        return;
    }

    seL4_Word sos_vaddr = get_routine_arg(coroutine_id, 1);

    set_routine_arg(coroutine_id, 0, 0);

    set_routine_arg(coroutine_id, 1, status);

    set_routine_arg(coroutine_id, 2, count);

    set_resume(coroutine_id);

    if (status == NFS_OK) {
        memcpy((void *) sos_vaddr, data, count);
    } else {
        set_routine_arg(coroutine_id, 0, -1);
    }
}



/*
 * =======================================================
 * WRITE
 * =======================================================
 */
static int vnode_write(struct vnode *vnode, struct uio *uio) {
    int err;
    seL4_Word sos_vaddr;
    seL4_Word buf_size = uio->size;
    char *uaddr = NULL;

    /* Check if we got a uaddr or vaddr */
    if (uio->uaddr != NULL) {
        /* uaddr */
        uaddr = uio->uaddr;
        err = sos_map_page((seL4_Word) uaddr, &sos_vaddr, curproc);
        if (err && err != ERR_ALREADY_MAPPED) return -1;
    } else {
        /* vaddr */
        sos_vaddr = uio->vaddr;
    }

    while (buf_size > 0) {
        seL4_Word size;
        seL4_Word end_uaddr, uaddr_next, sos_vaddr_next;

        if (uio->uaddr != NULL) {
            /* uaddr */

            /* We must find the sos_vaddrs for each page */
            end_uaddr = (seL4_Word) uaddr + buf_size;
            uaddr_next = PAGE_ALIGN_4K((seL4_Word) uaddr) + PAGE_SIZE_4K;
            if (end_uaddr >= uaddr_next) {
                size = uaddr_next - (seL4_Word) uaddr;
            } else {
                size = buf_size;
            }

            sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
            /*Add offset*/
            sos_vaddr |= ((seL4_Word) uaddr & PAGE_MASK_4K);

            /* Update uaddr's sos_vaddr */
            sos_vaddr_next = uaddr_to_sos_vaddr(uaddr_next);
            if (uaddr_next < end_uaddr) {
                err = sos_map_page((seL4_Word) uaddr_next, &sos_vaddr_next, curproc);
                if (err && err != ERR_ALREADY_MAPPED) return -1;
            }
        } else {
            /* vaddr */
            size = buf_size;
        }

        int req_id = 0;
        while (size > 0) {
            seL4_Word *token = malloc(sizeof(seL4_Word) * 4);
            if (token == NULL) return -1;

            token[0] = curproc->pid;
            token[1] = curproc->stime;
            token[2] = curr_coroutine_id;
            token[3] = req_id;

            int offset = MAX_WRITE_SIZE * req_id;

            if (size > MAX_WRITE_SIZE) {
                err = nfs_write(vnode->fh,
                                uio->offset + offset,
                                MAX_WRITE_SIZE,
                                sos_vaddr + offset,
                                &vnode_write_cb,
                                token);
                if (err) {
                    free(token);
                    return -1;
                }

                size -= MAX_WRITE_SIZE;
                req_id++;
            } else {
                err = nfs_write(vnode->fh,
                                uio->offset + offset,
                                size,
                                sos_vaddr + offset,
                                &vnode_write_cb,
                                token);
                if (err) {
                    free(token);
                    return -1;
                }

                size = 0;

                set_routine_arg(curr_coroutine_id, 0, (1 << (req_id + 1)) - 1); /* Mask */
                set_routine_arg(curr_coroutine_id, 1, 0); /* Count */
            }
        }

        if (uio->uaddr != NULL) {
            sos_vaddr = sos_vaddr_next;
        }

        yield();

        int mask = get_routine_arg(curr_coroutine_id, 0);
        seL4_Word count = get_routine_arg(curr_coroutine_id, 1);
        if (mask) return -1;

        buf_size -= count;
        if (uio->uaddr != NULL) uaddr += count;

        uio->remaining -= count;
        uio->offset += count;
    }

    return 0;
}

static void vnode_write_cb(uintptr_t token_ptr, enum nfs_stat status, fattr_t *fattr, int count) {
    seL4_Word *token = (seL4_Word *) token_ptr;
    seL4_Word pid = token[0];
    seL4_Word stime = token[1];
    seL4_Word coroutine_id = token[2];
    seL4_Word req_id = token[3];

    free(token);

    /* Check if proc was deleted */
    if (!is_still_valid_proc(pid, stime)) {
        return;
    }

    if (status == NFS_OK) {
        seL4_Word req_mask = get_routine_arg(coroutine_id, 0) & (~(1 << req_id));
        set_routine_arg(coroutine_id, 0, req_mask);
        seL4_Word cumulative_count = count + get_routine_arg(coroutine_id, 1);

        set_routine_arg(coroutine_id, 1, cumulative_count);
        if (req_mask == 0) {
            set_resume(coroutine_id);
        }
    } else {
        set_routine_arg(coroutine_id, 0, -1);
        set_resume(coroutine_id);
    }
}
