#ifndef _FILE_H
#define _FILE_H

#include <cspace/cspace.h>

#define MAX_OPEN_FILE 255
#define MAX_PROCESS 255

#define STD_IN 0
#define STD_OUT 1
#define STD_INOUT 2

struct oft_entry {
    sos_stat_t file_info;
    seL4_Word *ptr;
    seL4_Word ref;
    char *buffer;
    int buffer_index;
    int buffer_size;
    seL4_CPtr reply_cap;
};

#endif
