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

extern struct serial *serial_handle;

void console_init() {
    struct vnode_ops *console_ops = malloc(sizeof(struct vnode_ops));
    console_ops->vop_open = &console_open;
    console_ops->vop_close = &console_close;
    console_ops->vop_read = &console_read;
    console_ops->vop_write = &console_write;
    int err = dev_add("console", console_ops);
    conditional_panic(err, "Could not add console serial device");

    serial_register_handler(serial_handle, console_serial_handler);
}

static void console_serial_handler(struct serial *serial, char c) {
    struct oft_entry *entry = &of_table[STD_IN];

    /* Return if we do not currently need to read */
    if (entry->buffer == NULL || entry->buffer_size == 0) return;

    /* Check if we are on a new page */
    if (((seL4_Word) entry->buffer & PAGE_MASK_4K) == 0) {
        seL4_CPtr dummy_sos_vaddr;
        seL4_CPtr dummy_app_cap;
        int err = sos_map_page(entry->buffer,
                tty_test_process.vroot,
                tty_test_process.addrspace,
                &dummy_sos_vaddr,
                &dummy_app_cap);
    }

    /* Take uaddr and turn it into sos_vaddr */
    seL4_Word index1 = ((seL4_Word) entry->buffer >> 22);
    seL4_Word index2 = ((seL4_Word) entry->buffer << 10) >> 22;

    char *sos_vaddr = PAGE_ALIGN_4K(tty_test_process.addrspace->page_table[index1][index2].sos_vaddr);
    /* Add offset */
    sos_vaddr = ((seL4_Word) sos_vaddr) | ((seL4_Word) entry->buffer & PAGE_MASK_4K);

    /* Write into buffer */
    *sos_vaddr = c;
    entry->buffer++;
    entry->buffer_count++;

    /* Check end */
    if (entry->buffer_count == entry->buffer_size || c == '\n') {
        entry->buffer_size = 0;
        entry->buffer = NULL;

        /* Reply */
        if (entry->reply_cap != CSPACE_NULL) {
            seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
            seL4_SetMR(0, entry->buffer_count);
            seL4_Send(entry->reply_cap, reply);
            cspace_free_slot(cur_cspace, entry->reply_cap);

            entry->reply_cap = CSPACE_NULL;
        }
    }
}


int console_write(struct vnode *vn, struct uio *uio) {
    bytes_sent += serial_send(serial_handle, sos_vaddr, size);
    uio->remaining -= bytes_sent;
    return 0;
}

int console_read(struct vnode *vn, struct uio *uio) {
    of_table[STD_IN].buffer = uaddr;
    of_table[STD_IN].buffer_size = ubuf_size;
    of_table[STD_IN].buffer_count = 0;
    of_table[STD_IN].reply_cap = reply_cap;
    return 0;
}
