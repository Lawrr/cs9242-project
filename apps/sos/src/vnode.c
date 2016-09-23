#include <string.h>
#include "vnode.h"
#include "hashtable.h"
#include "process.h"
#include "coroutine.h"
#include <string.h>
#include <sys/panic.h>
#include <sys/stat.h>
#include <mapping.h>
#include <utils/page.h>
#include <nfs/nfs.h>
#include <clock/clock.h>

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

static seL4_Word arg[NUM_ARG];

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
        return ERR_DEV_NAME;
    }

    /* Find empty device slot */
    for (int i = 0; i < MAX_DEV_NUM; i++) {
        if (dev_list[i].name == NULL) {
            /* Add new device */
            dev_list[i].name = malloc(len + 1);
            strcpy(dev_list[i].name, name);
            dev_list[i].ops = ops;
            return 0;
        }
    }

    /* No free device slot */
    return ERR_MAX_DEV;
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
        conditional_panic(err, "Could not insert into hashtable\n");
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
    /* Dec ref counts */
    if ((mode & FM_READ) != 0) {
        vnode->read_count--;
    }
    if ((mode & FM_WRITE) != 0) {
        vnode->write_count--;
    }

    vnode->ops->vop_close(vnode);

    if (vnode->read_count + vnode->write_count == 0) {
        /* No more references left - Remove vnode */
        int err = hashtable_remove(vnode_table, vnode->path);
        conditional_panic(err, "Could not remove from hashtable\n");

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
    set_routine_arg(curr_coroutine_id, 0, uio);

    do {
        set_routine_arg(curr_coroutine_id, 0, uio);
        nfs_readdir(&mnt_point, cookies, vnode_readdir_cb, curr_coroutine_id);
        yield();
        int err = arg[0];
        cookies = arg[1];
    } while (uio->offset > 0 && cookies != 0);

    /* Error - reached over the end */
    if (uio->offset > 0 && cookies == 0) {
        return 1;
    }

    return 0;
}

