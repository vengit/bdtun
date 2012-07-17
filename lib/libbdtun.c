#include <unistd.h>
#include <stdlib.h>
#include <error.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "bdtun.h"

/* TODO: DOCS, WARNING!!!
 * 
 * In a multithreading backend the read_requests and set_requests
 * may collide, so the userspace program MUST synchronize correctly.
 * */

/*
 * Read a transfer request form a tunnel character device
 * 
 * After this, the read request will also be the data-current one.
 */
int bdtun_read_request(int fd, struct bdtun_txreq *req) {
        return read(fd, req, BDTUN_TXREQ_HEADER_SIZE);
}

/*
 * Set the request for data operations
 *
 * Sets the current request mmap and send request data work on
 */
int bdtun_set_request(int fd, struct bdtun_txreq *req)
{
        return write(fd, &req->id, sizeof(req->id));
}

/*
 * Mmap the data in the data-current request
 *
 * req must be the data-current request, because mmap have to
 * be given a size argument, and it must match the data-current
 * request's size.
 */
void *bdtun_mmap_request(int fd, struct bdtun_txreq *req)
{
        return mmap(0, req->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
}

int bdtun_munmap_request(void *buf, struct bdtun_txreq *req)
{
        return munmap(buf, req->size);
}

/*
 * Copy the data from the data-current request.
 *
 * Should only be called for write requests.
 */
ssize_t bdtun_get_request_data(int fd, struct bdtun_txreq *req, void *buf)
{
        return read(fd, buf, req->size);
}

/*
 * Copy data into the data-current request.
 *
 * Should only be called for read requests.
 */
ssize_t bdtun_send_request_data(int fd, struct bdtun_txreq *req, void *buf)
{
        return write(fd, buf, req->size);
}

/*
 * Tell the driver that the data-current bio is complete
 */
ssize_t bdtun_complete_request(int fd)
{
        return write(fd, "\0x00", 1);
}

/*
 * Tell the driver that the data-current bio is failed to complete
 */
ssize_t bdtun_fail_request(int fd)
{
        return write(fd, "\0x01", 1);
}

/*
 * Create a device pair with the given size and name
 */
int bdtun_create(int fd, const char *name, uint64_t blocksize, uint64_t size, int capabilities) {
        int ret;
        struct bdtun_ctrl_command c = {0};
        
        c.command = BDTUN_COMM_CREATE;
        c.create.blocksize = blocksize;
        c.create.size = size;
        c.create.capabilities = capabilities;
        strncpy(c.create.name, name, 32);
        
        PDEBUG(
                "Create: name: %s, bs: %" PRIu64 ", s: %" PRIu64 ", cap: %d\n",
                c.create.name, c.create.blocksize,  c.create.size, c.create.capabilities
        );
        
        ret = write(fd, &c, BDTUN_COMM_CREATE_SIZE);
        
        if (ret < 0) {
                return ret;
        }
        
        return 0;
}

/*
 * Resize an existing block device
 */
int bdtun_resize(int fd, const char *name, uint64_t size)
{
        int ret;
        struct bdtun_ctrl_command c = {0};
        
        c.command = BDTUN_COMM_RESIZE;
        c.resize.size = size;
        strncpy(c.resize.name, name, 32);
        
        ret = write(fd, &c, BDTUN_COMM_RESIZE_SIZE);
        
        if (ret < 0) {
                return ret;
        }
        
        return 0;
}

/*
 * Remove a device pair
 */
int bdtun_remove(int fd, const char *name) {
        int ret;
        struct bdtun_ctrl_command c = {0};
        
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
        struct bdtun_ctrl_command c = {0};
        
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
        struct bdtun_ctrl_command c = {0};
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
                return 0;
        }

        /* Names are always longer than 0 
         * There is at least one name */
        name_pbuf[0] = buf;
        j = 1;
        for (i = 1; i < ret; i++) {
                if (buf[i] == 0 && i + 1 < ret) {
                        name_pbuf[j] = buf + i + 1;
                        j++;
                }
        }
        
        return j;
}
