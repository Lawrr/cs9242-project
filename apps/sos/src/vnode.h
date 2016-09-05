#ifndef _VNODE_H_
#define _VNODE_H_

#include <cspace/cspace.h>
#include <nfs/nfs.h>
#include <sos.h>

#define MAX_DEV_NUM 32
#define MAX_DEV_NAME 512
#define MAX_PATH_LEN 512 

#define ERR_DEV_NAME -1
#define ERR_MAX_DEV -2

struct vnode {
    char *path;
    int read_count;
    int write_count;
    void *data;
    fhandle_t *fh;
    fattr_t* fattr;    
    struct vnode_ops *ops;
};

struct uio {
    char *addr;
    int size;
    int remaining;
    int offset;
};

struct vnode_ops {
    int (*vop_open)(struct vnode *vnode, fmode_t mode);
    int (*vop_close)(struct vnode *vnode);
    int (*vop_read)(struct vnode *vnode, struct uio *uio);
    int (*vop_write)(struct vnode *vnode, struct uio *uio);
    int (*vop_stat)(struct vnode *vnode, sos_stat_t* stat);
    int (*vop_getent)(const char * path);
};

struct dev {
    char* name;
    struct vnode_ops *ops;
};

int vfs_init();

int vfs_open(char *path, int mode, struct vnode **ret_vnode);

int vfs_close(struct vnode *vnode, int mode);

int dev_add(char *dev_name, struct vnode_ops *dev_ops);

#endif
