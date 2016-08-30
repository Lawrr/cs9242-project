#ifndef __VNODE_H
#define __VNODE_H

#include <cspace/cspace.h>

#define MAX_DEV_NUM 32
#define MAX_DEV_NAME 255//Not inculde terminator
#define MAX_PATH_LEN 512 
#define ERR_DEV_NAME -1
#define ERR_MAX_DEV -2

struct vnode{
    int vn_ref;
    //struct fs *vn_fs;
    void *vn_data;
    struct vnode_ops *vn_ops;
};

struct uio{
    seL4_Word bufAddr;
    seL4_Word bufSize;
    seL4_Word remaining;
    seL4_Word fileOffset;
};

struct vnode_ops{
    int (*vop_open)(struct vnode *vnode, char *path);
    int (*vop_close)(struct vnode *vnode);
    int (*vop_read)(struct vnode *vnode, struct uio *uio);
    int (*vop_write)(struct vnode *vnode, struct uio *uio);
};

struct dev{
    char* dev_name;
    struct vnode_ops *dev_ops;
};

void dev_list_init();

int dev_add(char *dev_name, struct vnode_ops *dev_ops);

int vfs_open(char*path,struct vnode **ret_vn);

#endif
