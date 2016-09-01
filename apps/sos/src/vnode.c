#include <string.h>
#include "sos.h"
#include "vnode.h"
#include "hashtable.h"

#define VNODE_TABLE_SLOTS 64

static int vnode_open(struct vnode *vnode, char *path);
static int vnode_close(struct vnode *vnode);
static int vnode_read(struct vnode *vnode, struct uio *uio);
static int vnode_write(struct vnode *vnode, struct uio *uio);
static void dev_list_init();

static struct dev dev_list[MAX_DEV_NUM];
static struct hashtable *vnode_table;

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
    &vnode_write
};

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

    vnode->path = malloc(strlen(path + 1));
    strcpy(vnode->path, path);
    vnode->read_count = 0;
    vnode->write_count = 0;

    int dev_id = is_dev(path);
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

int vfs_open(char *path, int mode, struct vnode **ret_vnode) {
    struct vnode *vnode;
    /* Check if vnode for path already exists */
    struct hashtable_entry *entry = hashtable_get(vnode_table, path);
    if (entry == NULL) {
        vnode = vnode_new(path);
        hashtable_insert(vnode_table, path, vnode);
    } else {
        vnode = (struct vnode *) entry->value;
    }

    /* Open the vnode */
    int err = vnode->ops->vop_open(vnode, mode);
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
        hashtable_remove(vnode_table, vnode->path);
        free(vnode->path);
        free(vnode);
    }

    return 0;
}

static int vnode_open(struct vnode *vnode, char *path) {
    return 0;
}

static int vnode_close(struct vnode *vnode) {
    return 0;
}

static int vnode_read(struct vnode *vnode, struct uio *uio) {
    return 0;
}

static int vnode_write(struct vnode *vnode, struct uio *uio) {
    return 0;
}
