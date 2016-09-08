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

extern struct PCB *curproc;
extern int curr_coroutine_id;
extern fhandle_t mnt_point;

static int vnode_open(struct vnode *vnode, int mode);
static int vnode_close(struct vnode *vnode);
static int vnode_read(struct vnode *vnode, struct uio *uio);
static int vnode_write(struct vnode *vnode, struct uio *uio);
static int vnode_stat(struct vnode *vnode, sos_stat_t *stat);
static int vnode_getdirent(struct vnode *vnode, struct uio *uio);

static void dev_list_init();

static struct dev dev_list[MAX_DEV_NUM];
static struct hashtable *vnode_table;

static seL4_Word arg[NUM_ARG];

int vfs_init() {
    vnode_table = hashtable_new(VNODE_TABLE_SLOTS);
    if (vnode_table == NULL) {
        return 1;
    }

    dev_list_init();

    return 0;
}

static const struct vnode_ops default_ops = {
    &vnode_open,
    &vnode_close,
    &vnode_read,
    &vnode_write,
    &vnode_stat,
    &vnode_getdirent
};

static void vnode_readdir_cb(uintptr_t token, enum nfs_stat status,
                             int num_files, char *file_names[],
                             nfscookie_t nfscookie) {
    arg[0] = (seL4_Word) status;
    arg[1] = nfscookie;

    struct uio *uio = get_routine_arg((int) token, 0);
    if (uio->offset == 0 && num_files == 0) {
        uio->addr = "\0";
        uio->offset = 0;
    } else if (uio->offset < num_files) {
        /* Check if we need to map a second page */
        int len = strlen(file_names[uio->offset]);
        if (len > uio->size) {
            arg[1] = 0; /*hard set to stop it */
        }

        seL4_Word uaddr = uio->addr;
        seL4_Word uaddr_end = uio->addr + len;

        seL4_Word sos_vaddr;
        int err = sos_map_page(uaddr, &sos_vaddr);
        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        sos_vaddr |= (uaddr & PAGE_MASK_4K);
        if (PAGE_ALIGN_4K(uaddr) != PAGE_ALIGN_4K(uaddr_end)) {
            seL4_Word uaddr_next = PAGE_ALIGN_4K(uaddr) + 0x1000;

            seL4_Word sos_vaddr_next;
            err = sos_map_page(uaddr_next, &sos_vaddr_next);

            sos_vaddr_next = PAGE_ALIGN_4K(sos_vaddr_next);

            /* Boundary write */
            memcpy(sos_vaddr, file_names[uio->offset], uaddr_next - uaddr);
            /* Write rest of next page */
            strcpy(sos_vaddr_next, ((char *) file_names[uio->offset]) + uaddr_next - uaddr);
        } else {
            /* All on same page */
            strncpy(sos_vaddr, file_names[uio->offset], uio->size);
        }

        uio->remaining = uio->size - len;
        uio->offset = 0;

    } else {
        uio->offset -= num_files;
    }

    set_resume(token);
}


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

static struct vnode *vnode_new(char *path) {
    struct vnode *vnode = malloc(sizeof(struct vnode));
    if (vnode == NULL) {
        return NULL;
    }

    int dev_id = is_dev(path);

    /* Initialise variables */
    vnode->path = malloc(strlen(path + 1));
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

        /* Set device's ops */
        vnode->ops = dev_list[dev_id].ops;
    } else {
        /* Handle file */
        vnode->ops = &default_ops;
    }

    return vnode;
}

int vfs_get(char *path, struct vnode **ret_vnode) {
    struct vnode *vnode;

    /* Check if vnode for path already exists */
    struct hashtable_entry *entry = hashtable_get(vnode_table, path);
    if (entry == NULL) {
        /* New vnode */
        vnode = vnode_new(path);
        if (vnode == NULL) {
            return -1;
        }

        hashtable_insert(vnode_table, path, vnode);

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
    if (err) {
        return err;
    }

    /* Open the vnode */
    err = vnode->ops->vop_open(vnode, mode);
    /* Check for errors, include single read */
    if (err) {
        return err;
    }

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
        hashtable_remove(vnode_table, vnode->path);

        if (vnode->fh != NULL) free(vnode->fh);
        if (vnode->fattr != NULL) free(vnode->fattr);

        free(vnode->path);
        free(vnode);
    }

    return 0;
}

