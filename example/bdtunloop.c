/*
 * Sample user space client for bdtun.
 * 
 * It acts as a loopback device.
 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <inttypes.h>
#include <argp.h>
#include <sys/mman.h>
#include <assert.h>

#include "bdtun.h"
#include "config.h"

#define ever (;;)
#define REQSIZE sizeof(struct bdtun_txreq)

const char *argp_program_version = PACKAGE_STRING;
const char *argp_program_bug_address = PACKAGE_BUGREPORT;

static struct argp_option options[] = {

{"block-size", 'b', "BLOCKSIZE", 0, "block size in bytes"},
{"create-tun", 'c', 0,           0, "create the BDTun device pair"},
{"create-img", 'i', 0,           0, "create / truncate disk image"},
{"size",       's', "SIZE",      0, "device size in bytes"},
{"zero",       'z', 0,           0, "fill disk with zeroes"},
{0}

};

struct arguments {
        int create_tun;
        int create_img;
        char *tunnel;
        char *device;
        char *filename;
        uint64_t size;
        uint64_t blocksize;
        int zero;
};

static int parse_opt(int key, char *arg, struct argp_state *state)
{
        struct arguments *args = state->input;

        uint64_t size = 0;
        char *endptr = 0;

        switch (key) {
        case 'c':
                args->create_tun = 1;
                break;
        case 'i':
                args->create_img = 1;
                break;
        case 's':
                size = strtoll(arg, &endptr, 10);
                if (*endptr) {
                        argp_error(state, "invalid number: %s", arg);
                }
                if (size < 1) {
                        argp_error(state, "size must be greater than zero");
                }
                args->size = size;
                break;
        case 'b':
                size = strtoll(arg, &endptr, 10);
                if (*endptr) {
                        argp_error(state, "invalid number: %s", arg);
                }
                if (size < 512) {
                        argp_error(state, "block size must be at least 512");
                }
                if (size > 4096) {
                        argp_error(state, "block size must be at most 4096");
                }
                if (size % 512) {
                        argp_error(state, "block size must be a multiple of 512");
                }
                args->blocksize = size;
                break;
        case 'z':
                args->zero = 1;
                break;
        case ARGP_KEY_ARG:
                if (state->arg_num > 1) {
                        argp_error(state, "too many arguments");
                }
                if (state->arg_num == 0) {
                        if (strlen(arg) >= 32) {
                                argp_error(state, "tunnel name must be shorter than %d", 32);
                        }
                        args->tunnel = arg;
                }
                if (state->arg_num == 1) {
                        args->filename = arg;
                }
                break;
        case ARGP_KEY_SUCCESS:
                if (state->arg_num < 2) {
                        argp_error(state, "not enough arguments");
                }
                if (args->create_tun && args->create_img && !args->size) {
                        argp_error(state, "you must provide a size");
                }
                break;
        }
        return 0;
}

static struct argp argp = {
        options,
        parse_opt,
        "<tunnel name> <filename>",
        "example user space loopback block device driver"
};

int main(int argc, char *argv[])
{
        struct arguments args = {0};
        char devname[42] = "/dev/";
        struct bdtun_txreq req;
        struct bdtun_info info = {0};
        struct stat stat;
        int bdtunch;
        int ctrldev;
        int img;
        int ret;
        char *imgmap;
                
        argp_parse(&argp, argc, argv, 0, 0, &args);
        
        strcat(devname+5, args.tunnel);
        strcat(devname+5+strlen(args.tunnel), "_tun");

        /* Open control device */
        ctrldev = open("/dev/bdtun", O_RDWR);
        if (ctrldev < 0) {
                printf("Could not open control device /dev/bdtun\n");
                return 1;
        }

        if (!args.create_tun) {
                ret = bdtun_info(ctrldev, args.tunnel, &info);
                if (ret) {
                        printf("Could not query tunnel %s. Does it exist?\n", args.tunnel);
                        return 1;
                }
                if (args.blocksize != 0) {
                        if (args.blocksize != info.bd_block_size) {
                                printf("Tunnelled device block size of %" PRIu64
                                " doesn't match given block size of %" PRIu64 "\n",
                                info.bd_block_size, args.blocksize);
                                return -1;
                        }
                } else {
                        args.blocksize = info.bd_block_size;
                }
                if (args.size != 0 && args.size != info.bd_size) {
                        printf("Tunnelled device size of %" PRIu64
                               " doesn't match given size of %" PRIu64 "\n",
                               info.bd_size, args.size);
                        return -1;
                }
        } else {
                if (args.blocksize == 0) {
                        args.blocksize = 512;
                }
        }
        assert(args.blocksize);
        
        if (args.create_img) {
                if (args.size == 0) {
                        assert(info.bd_size);
                        args.size = info.bd_size;
                }
                
                img = open(args.filename, O_RDWR | O_CREAT, 00600);
                if (img < 0) {
                        printf("Unable to create image file %s\n", args.filename);
                        return 1;
                }
                if (ftruncate(img, args.size)) {
                        printf("Could not truncate file %s to the given size %" PRIu64, args.filename, args.size);
                        return errno;
                }
        } else {
                img = open(args.filename, O_RDWR);
                if (img < 0) {
                        printf("Unable to open image file %s\n", args.filename);
                        return 1;
                }
                if(fstat(img, &stat) < 0) {
                        printf("Unable to stat image %s\n", args.filename);
                        return 1;
                }
                if (args.size == 0) {
                        args.size = stat.st_size;
                } else if (args.size != stat.st_size) {
                        printf("Tunnel size of %" PRIu64 " does not match file size of %zd\n", args.size, stat.st_size);
                        return 1;
                        
                }
                if(args.size % args.blocksize) {
                        printf("size of %" PRIu64 " is not a multiple of "
                               "blocksize of %" PRIu64 "\n",
                               stat.st_size, args.blocksize);
                        return 1;
                }
        }
        assert(args.size);
        assert(img);
        
        /* Create the device if needed */
        if (args.create_tun) {
                ret = bdtun_create(ctrldev, args.tunnel, args.blocksize, args.size, 0);
                if (ret < 0) {
                        printf("Could not create tunnel\n");
                        return 1;
                }
        }
        
        if((bdtunch = open(devname, O_RDWR)) < 0) {
                printf("Unable to open bdtun character device file %s\n", argv[1]);
                return 1;
        }
        
        close(ctrldev);
        
        imgmap = mmap(0, args.size, PROT_READ | PROT_WRITE, MAP_SHARED, img, 0);
        
        if (!imgmap) {
                printf("Could not mmap image\n");
                close(img);
                if (args.create_tun) {
                        ret = bdtun_remove(ctrldev, args.tunnel);
                        if (ret < 0) {
                                printf("Additionally, could not remove just-created tunnel %s\n", args.tunnel);
                        }
                }
                return 1;
        }

        if (args.zero) {
                printf("Writing zeros to image...\n");
                bzero(imgmap, args.size);
        }

        /* Start "event loop" */
        PDEBUG("Starting event loop\n");
        for ever {
                
                /* Read a request */
                PDEBUG("Reading request\n");
                if((ret = bdtun_read_request(bdtunch, &req)) != 0) {
                        printf("(3) Counld not get request from device: %d\n", ret);
                        return -1;
                }
                PDEBUG("Size: %lu\n", req.size);
                
                /* Read / write backing file */
                if (req.flags & REQ_WRITE) {
                        PDEBUG("Writing to disk image\n");
                        memcpy(imgmap + req.offset, req.buf, req.size);
                        // TODO: make this a parameter
                        msync(imgmap, args.size, MS_SYNC);
                } else {
                        PDEBUG("Reading from disk image\n");
                        memcpy(req.buf, imgmap + req.offset, req.size);
                }
                
                /* Complete request */
                PDEBUG("Completing request\n");
                if(bdtun_complete_request(bdtunch, &req)) {
                        printf("(7) Unable to signal completion on write: %d\n", ret);
                }
        }
        
        return 0;
}
