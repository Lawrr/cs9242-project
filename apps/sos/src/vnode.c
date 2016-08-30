static const struct vnode_ops default_vops ={
    &vnode_open,
    &vnode_close,    
    &vnode_read,
    &vnode_write
};

struct dev dev_list[MAX_DEV_NUM];

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
            strcpy(dev_list[i].dev_name,dev_name);
            dev_list[i].dev_ops = dev_ops;
            return 0;
        }
    }
    return ERR_MAX_DEV;
}

int strnlen(char *str, int n) {
    for (int i = 0; i < n; i++) {
        if (str[i] == '\0') {
            return i;
        }
    }
    return n;
}

/*Assume the name has '\0'*/
static int isDev(char *dev){
    for (int i = 0; i < MAX_DEV_NUM;i++){
        if (stncmp(dev_list[i].dev_name,dev,MAX_DEV_NAME)){
            return i;
        }
    }
    return -1;
}

int vfs_open(char*path,struct vnode **ret_vn){
    *ret_vn = vnode_new();
    (*ret_vn) -> vnode_open(path);
    (*ret_vn)->vn_ref;
    return 0;
}

/*The buf should be in the same address region*/
static int vnode_open(char *path){
    int dev_id = isDev(path);
    char safePath[MAX_PATH_LEN+1];
    strncpy(safePath,path,MAX_PATH_LEN);
    if (dev_id != -1) {
        vn->vnode_ops = dev_list[dev_id].dev_ops;
        //handle console here
    } else {
        //Not a dev, try doing with file
    }
    return 0;
}

static struct vnode *vnode_new() {
    struct vnode *vn = malloc(sizeof(struct vnode));
    vn->vn_ref = 0;
    vn->vn_ops = &default_vops;
}

/*The buf should be in the same address region*/
int vnode_close(struct vnode vn){


}

/*The buf should be in the same address region*/
int vnode_read(struct vnode *vn,struct uio* uio){
    
}


/*The buf should be in the same address region*/
int vnode_write(struct vnode *vn,struct uio* uio){

}