static void vnode_open_cb(uintptr_t token,
                          nfs_stat_t status,
                          fhandle_t *fh,
                          fattr_t *fattr) {
    arg[0] = (seL4_Word) status;
    arg[1] = (seL4_Word) malloc(sizeof(fhandle_t));
    arg[2] = (seL4_Word) malloc(sizeof(fattr_t));

    memcpy(arg[1], fh, sizeof(fhandle_t));
    memcpy(arg[2], fattr, sizeof(fattr_t));

    /* 0 has special meaning for setjmp returns, so map 0 to -1 */
    if (status == 0) status = -1;

    set_resume(token);
}

static void vnode_stat_cb(uintptr_t token, nfs_stat_t status, fhandle_t *fh, fattr_t *fattr) {
    arg[0] = (seL4_Word) status;
    arg[1] = (seL4_Word) malloc(sizeof(fattr_t));

    memcpy(arg[1], fattr, sizeof(fattr_t));

    /* 0 has special meaning for setjmp returns, so map 0 to -1 */
    if (status == 0) status = -1;

    set_resume(token);
}

int vnode_stat(struct vnode *vnode, sos_stat_t *stat) {
    nfs_lookup(&mnt_point, vnode->path, (nfs_lookup_cb_t) vnode_stat_cb, curr_coroutine_id);
    yield();

    int err = (int) arg[0];
    fattr_t *fattr = (fattr_t*) arg[1];

    int ret = 0;

    if (err == NFS_OK) {
        seL4_Word sos_vaddr;
        seL4_Word uaddr = (seL4_Word) stat;
        int err = sos_map_page(uaddr, &sos_vaddr);

        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        sos_vaddr |= (uaddr & PAGE_MASK_4K);

        seL4_Word *buffer;

        if (PAGE_ALIGN_4K(uaddr + sizeof(stat)) != PAGE_ALIGN_4K(uaddr)) {
            buffer = malloc(sizeof(sos_stat_t));
        } else {
            buffer = (seL4_Word*) sos_vaddr;
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

static void vnode_create_cb(uintptr_t token, nfs_stat_t status, fhandle_t *fh, fattr_t *fattr) {
    arg[0] = (seL4_Word) status;
    arg[1] = (seL4_Word) malloc(sizeof(fhandle_t));
    arg[2] = (seL4_Word) malloc(sizeof(fattr_t));

    memcpy(arg[1], fh, sizeof(fhandle_t));
    memcpy(arg[2], fattr, sizeof(fattr_t));

    set_resume(token);
}

static int vnode_open(struct vnode *vnode, fmode_t mode) {
    if (vnode->fh != NULL) return 0;

    nfs_lookup(&mnt_point, vnode->path, (nfs_lookup_cb_t) vnode_open_cb, curr_coroutine_id);
    yield();

    int err = (int) arg[0];

    if (err == NFS_OK) {
        vnode->fh = (fhandle_t*) arg[1];
        vnode->fattr = (fattr_t*) arg[2];

    } else if (err == NFSERR_NOENT) {
        /* Create new file */
        sattr_t sattr;
        sattr.mode = S_IROTH|S_IWOTH;
        sattr.uid = 1000;
        sattr.gid = 1000;
        sattr.size = 0;
        uint64_t timestamp = time_stamp();
        timeval_t time;
        time.seconds = timestamp / 1000;
        time.useconds = (timestamp - time.seconds * 1000) / 10000000;
        sattr.atime = time;
        sattr.mtime = time;
        nfs_create(&mnt_point, vnode->path, &sattr, (nfs_create_cb_t) vnode_create_cb, curr_coroutine_id);
        yield();

        err = (int) arg[0];

        if (err == NFS_OK) {
            vnode->fh = (fhandle_t*)arg[1];
            vnode->fattr = (fattr_t*)arg[2];
        } else {
            conditional_panic(err, "failed create");
        }

    } else {
        conditional_panic(err, "fail look up");
    }

    return 0;
}

static int vnode_close(struct vnode *vnode) {
    return 0;
}

static void vnode_read_cb(uintptr_t token, nfs_stat_t status, fattr_t *fattr, int count, void *data) {
    seL4_Word sos_vaddr = get_routine_arg(token, 0);
    if (status == NFS_OK) {
        memcpy((void *) sos_vaddr, data, count);
    }

    arg[0] = (seL4_Word)status;
    arg[1] = (seL4_Word)count;

    set_resume(token);
}

static int vnode_read(struct vnode *vnode, struct uio *uio) {
    char *uaddr = (char *) uio->addr;
    seL4_Word ubuf_size = uio->size;
    seL4_Word end_uaddr = (seL4_Word) uaddr + ubuf_size;
    seL4_Word bytes_read = 0;
    int err = 0;

    while (ubuf_size > 0) {
        seL4_Word uaddr_next = PAGE_ALIGN_4K((seL4_Word) uaddr) + 0x1000;
        seL4_Word size;
        if (end_uaddr >= uaddr_next) {
            size = uaddr_next - (seL4_Word) uaddr;
        } else {
            size = ubuf_size;
        }

        err = nfs_read(vnode->fh, uio->offset, size, (nfs_read_cb_t) vnode_read_cb, curr_coroutine_id);
        conditional_panic(err, "failed read at send phrase");

        seL4_CPtr sos_vaddr = get_sos_vaddr(uaddr, curproc->addrspace);
        if (!sos_vaddr) {
            err = sos_map_page((seL4_Word) uaddr, &sos_vaddr);
            if (err) {
                return 1;
            }
        }

        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        sos_vaddr |= ((seL4_Word) uaddr & PAGE_MASK_4K);
        set_routine_arg(curr_coroutine_id, 0, sos_vaddr);
        yield();

        conditional_panic(arg[0], "failed read in end phrase");

        bytes_read += (seL4_Word)arg[1];
        uio->remaining -= (seL4_Word) arg[1];
        ubuf_size -= (seL4_Word) arg[1];
        uaddr += (seL4_Word) arg[1];
        uio->offset += (seL4_Word) arg[1];

        if (uio->offset >= vnode->fattr->size) {
            return 0;
        }
    }
    return 0;
}

static void vnode_write_cb(uintptr_t token, enum nfs_stat status, fattr_t *fattr, int count) {
    seL4_Word * al = (seL4_Word*) token;
    seL4_Word sos_vaddr = get_routine_arg(al[0], 0);
    conditional_panic(status, "nfs_write fail in end phase");

    seL4_Word req_mask = get_routine_arg(al[0], 0) & (~(1 << al[1]));
    set_routine_arg(al[0], 0, req_mask);
    seL4_Word cumulative_count = count+get_routine_arg(al[0], 1);

    set_routine_arg(al[0], 1, cumulative_count);
    if ( req_mask == 0) {
        set_resume(al[0]);
    }
    free(al);
}

static int vnode_write(struct vnode *vnode, struct uio *uio) {
    char *uaddr = (char *) uio->addr;
    seL4_Word ubuf_size = uio->size;
    seL4_Word end_uaddr = (seL4_Word) uaddr + ubuf_size;
    seL4_Word bytes_sent = 0;
    int err = 0;

    seL4_CPtr sos_vaddr = get_sos_vaddr(uaddr, curproc->addrspace);
    if (!sos_vaddr) {
        err = sos_map_page((seL4_Word) uaddr, &sos_vaddr);
        if (err && err != ERR_ALREADY_MAPPED) {
            return 1;
        }
    }

    while (ubuf_size > 0) {
        seL4_Word uaddr_next = PAGE_ALIGN_4K((seL4_Word) uaddr) + 0x1000;
        seL4_Word size;
        if (end_uaddr >= uaddr_next) {
            size = uaddr_next - (seL4_Word) uaddr;
        } else {
            size = ubuf_size;
        }

        /* Though we can assume the buffer is mapped because it is a write operation,
           we still use sos_map_page to find the mapping address if it is already mapped */
        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        /*Add offset*/
        sos_vaddr |= ((seL4_Word) uaddr & PAGE_MASK_4K);

        int req_id = 0;
        while (size > 0) {
            seL4_Word *token = malloc(sizeof(seL4_Word) * 2);
            token[0] = curr_coroutine_id;
            token[1] = req_id;
            if (size > 1024) {
                err = nfs_write(vnode->fh, uio->offset + 1024 * req_id, 1024, sos_vaddr, &vnode_write_cb, token);
                req_id++;
                size -= 1024;
                conditional_panic(err, "fail write in send phrase");
            } else {
                err = nfs_write(vnode->fh, uio->offset + 1024 * req_id, size, sos_vaddr, &vnode_write_cb, token);
                size = 0;

                set_routine_arg(curr_coroutine_id, 0, (1<<(req_id+1))-1);
                set_routine_arg(curr_coroutine_id, 1, 0);

                conditional_panic(err, "fail write in send phrase");
            }
        }

        sos_vaddr = get_sos_vaddr(uaddr_next, curproc->addrspace);
        if (!sos_vaddr && uaddr_next < end_uaddr) {
            err = sos_map_page((seL4_Word) uaddr_next, &sos_vaddr);
            if (err && err != ERR_ALREADY_MAPPED) {
                return 1;
            }
        }

        yield();

        seL4_Word count = get_routine_arg(curr_coroutine_id, 1);
        bytes_sent += count;
        uio->remaining -= count;

        ubuf_size -= count;
        uaddr += count;
        uio->offset += count;
    }

    return 0;
}
