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
#include <autoconf.h>

#define verbose 5
#include <sys/debug.h>
#include <sys/panic.h>

#define READ_DELAY 10000 /* Microseconds */

extern struct PCB tty_test_process;
extern struct serial *serial_handle;
extern struct oft_entry of_table[MAX_OPEN_FILE];
extern seL4_Word ofd_count;
extern seL4_Word curr_free_ofd;
extern char *console;

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

/* Checks that user pointer is a valid address in userspace */
static int legal_uaddr(seL4_Word uaddr) {
    /* Check valid region */
    struct region *curr = tty_test_process.addrspace->regions;
    while (curr != NULL) {
        if (uaddr >= curr->baseaddr && uaddr < curr->baseaddr + curr->size) {
            break;
        }
        curr = curr->next;
    }

    /* User pointers should be below IPC buffer */
    if (curr != NULL && uaddr < PROCESS_IPC_BUFFER) {
        return 1;
    }
    return 0;
}

/* Checks that user pointer range is a valid in userspace */
static int legal_uaddr_range(seL4_Word base, uint32_t size) {
    /* Check valid region */
    struct region *curr = tty_test_process.addrspace->regions;
    while (curr != NULL) {
        if (base >= curr->baseaddr &&
            base + size < curr->baseaddr + curr->size) {
            break;
        }
        curr = curr->next;
    }

    /* User pointers should be below IPC buffer */
    if (curr != NULL && base + size < PROCESS_IPC_BUFFER) {
        return 1;
    }
    return 0;
}

void syscall_brk(seL4_CPtr reply_cap) {
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    uintptr_t newbrk = seL4_GetMR(1);

    /* Find heap region */
    struct region *curr_region = tty_test_process.addrspace->regions;
    while (curr_region != NULL &&
           curr_region->baseaddr != PROCESS_HEAP_START) {
        curr_region = curr_region->next;
    }

    /* Check that newbrk is before HEAP_END (and that we even have a heap region...) */
    if (curr_region == NULL || newbrk >= PROCESS_HEAP_END) {
        /* Set error */
        seL4_SetMR(0, 1);
        seL4_Send((seL4_CPtr) reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
    }

    printf("brk moved heap region end from %p to %p\n",
           curr_region->baseaddr + curr_region->size,
           curr_region->baseaddr + (newbrk - PROCESS_HEAP_START));

    /* Set new heap region */
    curr_region->size = newbrk - PROCESS_HEAP_START;

    /* Reply */
    seL4_SetMR(0, 0);
    seL4_Send(reply_cap, reply);
}

static void uwakeup(uint32_t id, void *reply_cap) {
    /* Wake up and reply back to application */
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 0);
    seL4_Send((seL4_CPtr) reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

void syscall_usleep(seL4_CPtr reply_cap) {
    int msec = seL4_GetMR(1);

    /* Make sure sec is positive */
    if (msec < 0) {
        msec = 0;
    }

    register_timer(msec * 1000, &uwakeup, (void *) reply_cap);
}

void syscall_time_stamp(seL4_CPtr reply_cap) {
    timestamp_t timestamp = time_stamp();

    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, timestamp); 
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

void syscall_write(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word uaddr = seL4_GetMR(2);
    seL4_Word ubuf_size = seL4_GetMR(3);

    /* Check ubuf_size */
    if (ubuf_size <= 0) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_ARGUMENT); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    /* Check user address */
    if (!legal_uaddr_range(uaddr, ubuf_size)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_USERADDR); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    /* Check fd */
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_FD);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    seL4_Word ofd = tty_test_process.addrspace->fd_table[fd].ofd;
    /* Check ofd */
    if (ofd == -1) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_FD);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /* Check access right */
    if (!(of_table[ofd].file_info.st_fmode & FM_WRITE)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_ACCESS_MODE); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    int bytes_sent = 0;
    if (ofd == STD_OUT || ofd == STD_INOUT) {
        /* Console */
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
            sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
            /* Add offset */
            sos_vaddr |= (uaddr & PAGE_MASK_4K);

            bytes_sent += serial_send(serial_handle, sos_vaddr, size);
            ubuf_size -= size;
            uaddr = uaddr_next;
        }
    } else {
        /* TODO actually manipulate file */
    }

    /* Reply */
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, bytes_sent);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

