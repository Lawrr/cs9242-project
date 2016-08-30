#include <string.h>
#include <vnode.h>

static int vnode_open(struct vnode *vn, char *path);
static int vnode_close(struct vnode vn);
static int vnode_read(struct vnode *vn,struct uio* uio);
static int vnode_write(struct vnode *vn,struct uio* uio);

static const struct vnode_ops default_vops ={
    &vnode_open,
    &vnode_close,    
    &vnode_read,
    &vnode_write
};

static struct dev dev_list[MAX_DEV_NUM];

void dev_list_init(){
    for (int i = 0 ; i < MAX_DEV_NUM; i++){
        dev_list[i].dev_name = NULL;
        dev_list[i].dev_ops = NULL;
    }
}

int  dev_add(char *dev_name,struct vnode_ops* dev_ops){
    int len = strnlen(dev_name,MAX_DEV_NAME);
    if (len == MAX_DEV_NAME){
        return ERR_DEV_NAME;          
    }
    for (int i = 0; i < MAX_DEV_NUM; i++){
        if (dev_list[i].dev_name == NULL){
            dev_list[i].dev_name = malloc(len+1);
            strcpy(dev_list[i].dev_name, dev_name);
            printf("Device name: '%s'\n", dev_name);
            printf("Device list: '%s'\n", dev_list[i].dev_name);
            dev_list[i].dev_ops = dev_ops;
            return 0;
        }
    }
    return ERR_MAX_DEV;
}

static int isDev(char *dev){
    for (int i = 0; i < MAX_DEV_NUM;i++){
        if (!strncmp(dev_list[i].dev_name, dev,MAX_DEV_NAME)) {
            return i;
        }
    }
    return -1;
}

static struct vnode *vnode_new() {
    struct vnode *vn = malloc(sizeof(struct vnode));
    vn->vn_ref = 0;
    vn->vn_ops = &default_vops;
    return vn;
}

int vfs_open(char*path, struct vnode **ret_vn){
    *ret_vn = vnode_new();
    vnode_open(*ret_vn, path);
    (*ret_vn)->vn_ops->vop_open(*ret_vn, path);
    (*ret_vn)->vn_ref++;
    return 0;
}

static int vnode_open(struct vnode *vn, char *path) {
    int dev_id = isDev(path);
    char safePath[MAX_PATH_LEN+1];
    strncpy(safePath,path,MAX_PATH_LEN);
    if (dev_id != -1) {
        vn->vn_ops = dev_list[dev_id].dev_ops;
        /* Handle console here */
    } else {
        /* Not a dev, try doing with file */
    }
    return 0;
}

/*The buf should be in the same address region*/
static int vnode_close(struct vnode vn){
    return 0;
}

/*The buf should be in the same address region*/
static int vnode_read(struct vnode *vn,struct uio* uio){
    return 0;
}


/*The buf should be in the same address region*/
static int vnode_write(struct vnode *vn,struct uio* uio){
    return 0;
}


