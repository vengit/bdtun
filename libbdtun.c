#include <unistd.h>
#include <stdlib.h>
#include <error.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "bdtun.h"

/*
 * Read a transfer request form a tunnel character device
 */
int bdtun_read_request(int fd, struct bdtun_txreq *rq) {
        ssize_t res;
        size_t bufsize = 0;
        static char *buf = NULL;
        
        res = read(fd, rq, BDTUN_TXREQ_HEADER_SIZE);
        if (res < 0) {
                return res;
        }

        /* If the buffer size is less than the request, then realloc */
        if (bufsize < rq->size) {
                buf = realloc(buf, rq->size);
                if (buf == NULL) {
                        return -ENOMEM;
                }
                bufsize = rq->size;
        }
        
        rq->buf = buf;
        
        if (rq->flags & REQ_WRITE) {
                printf("Write request, getting data from kernel\n");
                res = read(fd, rq->buf, rq->size);
                if (res < 0) {
                        return res;
                }
        }
        
        return 0;
}

/*
 * Tell the driver that the bio complete
 * 
 * If it was a read request, the buf member must contain the
 * data read by the user process.
 */
int bdtun_complete_request(int fd, struct bdtun_txreq *req) {
        int res;
        
        if (req->flags & REQ_WRITE) {
                printf("Completing write request by completion byte\n");
                res = write(fd, "\0x06", 1);
        } else {
                printf("Completing read request by sending data\n");
                res = write(fd, req->buf, req->size);
        }
        if (res < 0) {
                return res;
        }
        
        return 0;
}

/*
 * Create a device pair with the given size and name
 */
int bdtun_create(int fd, const char *name, uint64_t blocksize, uint64_t size) {
        int ret;
        struct bdtun_ctrl_command c;
        
        c.command = BDTUN_COMM_CREATE;
        c.create.blocksize = blocksize;
        c.create.size = size;
        strncpy(c.create.name, name, 32);
        
        ret = write(fd, &c, BDTUN_COMM_CREATE_SIZE);
        
        if (ret < 0) {
                return ret;
        }
        
        return 0;
}

/*
 * Resize an existing block device
 */
int bdtun_resize(int fd, const char *name, uint64_t blocksize, uint64_t size) {
        return 0;
}

/*
 * Remove a device pair
 */
int bdtun_remove(int fd, const char *name) {
        int ret;
        struct bdtun_ctrl_command c;
        
        c.command = BDTUN_COMM_REMOVE;
        strncpy(c.remove.name, name, 32);
        
        ret = write(fd, &c, BDTUN_COMM_REMOVE_SIZE);
        
        if (ret < 0) {
                return ret;
        }
        
        return 0;
}

/*
 * Get information about a device
 */
int bdtun_info(int fd, const char *name, struct bdtun_info *info) {
        int ret;
        struct bdtun_ctrl_command c;
        
        c.command = BDTUN_COMM_INFO;
        strncpy(c.info.name, name, 32);
        
        ret = write(fd, &c, BDTUN_COMM_INFO_SIZE);
        
        if (ret < 0) {
                return ret;
        }
        
        ret = read(fd, info, sizeof(struct bdtun_info));
        
        if (ret < 0) {
                return ret;
        }
        
        return 0;
}

/*
 * List devices
 */
int bdtun_list(int fd, size_t offset, size_t maxdevices, char ***names)
{
        int i, j, ret;
        struct bdtun_ctrl_command c;
        static char buf[BDTUN_RESPONSE_SIZE];
        static char *name_pbuf[BDTUN_DEVNAMES];
        
        c.command         = BDTUN_COMM_LIST;
        c.list.maxdevices = maxdevices;
        c.list.offset     = offset;
        
        ret = write(fd, &c, BDTUN_COMM_LIST_SIZE);
        
        if (ret < 0) {
                return ret;
        }
        
        ret = read(fd, buf, BDTUN_RESPONSE_SIZE);
        
        if (ret < 0) {
                return ret;
        }
        
        for (i = 0; i < BDTUN_DEVNAMES; i++) {
                name_pbuf[i] = NULL;
        }
        
        *names = name_pbuf;

        /* There is no names in the buffer, we're done. */
        if (ret == 0) {
                name_pbuf[0] = NULL;
                return 0;
        }

        /* Names are always longer than 0 
         * There is at least one name */
        name_pbuf[0] = buf;
        j = 1;
        for (i = 1; i < ret; i++) {
                if (buf[i] == 0) {
                        name_pbuf[j] = buf + i + 1;
                        j++;
                }
        }
        
        return 0;
}
