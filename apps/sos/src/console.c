#include <cspace/cspace.h>

#include <serial/serial.h>
#include <utils/page.h>
#include <fcntl.h>

#include "mapping.h"
#include "sos.h"
#include "vnode.h"
#include "console.h"
#include "coroutine.h"
#include "process.h"
#include <sys/debug.h>
#include <sys/panic.h>

extern struct PCB *curproc;
extern int curr_coroutine_id;

static struct serial *serial_handle;
static struct vnode *console_vnode;
static struct uio *console_uio;
static pid_t read_pid;
static unsigned int read_stime;
static int read_coroutine_id;

static void console_serial_handler(struct serial *serial, char c);
static int console_write(struct vnode *vnode, struct uio *uio);
static int console_read(struct vnode *vnode, struct uio *uio);
static int console_open(struct vnode *vnode, int mode);
static int console_close(struct vnode *vnode);

int console_init(struct vnode **ret_vnode) {
    /* Initialise serial driver */
    serial_handle = serial_init();
    serial_register_handler(serial_handle, console_serial_handler);

    /* Set up console dev */
    struct vnode_ops *console_ops = malloc(sizeof(struct vnode_ops));
    if (console_ops == NULL) return -1;

    console_ops->vop_open = &console_open;
    console_ops->vop_close = &console_close;
    console_ops->vop_read = &console_read;
    console_ops->vop_write = &console_write;
    console_ops->vop_stat = NULL;
    console_ops->vop_getdirent = NULL;

    int err = dev_add("console", console_ops);
    if (err) {
        free(console_ops);
        return -1;
    }

    /* Set return console vnode */
    /* FM_WRITE for STDOUT */
    err = vfs_open("console", FM_WRITE, ret_vnode);
    if (err) {
        dev_remove("console");
        free(console_ops);
        return -1;
    }

    return 0;
}

static void console_serial_handler(struct serial *serial, char c) {
    /* Return if we do not currently need to read */
    if (console_uio == NULL || console_uio->remaining == 0) return;

    /* Check if proc was deleted */
    struct PCB *pcb = process_status(read_pid);
    if (pcb == NULL || pcb->stime != read_stime) return;

    /* Take uaddr and turn it into sos_vaddr */
    int index1 = root_index((seL4_Word) console_uio->uaddr);
    int index2 = leaf_index((seL4_Word) console_uio->uaddr);

    struct page_table_entry **page_table = pcb->addrspace->page_table;

    /* Align and add offset */
    char *sos_vaddr = PAGE_ALIGN_4K(page_table[index1][index2].sos_vaddr);
    sos_vaddr = ((seL4_Word) sos_vaddr) | ((seL4_Word) console_uio->uaddr & PAGE_MASK_4K);

    /* Write into buffer */
    *sos_vaddr = c;
    console_uio->uaddr++;
    console_uio->remaining--;
    console_uio->offset++;

    /* Check end */
    if (console_uio->remaining == 0 || c == '\n') {
        set_resume(read_coroutine_id);
    }
}

static int console_write(struct vnode *vnode, struct uio *uio) {
    seL4_Word uaddr = uio->uaddr;
    seL4_Word ubuf_size = uio->size;
    seL4_Word end_uaddr = uaddr + ubuf_size;


    while (ubuf_size > 0) {
        seL4_Word uaddr_next = PAGE_ALIGN_4K(uaddr) + PAGE_SIZE_4K;
        seL4_Word size;
        if (end_uaddr >= uaddr_next) {
            size = uaddr_next - uaddr;
        } else {
            size = ubuf_size;
        }

        /* Although we can assume the buffer is mapped because it is a write operation,
         * we still use sos_map_page to find the mapping address if it is already mapped */
        seL4_Word sos_vaddr;
        int err = sos_map_page(uaddr, &sos_vaddr, curproc);
        if (err && err != ERR_ALREADY_MAPPED) return -1;
        
        sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
        /* Add offset */
        sos_vaddr |= (uaddr & PAGE_MASK_4K);

        int bytes_sent = serial_send(serial_handle, sos_vaddr, size);
        uio->remaining -= bytes_sent;

        ubuf_size -= size;
        uaddr = uaddr_next;
        uio->offset += size;
    }

    return 0;
}

static int console_read(struct vnode *vnode, struct uio *uio) {
    read_pid = curproc->pid;
    read_stime = curproc->stime;
    read_coroutine_id = curr_coroutine_id;

    seL4_Word uaddr = uio->uaddr;
    seL4_Word ubuf_size = uio->size;

    /* Make sure address is mapped */
    seL4_Word end_uaddr = uaddr + ubuf_size;
    seL4_Word curr_uaddr = uaddr;
    seL4_Word curr_size = ubuf_size;

    while (curr_size > 0) {
        seL4_Word uaddr_next = PAGE_ALIGN_4K(curr_uaddr) + PAGE_SIZE_4K;
        seL4_Word size;
        if (end_uaddr >= uaddr_next) {
            size = uaddr_next - curr_uaddr;
        } else {
            size = curr_size;
        }

        seL4_CPtr sos_vaddr;
        int err = sos_map_page(curr_uaddr, &sos_vaddr, curproc);

        curr_size -= size;
        curr_uaddr = uaddr_next;
    }

    console_vnode = vnode;
    console_uio = uio;

    yield();
 
    console_vnode = NULL;
    console_uio = NULL;

    return 0;
}

static int console_open(struct vnode *vnode, int mode) {
    /* Only allow one read */
    if (vnode->read_count == 1 && (mode & FM_READ) != 0) {
        return 1;
    }
    return 0;
}

static int console_close(struct vnode *vnode) {
    return 0;
}
