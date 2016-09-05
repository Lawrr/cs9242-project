#ifndef _FILE_H_
#define _FILE_H_

#include <cspace/cspace.h>

#define MAX_OPEN_FILE 255
#define MAX_PROCESS 255

//#define STDIN 0
#define STDOUT 0

struct oft_entry {
    sos_stat_t file_info;
    struct vnode *vnode;
    int ref_count;
    int offset;
};

#endif
