#include <cspace/cspace.h>
#include <sys/panic.h>

#include "file.h"
#include "sos.h"
#include "console.h"

struct oft_entry of_table[MAX_OPEN_FILE];
seL4_Word ofd_count = 0;
seL4_Word curr_free_ofd = 0;

void of_table_init() {
    /* Add console device */
    struct vnode *console_vnode;
    int err = console_init(&console_vnode);
    conditional_panic(err, "Could not initialise console\n");

    /* Set up of table */
    of_table[STDOUT_OFD].vnode = console_vnode;
    of_table[STDOUT_OFD].file_info.st_fmode = FM_WRITE;
    ofd_count++;
    curr_free_ofd = (STDOUT_OFD + 1) % MAX_OPEN_FILE;
}

