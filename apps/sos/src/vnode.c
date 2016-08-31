#include <string.h>
#include <vnode.h>

static int vnode_open(struct vnode *vnode, char *path);
static int vnode_close(struct vnode *vnode);
static int vnode_read(struct vnode *vnode, struct uio *uio);
static int vnode_write(struct vnode *vnode, struct uio *uio);

static const struct vnode_ops default_ops = {
    &vnode_open,
    &vnode_close,
    &vnode_read,
    &vnode_write
};

static struct dev dev_list[MAX_DEV_NUM];

void dev_list_init() {
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

static struct vnode *vnode_new() {
    struct vnode *vnode = malloc(sizeof(struct vnode));
    vnode->ref_count = 0;
    vnode->ops = &default_ops;

    return vnode;
}

int vfs_open(char *path, struct vnode **ret_vnode) {
    *ret_vnode = vnode_new();

    vnode_open(*ret_vnode, path);

    (*ret_vnode)->ops->vop_open(*ret_vnode, path);

    (*ret_vnode)->ref_count++;

    return 0;
}

static int vnode_open(struct vnode *vnode, char *path) {
    int dev_id = is_dev(path);

    if (dev_id != -1) {
        /* Handle device */

        /* Set device's ops */
        vnode->ops = dev_list[dev_id].ops;
    } else {
        /* Handle file */
    }

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
