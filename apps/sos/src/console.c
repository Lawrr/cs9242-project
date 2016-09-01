#include <cspace/cspace.h>

#include <serial/serial.h>
#include <utils/page.h>
#include <fcntl.h>

#include "mapping.h"
#include "sos.h"
#include "vnode.h"
#include "console.h"

#include <sys/debug.h>
#include <sys/panic.h>

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
        seL4_CPtr reply_cap = (seL4_CPtr) vnode_data[0];
        free(console_vnode->data);

        /* Reply */
        if (reply_cap != CSPACE_NULL) {
            seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
            seL4_SetMR(0, console_uio.size - console_uio.remaining);
            seL4_Send(reply_cap, reply);
            cspace_free_slot(cur_cspace, reply_cap);
        }

        console_uio.addr = NULL;
        console_uio.remaining = 0;
    }
}

int console_write(struct vnode *vnode, struct uio *uio) {
    int bytes_sent = serial_send(serial_handle, uio->addr, uio->size);
    uio->remaining -= bytes_sent;

    return 0;
}

int console_read(struct vnode *vnode, struct uio *uio) {
    console_vnode = vnode;
    console_uio = *uio;

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