void syscall_read(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word uaddr = seL4_GetMR(2);
    seL4_Word ubuf_size = seL4_GetMR(3);
    
    /* Check ubuf_size */
    if (ubuf_size <= 0) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_ARGUMENT); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    /* Check user address */
    if (!legal_uaddr_range(uaddr, ubuf_size)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_USERADDR); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    /* Check fd */
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0,ERR_INVALID_FD); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /* Make sure address is mapped */
    seL4_CPtr app_cap;
    seL4_CPtr sos_vaddr;
    int err = sos_map_page(uaddr,
            tty_test_process.vroot,
            tty_test_process.addrspace,
            &sos_vaddr,
            &app_cap);

    sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
     /* Add offset */
    sos_vaddr |= (uaddr & PAGE_MASK_4K);

    seL4_Word ofd = tty_test_process.addrspace->fd_table[fd].ofd;
    /* Check ofd */
    if (ofd == -1) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_FD); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /* Check access right */
    if (!(of_table[ofd].file_info.st_fmode & FM_READ)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_ACCESS_MODE);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    if (ofd == STD_IN || ofd == STD_INOUT) {
        /* Console */
        of_table[STD_IN].buffer = uaddr;
        of_table[STD_IN].buffer_size = ubuf_size;
        of_table[STD_IN].buffer_count = 0;
        of_table[STD_IN].reply_cap = reply_cap;
        serial_register_handler(serial_handle, console_serial_handler);
    } else {
        /* TODO actually manipulate file */

        /* Free slot only if reply is handled */
        cspace_free_slot(cur_cspace, reply_cap);
    }
}

void syscall_open(seL4_CPtr reply_cap) {
    seL4_Word fdt_status = tty_test_process.addrspace->fdt_status;			  
    seL4_Word free_fd = fdt_status & LOWER_TWO_BYTE_MASK;
    seL4_Word fd_count = fdt_status >> TWO_BYTE_BITS;

    seL4_Word uaddr = seL4_GetMR(1);
    fmode_t access_mode = seL4_GetMR(2); 

    if (fd_count == PROCESS_MAX_FILES) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_MAX_FILE);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    } else if (ofd_count == MAX_OPEN_FILE) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_MAX_SYSTEM_FILE);
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    /* Check user address */
    if (!legal_uaddr(uaddr)) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_ILLEGAL_USERADDR); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /* Make sure address is mapped */
    seL4_CPtr app_cap;
    seL4_Word sos_vaddr;
    int err = sos_map_page(uaddr,
                           tty_test_process.vroot,
                           tty_test_process.addrspace,
                           &sos_vaddr,
                           &app_cap);

    sos_vaddr = PAGE_ALIGN_4K(sos_vaddr);
    sos_vaddr |= (uaddr & PAGE_MASK_4K);

    /* Check if it's console */
    char *path = (char *) sos_vaddr;
    if (!strcmp(path, console)) {
        int ofd;
        if (access_mode == O_RDWR) {
            ofd = STD_INOUT;
        } else if (access_mode != O_WRONLY) {
            ofd = STD_IN;
        } else {
            ofd = STD_OUT;
        }
        tty_test_process.addrspace->fd_table[free_fd].ofd = ofd;
        of_table[ofd].ref++;
    } else {
        /* TODO Actual file manipulation */

        tty_test_process.addrspace->fd_table[free_fd].ofd = curr_free_ofd;

        /* TODO: NEED TO CHANGE LATER! */
        of_table[curr_free_ofd].ptr = console;

        /* Set access mode and add ref count */
        of_table[curr_free_ofd].file_info.st_fmode = access_mode;
        of_table[curr_free_ofd].ref++;

        ofd_count++;
        while (of_table[curr_free_ofd].ptr != NULL) {
            curr_free_ofd = (curr_free_ofd + 1) % MAX_OPEN_FILE;  
        }
    }

    /* Compute free_fd */
    fd_count++;

    seL4_SetMR(0, free_fd);

    while (tty_test_process.addrspace->fd_table[free_fd].ofd != -1) {
        free_fd = (free_fd + 1) % PROCESS_MAX_FILES;
    }

    tty_test_process.addrspace->fdt_status = (fd_count << TWO_BYTE_BITS) |
                                             free_fd;

    /* Reply */
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_Send(reply_cap, reply);
    cspace_free_slot(cur_cspace, reply_cap);
}

void syscall_close(seL4_CPtr reply_cap) {
    int fd = seL4_GetMR(1);
    seL4_Word ofd = tty_test_process.addrspace->fd_table[fd].ofd;

    /* Check fd */
    if (fd < 0 || fd >= PROCESS_MAX_FILES) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_FD); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }
    /* Check ofd */	
    if (ofd == -1 || of_table[ofd].ref == 0) {
        seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
        seL4_SetMR(0, ERR_INVALID_FD); 
        seL4_Send(reply_cap, reply);
        cspace_free_slot(cur_cspace, reply_cap);
        return;
    }

    /* TODO Close actual file
     *
     *
     *
     *
     * */

    tty_test_process.addrspace->fd_table[fd].ofd = -1;
    of_table[ofd].ref--;

    if (ofd == STD_IN || ofd == STD_OUT || ofd == STD_INOUT) {
        /* Console related */
    } else {
        if (of_table[curr_free_ofd].ref == 0) {
            of_table[curr_free_ofd].ptr = NULL;
            of_table[curr_free_ofd].file_info.st_fmode = 0;
        }
    }

    /* Reply */
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, 1);
    seL4_SetMR(0, 0);
    seL4_Send(reply_cap, reply);       	
    cspace_free_slot(cur_cspace, reply_cap);
}
