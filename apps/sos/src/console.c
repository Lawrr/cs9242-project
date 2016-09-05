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

extern struct PCB tty_test_process;
extern int curr_coroutine_id;
int my_coroutine_id;
static struct serial *serial_handle;
static struct vnode *console_vnode;
static struct uio console_uio;

static void console_serial_handler(struct serial *serial, char c) {
    /* Return if we do not currently need to read */
    if (console_uio.addr == NULL || console_uio.remaining == 0) return;

    seL4_Word *vnode_data = (seL4_Word *) (console_vnode->data);

    /* Take uaddr and turn it into sos_vaddr */
    seL4_Word index1 = ((seL4_Word) console_uio.addr >> 22);
    seL4_Word index2 = ((seL4_Word) console_uio.addr << 10) >> 22;
    struct page_table_entry **page_table = (struct page_table **) vnode_data[1];

    char *sos_vaddr = PAGE_ALIGN_4K(page_table[index1][index2].sos_vaddr);
    /* Add offset */
    sos_vaddr = ((seL4_Word) sos_vaddr) | ((seL4_Word) console_uio.addr & PAGE_MASK_4K);

    /* Write into buffer */
    *sos_vaddr = c;
    console_uio.addr++;
    console_uio.remaining--;

    /* Check end */
    if (console_uio.remaining == 0 || c == '\n') {
        set_resume(my_coroutine_id);
    }
}

int console_write(struct vnode *vnode, struct uio *uio) {
    seL4_Word uaddr = uio->addr;
    seL4_Word ubuf_size = uio->size;
    seL4_Word end_uaddr = uaddr + ubuf_size;

    while (ubuf_size > 0) {
        seL4_Word uaddr_next = PAGE_ALIGN_4K(uaddr) + 0x1000;
        seL4_Word size;
        if (end_uaddr >= uaddr_next) {
            size = uaddr_next-uaddr;
        } else {
            size = ubuf_size;
        }

        /* Though we can assume the buffer is mapped because it is a write operation,
         * we still use sos_map_page to find the mapping address if it is already mapped */
        seL4_CPtr app_cap;
        seL4_CPtr sos_vaddr;
        int err = sos_map_page(uaddr,
                tty_test_process.vroot,
                tty_test_process.addrspace,
                &sos_vaddr,
                &app_cap);
        if (err && err != ERR_ALREADY_MAPPED) {
            return 1;
        }
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

int console_read(struct vnode *vnode, struct uio *uio) {
    seL4_Word uaddr = uio->addr;
    seL4_Word ubuf_size = uio->size;

    /* Make sure address is mapped */
    seL4_Word end_uaddr = uaddr + ubuf_size;
    seL4_Word curr_uaddr = uaddr;
    seL4_Word curr_size = ubuf_size;

    my_coroutine_id = curr_coroutine_id;
    while (curr_size > 0) {
        seL4_Word uaddr_next = PAGE_ALIGN_4K(curr_uaddr) + 0x1000;
        seL4_Word size;
        if (end_uaddr >= uaddr_next) {
            size = uaddr_next-curr_uaddr;
        } else {
            size = curr_size;
        }

        seL4_CPtr app_cap;
        seL4_CPtr sos_vaddr;
        int err = sos_map_page(curr_uaddr,
                               tty_test_process.vroot,
                               tty_test_process.addrspace,
                               &sos_vaddr,
                               &app_cap);

        curr_size -= size;
        curr_uaddr = uaddr_next;
    }

    console_vnode = vnode;
    console_uio = *uio;
    yield();
    return 0;
}

int console_open(struct vnode *vnode, int mode) {
    /* Only allow one read */
    if (vnode->read_count == 1 && (mode & FM_READ) != 0) {
        return 1;
    }
    return 0;
}

int console_close(struct vnode *vnode) {
    return 0;
}

void console_init(struct vnode **ret_vnode) {
    /* Initialise serial driver */
    serial_handle = serial_init();
    serial_register_handler(serial_handle, console_serial_handler);

    /* Set up console dev */
    struct vnode_ops *console_ops = malloc(sizeof(struct vnode_ops));
    console_ops->vop_open = &console_open;
    console_ops->vop_close = &console_close;
    console_ops->vop_read = &console_read;
    console_ops->vop_write = &console_write;

    int err = dev_add("console", console_ops);
    conditional_panic(err, "Could not add console serial device");

    /* Set return console vnode */
    /* FM_WRITE for STDOUT */
    err = vfs_open("console", FM_WRITE, ret_vnode);
    conditional_panic(err, "Registered console dev not found");
}
