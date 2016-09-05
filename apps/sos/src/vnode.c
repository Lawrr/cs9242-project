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

extern struct PCB tty_test_process;
extern int curr_coroutine_id;
extern fhandle_t mnt_point;

static int vnode_open(struct vnode *vnode, int mode);
static int vnode_close(struct vnode *vnode);
static int vnode_read(struct vnode *vnode, struct uio *uio);
static int vnode_write(struct vnode *vnode, struct uio *uio);
static int vnode_stat(struct vnode * vnode,sos_stat_t* stat);
static int vnode_getdirent(struct vnode* vnode,struct uio *uio);
static void dev_list_init();

static struct dev dev_list[MAX_DEV_NUM];
static struct hashtable *vnode_table;

static seL4_Word argument[NUM_ARG];

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

void vnode_readdir_cb(uintptr_t token, enum nfs_stat status,
                      int num_files, char* file_names[],
                      nfscookie_t nfscookie) {
    argument[0] = (seL4_Word) status; 
    argument[1] = nfscookie;
    argument[2] = 0; /* Found */

    struct uio *uio = get_routine_argument(0);
    int cumulative_pos = get_routine_argument(1);
    int curr_pos = 0;
    while (num_files > 0) {
        if (cumulative_pos == uio->offset) {
            break;
        }
        curr_pos++;
        cumulative_pos++;
        num_files--;
    }
    if (cumulative_pos == uio->offset) {
        /* Found the position - copy path into buffer if not null */
        if (file_names[curr_pos] != NULL) {
            strncpy(uio->addr, file_names[curr_pos], uio->size);
            int len = strnlen(file_names[curr_pos], uio->size);
            uio->remaining = uio->size - len;
        } else {
            *uio->addr = '\0';
            uio->remaining = uio->size;
        }
        argument[2] = 1; /* Found */
    } else if (file_names[curr_pos] == NULL) {
        /* Went over the end */
        argument[2] = -1;
    }
    argument[3] = cumulative_pos;

    set_resume(token);
}