static void vnode_readdir_cb(uintptr_t token, enum nfs_stat status,
        int num_files, char *file_names[],
        nfscookie_t nfscookie) {
    arg[0] = (seL4_Word) status;
    arg[1] = nfscookie;

    struct uio *uio = get_routine_arg((int) token, 0);

    if (uio->offset == 0 && num_files == 0) {
        /* Valid next pos NULL */
        uio->uaddr = "\0";

    } else if (uio->offset < num_files) {
        /* Check if we need to map a second page */
        int len = strlen(file_names[uio->offset]) + 1;
        if (len > uio->size) {
            arg[1] = 0; /* Hard set to stop it continuing */
        }

        seL4_Word uaddr = uio->uaddr;
        seL4_Word uaddr_end = uio->uaddr + len;

        seL4_Word sos_vaddr;
        int err = sos_map_page(uaddr, &sos_vaddr);
        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        sos_vaddr |= (uaddr & PAGE_MASK_4K);
        if (PAGE_ALIGN_4K(uaddr) != PAGE_ALIGN_4K(uaddr_end)) {
            seL4_Word uaddr_next = PAGE_ALIGN_4K(uaddr) + PAGE_SIZE_4K;

            seL4_Word sos_vaddr_next;
            err = sos_map_page(uaddr_next, &sos_vaddr_next);

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

    set_resume(token);
}


/*
 * =======================================================
 * STAT
 * =======================================================
 */
static int vnode_stat(struct vnode *vnode, sos_stat_t *stat) {
    nfs_lookup(&mnt_point, vnode->path, (nfs_lookup_cb_t) vnode_stat_cb, curr_coroutine_id);
    yield();

    int err = (int) arg[0];
    fattr_t *fattr = (fattr_t *) arg[1];

    int ret = 0;

    if (err == NFS_OK) {
        seL4_Word sos_vaddr;
        seL4_Word uaddr = (seL4_Word) stat;
        int err = sos_map_page(uaddr, &sos_vaddr);
        /* Add offset */
        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        sos_vaddr |= (uaddr & PAGE_MASK_4K);

        seL4_Word *buffer;

        if (PAGE_ALIGN_4K(uaddr + sizeof(stat)) != PAGE_ALIGN_4K(uaddr)) {
            buffer = malloc(sizeof(sos_stat_t));
        } else {
            buffer = (seL4_Word *) sos_vaddr;
        }

        buffer[0] = fattr->type;
        buffer[1] = fattr->mode;
        buffer[2] = fattr->size;
        buffer[3] = fattr->ctime.seconds * 1000 + fattr->ctime.seconds / 1000;
        buffer[4] = fattr->atime.seconds * 1000 + fattr->atime.seconds / 1000;

        if (PAGE_ALIGN_4K(uaddr + sizeof(stat)) != PAGE_ALIGN_4K(uaddr)) {
            seL4_Word sos_vaddr_next;
            int err = sos_map_page(PAGE_ALIGN_4K(uaddr + sizeof(stat)), &sos_vaddr_next);

            sos_vaddr_next = PAGE_ALIGN_4K(sos_vaddr);

            seL4_Word first_half = (PAGE_ALIGN_4K(uaddr + sizeof(stat)) - (seL4_Word) stat);

            /* Boundary */
            memcpy(sos_vaddr, buffer, first_half);
            memcpy(sos_vaddr_next, buffer + first_half, sizeof(stat) - first_half);

            free(buffer);
        }

    } else if (err == NFSERR_NOENT) {
        ret = -1;
    }

    if (fattr != NULL) free(fattr);

    return ret;
}

static void vnode_stat_cb(uintptr_t token, nfs_stat_t status, fhandle_t *fh, fattr_t *fattr) {
    arg[0] = (seL4_Word) status;
    arg[1] = (seL4_Word) malloc(sizeof(fattr_t));

    memcpy(arg[1], fattr, sizeof(fattr_t));

    /* 0 has special meaning for setjmp returns, so map 0 to -1 */
    if (status == 0) status = -1;

    set_resume(token);
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

    nfs_create(&mnt_point, vnode->path, &sattr, (nfs_create_cb_t) vnode_create_cb, curr_coroutine_id);

    set_routine_arg(curr_coroutine_id, 0, 1);

    yield();
}

static void vnode_create_cb(uintptr_t token, nfs_stat_t status, fhandle_t *fh, fattr_t *fattr) {
    arg[0] = (seL4_Word) status;
    arg[1] = (seL4_Word) malloc(sizeof(fhandle_t));
    arg[2] = (seL4_Word) malloc(sizeof(fattr_t));

    memcpy(arg[1], fh, sizeof(fhandle_t));
    memcpy(arg[2], fattr, sizeof(fattr_t));

    set_routine_arg(curr_coroutine_id, 0, 0);

    set_resume(token);
}


/*
 * =======================================================
 * OPEN
 * =======================================================
 */
static int vnode_open(struct vnode *vnode, fmode_t mode) {
    if (vnode->fh != NULL) return 0;

    nfs_lookup(&mnt_point, vnode->path, (nfs_lookup_cb_t) vnode_open_cb, curr_coroutine_id);
    set_routine_arg(curr_coroutine_id, 0, 1);
    yield();
    int err = (int) arg[0];

    if (err == NFS_OK) {
        vnode->fh = (fhandle_t *) arg[1];
        vnode->fattr = (fattr_t *) arg[2];

    } else if (err == NFSERR_NOENT) {
        /* Create new file */
        file_create(vnode);

        err = (int) arg[0];

        if (err == NFS_OK) {
            vnode->fh = (fhandle_t *) arg[1];
            vnode->fattr = (fattr_t *) arg[2];
        } else {
            conditional_panic(err, "failed create");
        }

    } else {
        conditional_panic(err, "fail look up");
    }

    return 0;
}

static void vnode_open_cb(uintptr_t token, nfs_stat_t status, fhandle_t *fh, fattr_t *fattr) {
    arg[0] = (seL4_Word) status;
    arg[1] = (seL4_Word) malloc(sizeof(fhandle_t));
    arg[2] = (seL4_Word) malloc(sizeof(fattr_t));

    memcpy(arg[1], fh, sizeof(fhandle_t));
    memcpy(arg[2], fattr, sizeof(fattr_t));
    /* 0 has special meaning for setjmp returns, so map 0 to -1 */
    if (status == 0) status = -1;

    set_routine_arg(curr_coroutine_id, 0, 0);

    set_resume(token);
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
            err = sos_map_page((seL4_Word) uaddr, &sos_vaddr);
            if (err && err != ERR_ALREADY_MAPPED) {
                return 1;
            }

            sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
            sos_vaddr |= ((seL4_Word) uaddr & PAGE_MASK_4K);
        } else {
            /* vaddr */
            sos_vaddr = uio->vaddr;
        }

        err = nfs_read(vnode->fh, uio->offset, size, (nfs_read_cb_t) vnode_read_cb, curr_coroutine_id);
        conditional_panic(err, "failed read at send phrase");

        set_routine_arg(curr_coroutine_id, 0, 1);
        set_routine_arg(curr_coroutine_id, 1, sos_vaddr);

        yield();

        seL4_Word count = arg[0];

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

static void vnode_read_cb(uintptr_t token, nfs_stat_t status, fattr_t *fattr, int count, void *data) {
    conditional_panic(status, "failed read in end phrase");

    seL4_Word sos_vaddr = get_routine_arg(token, 1);

    if (status == NFS_OK) {
        memcpy((void *) sos_vaddr, data, count);
    }

    arg[0] = (seL4_Word)count;

    set_routine_arg(curr_coroutine_id, 0, 0);

    set_resume(token);
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
        err = sos_map_page((seL4_Word) uaddr, &sos_vaddr);
        if (err && err != ERR_ALREADY_MAPPED) {
            return 1;
        }
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
                err = sos_map_page((seL4_Word) uaddr_next, &sos_vaddr_next);
                if (err && err != ERR_ALREADY_MAPPED) {
                    return 1;
                }
            }
        } else {
            /* vaddr */
            size = buf_size;
        }

        int req_id = 0;
        while (size > 0) {
            seL4_Word *token = malloc(sizeof(seL4_Word) * 2);
            token[0] = curr_coroutine_id;
            token[1] = req_id;

            int offset = MAX_WRITE_SIZE * req_id;

            if (size > MAX_WRITE_SIZE) {
                err = nfs_write(vnode->fh,
                                uio->offset + offset,
                                MAX_WRITE_SIZE,
                                sos_vaddr + offset,
                                &vnode_write_cb,
                                token);
                conditional_panic(err, "fail write in send phrase");

                size -= MAX_WRITE_SIZE;
                req_id++;
            } else {
                err = nfs_write(vnode->fh,
                                uio->offset + offset,
                                size,
                                sos_vaddr + offset,
                                &vnode_write_cb,
                                token);
                conditional_panic(err, "fail write in send phrase");

                size = 0;

                set_routine_arg(curr_coroutine_id, 0, (1 << (req_id + 1)) - 1);
                set_routine_arg(curr_coroutine_id, 1, 0);
            }
        }

        if (uio->uaddr != NULL) {
            sos_vaddr = sos_vaddr_next;
        }

        yield();

        seL4_Word count = get_routine_arg(curr_coroutine_id, 1);

        buf_size -= count;
        if (uio->uaddr != NULL) uaddr += count;

        uio->remaining -= count;
        uio->offset += count;
    }

    return 0;
}

static void vnode_write_cb(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count) {
    conditional_panic(status, "nfs_write fail in end phase");

    seL4_Word *al = (seL4_Word *) token;

    seL4_Word req_mask = get_routine_arg(al[0], 0) & (~(1 << al[1]));
    set_routine_arg(al[0], 0, req_mask);
    seL4_Word cumulative_count = count + get_routine_arg(al[0], 1);

    set_routine_arg(al[0], 1, cumulative_count);
    if (req_mask == 0) {
        set_resume(al[0]);
    }

    free(al);
}
