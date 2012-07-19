#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>
#include <argp.h>

#include "bdtun.h"
#include "config.h"

#include <linux/fs.h>

const char *argp_program_version = PACKAGE_STRING;
const char *argp_program_bug_address = PACKAGE_BUGREPORT;

enum command {CREATE, REMOVE, RESIZE, LIST, INFO};

#define KEY_CREATE 'c'
#define KEY_REMOVE 'r'
#define KEY_RESIZE 'z'
#define KEY_LIST 'l'
#define KEY_INFO 'i'
#define KEY_NAME 'n'
#define KEY_SIZE 's'
#define KEY_BSIZE 'b'
#define KEY_FLUSH 1000
#define KEY_FUA 1001
#define KEY_DISCARD 1002
#define KEY_SECURE 1003

struct arguments {
        enum command command;
        char *name;
        uint64_t size;
        uint64_t blocksize;
        int req_flush;
        int req_fua;
        int req_discard;
        int req_secure;
};

static struct argp_option options[] = {
{0, 0, 0, 0, "Informational options", -1},

{0,        0,          0, 0, "Commands",                           0},
{"create", KEY_CREATE, 0, 0, "create a tunnel (with -bns)",        0},
{"remove", KEY_REMOVE, 0, 0, "remove a tunnel (with -n)",          0},
{"resize", KEY_RESIZE, 0, 0, "resize the block device (with -ns)", 0},
{"list",   KEY_LIST,   0, 0, "list existing tunnels",              0},
{"info",   KEY_INFO,   0, 0, "get info on a tunnel (with -n)",     0},

{0,             0,           0,           0, "Options",                           1},
{"name",        KEY_NAME,    "NAME",      0, "name of tunnel",                    1},
{"size",        KEY_SIZE,    "SIZE",      0, "block device size in bytes",        1},
{"block-size",  KEY_BSIZE,   "BLOCKSIZE", 0, "device block size in bytes",        1},
{"req-flush",   KEY_FLUSH,   0,           0, "support REQ_FLUSH",                 1},
{"req-fua",     KEY_FUA,     0,           0, "support REQ_FUA",                   1},
{"req-discard", KEY_DISCARD, 0,           0, "support REQ_DISCARD ",              1},
{"req-secure",  KEY_SECURE,  0,           0, "support secure discard (REQ_SAFE)", 1},

{0}
};

static int parse_opt(int key, char *arg, struct argp_state *state)
{
        struct arguments *args = state->input;

        uint64_t size = 0;
        char *endptr = 0;
        
        switch (key) {
        case KEY_CREATE:
                args->command = CREATE;
                break;
        case KEY_REMOVE:
                args->command = REMOVE;
                break;
        case KEY_RESIZE:
                args->command = RESIZE;
                break;
        case KEY_LIST:
                args->command = LIST;
                break;
        case KEY_INFO:
                args->command = INFO;
                break;
        case KEY_NAME:
                if (strlen(arg) >= 32) {
                        argp_error(state, "tunnel name must be shorter than %d", 32);
                }
                args->name = arg;
                break;
        case KEY_SIZE:
                size = strtoll(arg, &endptr, 10);
                if (*endptr) {
                        argp_error(state, "invalid number: %s", arg);
                }
                if (size < 1) {
                        argp_error(state, "size must be greater than zero");
                }
                args->size = size;
                break;
        case KEY_BSIZE:
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
                if (size & (size - 1)) {
                        argp_error(state, "block size must be a power of two");
                }
                args->blocksize = size;
                break;
        case KEY_FLUSH:
                args->req_flush = 1;
                break;
        case KEY_FUA:
                args->req_fua = 1;
                break;
        case KEY_DISCARD:
                args->req_discard = 1;
                break;
        case KEY_SECURE:
                args->req_secure = 1;
                break;
        case ARGP_KEY_ARG:
                argp_error(state, "too many arguments");
                break;
        case ARGP_KEY_SUCCESS:
                if (args->command == CREATE) {
                        if (args->name == NULL) {
                                argp_error(state, "a name must be given");
                        }
                        if (args->size == 0) {
                                argp_error(state, "a size must be give");
                        }
                        if (args->blocksize == 0) {
                                argp_error(state, "a block size must be given");
                        }
                        if (args->size % args->blocksize) {
                                argp_error(state, "size must be a multiple of blocksize");
                        }
                }
                if (args->command == RESIZE) {
                        if (args->name == NULL) {
                                argp_error(state, "a name must be given");
                        }
                        if (args->size == 0) {
                                argp_error(state, "a size must be give");
                        }
                }
                if (args->command == REMOVE) {
                        if (args->name == NULL) {
                                argp_error(state, "a name must be given");
                        }
                }
                if (args->command == INFO) {
                        if (args->name == NULL) {
                                argp_error(state, "a name must be given");
                        }
                }
                break;
        }
        return 0;
}

static struct argp argp = {
        options,
        parse_opt,
        NULL,
        "Manage bdtun device pairs"
};

int open_ctrldev() {
        int f;
        f = open("/dev/bdtun", O_RDWR);
        if (f < 0) {
                printf("Could not open control device /dev/bdtun\n");
                exit(errno);
        }
        return f;
}

int main(int argc, char **argv) {
        int f, ret = 0, i, capabilities, first, off;
        struct bdtun_info info;
        char **names;

        struct arguments args = {0};
        
        argp_parse(&argp, argc, argv, ARGP_IN_ORDER, 0, &args);

        f = open_ctrldev();
        
        switch (args.command) {
        case CREATE:
                capabilities = 0;
                if (args.req_flush) {
                        capabilities |= BDTUN_FLUSH;
                }
                if (args.req_fua) {
                        capabilities |= BDTUN_FUA;
                }
                if (args.req_discard) {
                        capabilities |= BDTUN_DISCARD;
                }
                if (args.req_secure) {
                        capabilities |= BDTUN_SECURE;
                }
                ret = bdtun_create(f, args.name, args.blocksize, args.size, capabilities);
                break;
        case RESIZE:
                ret = bdtun_resize(f, args.name, args.size);
                break;
        case REMOVE:
                ret = bdtun_remove(f, args.name);
                break;
        case LIST:
                first = 1;
                off = 0;
                do {
                        if ((ret = bdtun_list(f, off, 32, &names)) < 0) {
                                printf("Operation failed\n");
                                break;
                        }
                        if (first) {
                                if (names[0] == NULL) {
                                        printf("No tunnels found.\n");
                                } else {
                                        printf("The following tunnels were found:\n\n");
                                }
                                first = 0;
                        }
                        for (i = 0; i < ret; i++) {
                                printf("%s\n", names[i]);
                                i++; off++;
                        }
                } while (ret);
                
                break;
        case INFO:
                if ((ret = bdtun_info(f, args.name, &info)) < 0) {
                        break;
                }
                printf(
                        "Information for device %s:\n\n"
                        "Size in bytes:      %" PRIu64 "\n"
                        "Block size:         %" PRIu64 "\n"
                        "Block device major: %d\n"
                        "Block device minor: %d\n"
                        "Char device major:  %d\n"
                        "Char device minor:  %d\n\n",
                        args.name,
                        info.bd_size, info.bd_block_size,
                        info.bd_major, info.bd_minor,
                        info.ch_major, info.ch_minor                        
                );
                break;
        }
        
        close(f);
        
        if (ret < 0) {
                printf("Operation failed\n");
                return 1;
        }
        
        return 0;

}