static int vnode_getdirent(struct vnode* vnode, struct uio *uio) {
    int cumulative_pos = 0;
    seL4_Word cookies = 0;
    int found = 0;

    do {
        set_routine_argument(0, uio);
        set_routine_argument(1, cumulative_pos);
        nfs_readdir(&mnt_point, cookies, vnode_readdir_cb, curr_coroutine_id);  
        yield();
        int err = argument[0];
        cookies = argument[1];
        found = argument[2];
        cumulative_pos = argument[3];
    } while (!found && cookies != 0);

    /* Error over end */
    if (found == -1) {
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
        if (dev_list[i].name == NULL){
            return -1;
        }
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
        vnode = vnode_new(path);
        if (vnode == NULL) {
            return -1;
        }
        hashtable_insert(vnode_table, path, vnode);
    } else {
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
    argument[0] = (seL4_Word)status;
    argument[1] = (seL4_Word)malloc(sizeof(fhandle_t));
    argument[2] = (seL4_Word)malloc(sizeof(fattr_t));
    memcpy(argument[1],fh,sizeof(fhandle_t));
    memcpy(argument[2],fattr,sizeof(fattr_t));
    status == 0 ? -1:status;
    set_resume(token);    
}

static void vnode_stat_cb(uintptr_t token, nfs_stat_t status, fhandle_t *fh, fattr_t *fattr){
    argument[0] = (seL4_Word)status;
    argument[1] = (seL4_Word)malloc(sizeof(fattr_t));
    memcpy(argument[1],fattr,sizeof(fattr_t));
    status == 0 ? -1:status;
    set_resume(token);    
}

int vnode_stat(struct vnode *vnode, sos_stat_t* a){
    nfs_lookup (&mnt_point, vnode->path, (nfs_lookup_cb_t) vnode_stat_cb,curr_coroutine_id);
    yield();
    int err = (int) argument[0];
    int ret = 0;
    fattr_t *fattr = (fattr_t*) argument[1];

    if (err == NFS_OK) {
        a->st_type = fattr->type,
        a->st_fmode = fattr->mode,
        a->st_size = fattr->size,
        ((seL4_Word*)a)[3] = fattr->ctime.seconds * 1000 +
                             fattr->ctime.seconds / 1000,
        ((seL4_Word*)a)[4] = fattr->atime.seconds * 1000 +
                             fattr->atime.seconds / 1000;

    } else if (err == NFSERR_NOENT) {
        ret = -1;
    }

    if (fattr != NULL) free(fattr);
    return ret;
}

static void vnode_create_cb(uintptr_t token, nfs_stat_t status,fhandle_t *fh, fattr_t *fattr){
    argument[0] = (seL4_Word)status;
    argument[1] = (seL4_Word)malloc(sizeof(fhandle_t));
    argument[2] = (seL4_Word)malloc(sizeof(fattr_t));

    memcpy(argument[1],fh,sizeof(fhandle_t));
    memcpy(argument[2],fattr,sizeof(fattr_t));
    set_resume(token);  
}

static int vnode_open(struct vnode *vnode, fmode_t mode) {
    if (vnode->fh != NULL) return 0;

    nfs_lookup (&mnt_point, vnode->path,(nfs_lookup_cb_t)vnode_open_cb,curr_coroutine_id);
    yield();
    int err = (int)argument[0];
    if (err == NFS_OK){
        vnode->fh = (fhandle_t*)argument[1];
        vnode->fattr = (fattr_t*)argument[2];
    }  else if (err == NFSERR_NOENT){
        sattr_t sattr;
        sattr.mode = S_IROTH|S_IWOTH;
        sattr.uid = 1000;
        sattr.gid = 1000;
        sattr.size = 0;
        uint64_t timestamp = time_stamp();
        timeval_t time;
        time.seconds = timestamp/1000;
        time.useconds = (timestamp - time.seconds*1000)/10000000;
        sattr.atime = time;
        sattr.mtime = time;
        nfs_create( &mnt_point, vnode->path, &sattr, (nfs_create_cb_t)vnode_create_cb, curr_coroutine_id);
        yield();
        err = (int)argument[0];

        if (err == NFS_OK){
            vnode->fh = (fhandle_t*)argument[1];
            vnode->fattr = (fattr_t*)argument[2];
        }  else{
            conditional_panic(err,"faile create");
        }

    }  else{
        conditional_panic(err,"fail look up");
    } 
    return 0;
}

static int vnode_close(struct vnode *vnode) {
    /*Seems nothing to do*/ 
    return 0;
}

static void vnode_read_cb(uintptr_t token, nfs_stat_t status,fattr_t *fattr, int count, void* data){
    seL4_Word sos_vaddr = get_routine_argument(token,0); 
    if  (status == NFS_OK){
        memcpy((void*)sos_vaddr, data,count);
    }

    argument[0] = (seL4_Word)status;
    argument[1] = (seL4_Word)count;
    set_resume(token);
}

//doesn't work right now and I don't know why
static int vnode_read(struct vnode *vnode, struct uio *uio) {
    char *uaddr = (char *) uio->addr;
    seL4_Word ubuf_size = uio->size;
    seL4_Word end_uaddr = (seL4_Word) uaddr + ubuf_size;
    seL4_Word bytes_read = 0;
    while (ubuf_size > 0) {
        seL4_Word uaddr_next = PAGE_ALIGN_4K((seL4_Word) uaddr) + 0x1000;
        seL4_Word size;
        if (end_uaddr >= uaddr_next) {
            size = uaddr_next - (seL4_Word) uaddr;
        } else {
            size = ubuf_size;
        }

        /*   Though we can assume the buffer is mapped because it is a write operation,
             we still use sos_map_page to find the mapping address if it is already mapped */
        seL4_CPtr app_cap;
        seL4_CPtr sos_vaddr;
        int err = sos_map_page((seL4_Word) uaddr,
                tty_test_process.vroot,
                tty_test_process.addrspace,
                &sos_vaddr,
                &app_cap);
        if (err && err != ERR_ALREADY_MAPPED) {
            return 1;
        }
        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        //Add offset
        sos_vaddr |= ((seL4_Word) uaddr & PAGE_MASK_4K);

        set_routine_argument(0,sos_vaddr);
        err = nfs_read(vnode->fh, uio->offset, size,(nfs_read_cb_t)(vnode_read_cb),curr_coroutine_id); 
        conditional_panic(err,"faild read at send phrase");	
        yield();
        conditional_panic(argument[0],"fail read in end phrase");
        bytes_read += (seL4_Word)argument[1];
        uio->remaining -= (seL4_Word) argument[1];
        ubuf_size -= (seL4_Word) argument[1];
        uaddr += (seL4_Word) argument[1];
        uio->offset += (seL4_Word) argument[1];
        if (uio->size - uio->remaining == vnode->fattr->size) {
            return 0;
        }
    } 
    return 0;
}
static void vnode_write_cb(uintptr_t token, enum nfs_stat status,fattr_t *fattr, int count){
    seL4_Word sos_vaddr = get_routine_argument(token,0); 
    argument[0] = (seL4_Word)status;
    argument[1] = (seL4_Word)count;
    set_resume(token);
}

static int vnode_write(struct vnode *vnode, struct uio *uio) {
    char *uaddr = (char *) uio->addr;
    seL4_Word ubuf_size = uio->size;
    seL4_Word end_uaddr = (seL4_Word) uaddr + ubuf_size;
    seL4_Word bytes_sent = 0;
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
        seL4_CPtr app_cap;
        seL4_CPtr sos_vaddr;
        int err = sos_map_page((seL4_Word) uaddr,
                tty_test_process.vroot,
                tty_test_process.addrspace,
                &sos_vaddr,
                &app_cap);
        if (err && err != ERR_ALREADY_MAPPED) {
            return 1;
        }
        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        /*Add offset*/
        sos_vaddr |= ((seL4_Word) uaddr & PAGE_MASK_4K);
        set_routine_argument(0,sos_vaddr);

        err = nfs_write(vnode->fh, uio->offset, (int)size,sos_vaddr,&vnode_write_cb,curr_coroutine_id);
        conditional_panic(err,"fail write in send phrase");
        yield();
        conditional_panic(argument[0],"fail write in end phrase");
        bytes_sent += (seL4_Word)argument[1];
        uio->remaining -= (seL4_Word)argument[1];

        ubuf_size -= (seL4_Word)argument[1];
        uaddr += (seL4_Word)argument[1];
        uio->offset += (seL4_Word)argument[1];
    }
    return 0;
}
