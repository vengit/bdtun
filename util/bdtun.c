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

#define KEY_NAME 'n'
#define KEY_SIZE 's'
#define KEY_BSIZE 'b'
#define KEY_FWEIO 'f'
#define KEY_FLUSH 1000
#define KEY_FUA 1001
#define KEY_DISCARD 1002
#define KEY_SECURE 1003

struct arguments {
        enum command command;
        char *name;
        uint64_t size;
        uint64_t blocksize;
        int flush_with_eio;
        int req_flush;
        int req_fua;
        int req_discard;
        int req_secure;
};

static struct argp_option options[] = {
{0, 0, 0, 0, "Informational options", -1},

{0,             0,           0,           0, "Create options",                    0},
{"name",        KEY_NAME,    "NAME",      0, "name of tunnel",                    0},
{"size",        KEY_SIZE,    "SIZE",      0, "block device size in bytes",        0},
{"block-size",  KEY_BSIZE,   "BLOCKSIZE", 0, "device block size in bytes",        0},
{"req-flush",   KEY_FLUSH,   0,           0, "support REQ_FLUSH",                 0},
{"req-fua",     KEY_FUA,     0,           0, "support REQ_FUA",                   0},
{"req-discard", KEY_DISCARD, 0,           0, "support REQ_DISCARD ",              0},
{"req-secure",  KEY_SECURE,  0,           0, "support secure discard (REQ_SAFE)", 0},

{0,      0,        0,      0, "Resize options",             1},
{"name", KEY_NAME, "NAME", 0, "name of tunnel",             1},
{"size", KEY_SIZE, "SIZE", 0, "block device size in bytes", 1},

{0,                0,         0,           0, "Remove options",             2},
{"name",           KEY_NAME,  "NAME",      0, "name of tunnel",             2},
{"flush-with-eio", KEY_FWEIO, 0,           0, "fail every pending request", 2},

{0,      0,        0,      0, "Info options",   3},
{"name", KEY_NAME, "NAME", 0, "name of tunnel", 3},

{0}
};

static int parse_opt(int key, char *arg, struct argp_state *state)
{
        struct arguments *args = state->input;

        uint64_t size = 0;
        char *endptr = 0;
        
        /* First parameter must be an argument */
        if (state->next == 2 && key != ARGP_KEY_ARG && key != ARGP_KEY_ERROR &&
            key != ARGP_KEY_END && key != ARGP_KEY_SUCCESS && key != ARGP_KEY_FINI) {
                argp_error(state, "the first parameter should be a command");
        }

        switch (key) {
        case KEY_NAME:
                if (args->command != CREATE && args->command != RESIZE &&
                    args->command != REMOVE && args->command != INFO ) {
                        argp_error(state, "'name' is only for create, resize, remove and info");
                }
                if (strlen(arg) >= 32) {
                        argp_error(state, "tunnel name must be shorter than %d", 32);
                }
                args->name = arg;
                break;
        case KEY_SIZE:
                if (args->command != CREATE && args->command != RESIZE) {
                        argp_error(state, "'size' is only for create and resize");
                }
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
                if (args->command != CREATE) {
                        argp_error(state, "'block size' is only for create");
                }
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
        case KEY_FLUSH:
                if (args->command != CREATE) {
                        argp_error(state, "--req-flush is only for create");
                }
                args->req_flush = 1;
                break;
        case KEY_FUA:
                printf("%d\n", args->command);
                if (args->command != CREATE) {
                        argp_error(state, "--req-fua is only for create");
                }
                args->req_fua = 1;
                break;
        case KEY_DISCARD:
                if (args->command != CREATE) {
                        argp_error(state, "--req-discard is only for create");
                }
                args->req_discard = 1;
                break;
        case KEY_SECURE:
                if (args->command != CREATE) {
                        argp_error(state, "--req-secure is only for create");
                }
                args->req_secure = 1;
                break;
        case KEY_FWEIO:
                if (args->command != REMOVE) {
                        argp_error(state, "--flush-with-eio is only for remove");
                }
                args->flush_with_eio = 1;
                break;
        case ARGP_KEY_NO_ARGS:
                argp_error(state, "there must be at least one argument");
                break;
        case ARGP_KEY_ARG:
                if (state->arg_num != 0) {
                        argp_error(state, "too many arguments");
                }
                if (strcmp(arg, "create") == 0) {
                        args->command = CREATE;
                } else 
                if (strcmp(arg, "remove") == 0) {
                        args->command = REMOVE;
                } else 
                if (strcmp(arg, "list") == 0) {
                        args->command = LIST;
                } else 
                if (strcmp(arg, "info") == 0) {
                        args->command = INFO;
                } else {
                        argp_error(state, "invalid argument");
                }
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
        int f, ret = 0, i, capabilities, dev;
        struct bdtun_info info;
        char **names;
        char devname[42] = "/dev/";

        struct arguments args = {0};
        
        /* Horrible-ugly hack */
        argv[0] = "bdtun <create|resize|remove|list|info>";

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
                break;
        case REMOVE:
                if (args.flush_with_eio) {
                        strcat(devname+5, args.name);
                        strcat(devname+5+strlen(args.name), "_tun");
                        dev = open(devname, O_RDWR);
                        if (!dev) {
                                printf("Could not open tunnel device %s", devname);
                                ret = 1;
                                break;
                        }
                        while(write(dev, "\x001", 1) > 0);
                        close(dev);
                }
                ret = bdtun_remove(f, args.name);
                break;
        case LIST:
                if ((ret = bdtun_list(f, 0, 32, &names)) < 0) {
                        printf("Operation failed\n");
                        break;
                }
                if (names[0] == NULL) {
                        printf("No tunnels found.\n");
                        break;
                }
                printf("The following tunnels were found:\n\n");
                i = 0;
                while (names[i] != NULL) {
                        printf("%s\n", names[i]);
                        i++;
                }
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
