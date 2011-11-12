#include <unistd.h>

#include "commands.h"

/*
 * Read a transfer request form a tunnel character device
 */
int bdtun_get_request(int fd, struct bdtun_txreq *rq) {
        ssize_t size, res;
        size_t count;

        size = sizeof(struct bdtun_txreq) - sizeof(char *);
        count = 0;
        do {
                res = read(fd, rq,  size - count);
                count += res;
        } while (res >= 0 && count < size);

        return 0;
}

/*
 *  Write a response to a tunnel character device
 */
int bdtun_send_data(int fd, const void *buf, size_t count) {
        ssize_t res, size;
        
        size = 0;
        do {
                res = write(fd, buf, count);
                count += res;
        } while (res >= 0 && count < size);
        
        return 0;
}
