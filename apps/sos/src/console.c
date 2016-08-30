#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <cspace/cspace.h>

#include <cpio/cpio.h>
#include <nfs/nfs.h>
#include <elf/elf.h>
#include <serial/serial.h>
#include <clock/clock.h>
#include <utils/page.h>
#include <fcntl.h>

#include "addrspace.h"
#include "frametable.h"
#include "network.h"
#include "elf.h"
#include "sos_syscall.h"
#include "ut_manager/ut.h"
#include "vmem_layout.h"
#include "mapping.h"
#include "sos.h"
#include "file.h"
#include "process.h"
#include "vnode.h"
#include "console.h"
#include <autoconf.h>

#define verbose 5
#include <sys/debug.h>
#include <sys/panic.h>

static struct serial *serial_handle;
static struct vnode *console_vn;
static struct uio console_uio;

static void console_serial_handler(struct serial *serial, char c) {
    /* Return if we do not currently need to read */
    if (console_uio.bufAddr == NULL || console_uio.remaining == 0) return;


    /* Take uaddr and turn it into sos_vaddr */
    seL4_Word index1 = ((seL4_Word) console_uio.bufAddr >> 22);
    seL4_Word index2 = ((seL4_Word) console_uio.bufAddr << 10) >> 22;
    struct page_table_entry **page_table = ((seL4_Word *) (console_vn->vn_data))[1];

    char *sos_vaddr = PAGE_ALIGN_4K(page_table[index1][index2].sos_vaddr);
    /* Add offset */
    sos_vaddr = ((seL4_Word) sos_vaddr) | ((seL4_Word) console_uio.bufAddr & PAGE_MASK_4K);

    /* Write into buffer */
    *sos_vaddr = c;
    console_uio.bufAddr++;
    console_uio.remaining--;

    /* Check end */
    if (console_uio.remaining == 0 || c == '\n') {
        console_uio.bufAddr = NULL;
        console_uio.remaining = 0;

        seL4_CPtr reply_cap = ((seL4_Word *) (console_vn->vn_data))[0];
        free(console_vn->vn_data);

        /* Reply */
        if (reply_cap != CSPACE_NULL) {
            seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
            seL4_SetMR(0, console_uio.bufSize);
            seL4_Send(reply_cap, reply);
            cspace_free_slot(cur_cspace, reply_cap);
        }
    }
}


int console_write(struct vnode *vn, struct uio *uio) {
    int bytes_sent = serial_send(serial_handle, uio->bufAddr, uio->bufSize);
    uio->remaining -= bytes_sent;
    return 0;
}

int console_read(struct vnode *vn, struct uio *uio) {
    printf("CONSOLE READ\n");
    console_vn = vn;
    console_uio = *uio;
    return 0;
}

int console_open(struct vnode *vnode, char *path) {
    return 0;
}

int console_close(struct vnode *vnode) {
    return 0;
}

void console_init(struct vnode **vnode) {
    /* Initialise serial driver */
    serial_handle = serial_init();
    serial_register_handler(serial_handle, console_serial_handler);

    struct vnode_ops *console_ops = malloc(sizeof(struct vnode_ops));
    console_ops->vop_open = &console_open;
    console_ops->vop_close = &console_close;
    console_ops->vop_read = &console_read;
    console_ops->vop_write = &console_write;
    int err = dev_add("console", console_ops);
    vfs_open("console", vnode);
    conditional_panic(err, "Could not add console serial device");
}

