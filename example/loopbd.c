#include <argp.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "bdtun_backend.h"

// Name of this backend
char * backend_program_name = "loopbd";

// Backend-specific global state and configuration
struct backend_args {
        char *filename;
        int fd;
        void *imgmap;
};

// Backend-specific argp settings
static struct argp_option options[] = {
{"filename",     'f', "FILENAME",           0, "Name and path of loopback file"},
{0}
};

static int parse_opt(int key, char *arg, struct argp_state *state)
{
        struct backend_args *args = (struct backend_args *)state->input;

        switch (key) {
        case 'f':
                args->filename = arg;
                break;
        }
        
        return 0;
}

static struct argp argp = {
        options,
        parse_opt
};

/*
 * Initializes custom argp parsing, and maybe others
 */
int backend_init() {
        args.backend_args = (void *)calloc(1, sizeof(struct backend_args));
        if (args.backend_args == 0) {
                LOG_ERROR("backend_init: cannot allocate memory\n");
                return -1;
        }
        set_argp(&argp);
        PDEBUG("backend_init: initialized argp\n");
        
        return 0;
}

/*
 * Do some deinitialization, if needed
 */
void backend_deinit() {
        free(args.backend_args);
}

/*
 * Opens the backend
 */
int backend_open() {
        struct backend_args *bargs = 
                (struct backend_args *)args.backend_args;
        struct stat sts;
        int created = 0;
        
        if (bargs->filename == 0) {
                bargs->filename = strdup(args.tunnel);
        }
        
        if (stat(bargs->filename, &sts) == -1) {
                if (errno == ENOENT) {
                        bargs->fd = open(bargs->filename, O_RDWR | O_CREAT, 00600);
                        created = 1;
                } else {
                        LOG_ERROR("Cannot stat file %s\n", bargs->filename);
                        return -1;
                }
        } else {
                if (sts.st_size != args.size) {
                        LOG_ERROR("Existing file %s size %zd does not match given size %" PRIu64 "\n",
                                bargs->filename, sts.st_size, args.size);
                        return -1;
                }
                bargs->fd = open(bargs->filename, O_RDWR);
        }
        
        if (bargs->fd < 0) {
                LOG_ERROR("Cannot open or create file %s\n", bargs->filename);
                return -1;
        }
        
        bargs->imgmap = mmap(0, args.size, PROT_READ | PROT_WRITE, MAP_SHARED, bargs->fd, 0);
        
        if (bargs->imgmap < 0) {
                LOG_ERROR("Cannot mmap file %s\n", bargs->filename);
                close(bargs->fd);
                if (created)
                        unlink(bargs->filename);
                return -1;
        }
        
        return 0;
}

/*
 * Closes the backend
 */
void backend_close() {
        struct backend_args *bargs = 
                (struct backend_args *)args.backend_args;
                
        munmap(bargs->imgmap, args.size);
        close(bargs->fd);
        
        return;
}

/*
 * Called on read requests
 */
int backend_read(struct bdtun_txreq *req) {
        struct backend_args *bargs = 
                (struct backend_args *)args.backend_args;

        memcpy(req->buf, bargs->imgmap + req->offset, req->size);
        return 0;
}

/*
 * Called on write requests
 */
int backend_write(struct bdtun_txreq *req) {
        struct backend_args *bargs = 
                (struct backend_args *)args.backend_args;

        memcpy(bargs->imgmap + req->offset, req->buf, req->size);
        return 0;
}
