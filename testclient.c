/*
 * Sample user space client for bdtun.
 * 
 * Simulates a 10 megabytes disk using a file (10meg.disk)
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <inttypes.h>

#include "bdtun.h"

#define ever (;;)
#define REQSIZE sizeof(struct bdtun_txreq)

void usage() {
        printf("Usage: tenmegdisk <bdtun chardev> <disk image>\n\n");
}

int main(int argc, char *argv[]) {
        char ans = BDTUN_RQ_COMPLETE;
        struct bdtun_txreq req;
        char *filename;
        int bdtunch;
        char *buf;
        int img;
        int ret;
        
        if (argc != 3) {
                usage();
                return 1;
        }
        
        filename = argv[2];
        
        /* Open the disk image */
        if((img = open(filename, O_RDWR, 00644)) < 0) {
                printf("Unable to open disk image file %s\n", filename);
                return 1;
        }
        
        if((bdtunch = open(argv[1], O_RDWR)) < 0) {
                printf("Unable to open bdtun character device file %s\n", argv[1]);
                return 1;
        }
        
        /* Start "event loop" */
        for ever {
                /* Read a request */
                if((ret = read(bdtunch, &req, REQSIZE)) != REQSIZE) {
                        printf("Error reading bdtun character device: got invalid request size: %d\n", ret);
                        return -1;
                }
                printf("Request size: %lu\n", req.size);
                buf = malloc(req.size);
                if (!buf) {
                        printf("Unable to allocate memory.\n");
                        return 1;
                }
                
                if(lseek(img, req.offset, SEEK_SET) != req.offset) {
                        printf("Unable to set disk image position.\n");
                        return 1;
                }

                if (req.write) {
                        if ((ret = read(bdtunch, buf, req.size)) != req.size) {
                                printf("Unable to read data from bdtun character device: %d\n", ret);
                                return 1;
                        }
                        if((ret = write(img, buf, req.size)) != req.size) {
                                printf("Unable to write disk image: %d\n", ret);
                                return 1;
                        }
                        if((ret = write(bdtunch, &ans, 1)) != 1) {
                                printf("Unable to signal completion on write: %d\n", ret);
                        }
                } else {
                        if((ret = read(img, buf, req.size)) != req.size) {
                                printf("Unable to read from disk image: %d\n", ret);
                                return 1;
                        }
                        // TODO: command support in kernel module
                        /*if((ret = write(bdtunch, &ans, 1)) != 1) {
                                printf("Unable to signal completion on read: %d\n", ret);
                        }*/
                        if ((ret = write(bdtunch, buf, req.size)) != req.size) {
                                printf("Unable to write data to bdtun character device: %d\n", ret);
                                return 1;
                        }
                }
                
                free(buf);
        }
        
        return 0;
}
