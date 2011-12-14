#include <unistd.h>
#include <stdlib.h>
#include <error.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>

#include "bdtun.h"

/*
 * Read a transfer request form a tunnel character device
 */
int bdtun_read_request(int fd, struct bdtun_txreq *rq) {
        ssize_t res;
        size_t count;
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
        ssize_t res, size;
        
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
int bdtun_create(int fd, char *name, uint64_t size) {
        return 0;
}

/*
 * Resize an existing block device
 */
int bdtun_resize(int fd, char *name, uint64_t size) {
        return 0;
}

/*
 * Remove a device pair
 */
int bdtun_remove(int fd, char *name) {
        return 0;
}

/*
 * Get information about a device
 */
int bdtun_info(int fd, char *name, struct bdtun_info *info) {
        return 0;
}

/*
 * List devices
 */
char **bdtun_list(int fd, size_t offset, size_t maxdevices) {
        return NULL;
}
