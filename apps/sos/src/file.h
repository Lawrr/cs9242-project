#ifndef _FILE_H_
#define _FILE_H_

#include <cspace/cspace.h>

#include "sos.h"

#define MAX_OPEN_FILE 255

#define STDOUT_OFD 0

#define STDOUT_FD 1
#define STDERR_FD 2

struct oft_entry {
    sos_stat_t file_info;
    struct vnode *vnode;
    int ref_count;
    int offset;
};

void of_table_init();

#endif
