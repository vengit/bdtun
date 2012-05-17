#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "bdtun_backend.h"

// Name of this backend
char * backend_program_name = "membd";

// Backend-specific global state and configuration
struct backend_args {
        char *memmap;
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
        
        return 0;
}

/*
 * Do some deinitialization, if needed
 */
void backend_deinit() {
        free(args.backend_args);
        return;
}

/*
 * Opens the backend
 */
int backend_open() {
        struct backend_args *bargs = 
                (struct backend_args *)args.backend_args;

        bargs->memmap = (char *)malloc(args.size);
        if (bargs->memmap == 0) {
                LOG_ERROR("backend_open: cannot allocate memory\n");
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

        free(bargs->memmap);
        return;
}

/*
 * Called on read requests
 */
int backend_read(struct bdtun_txreq *req) {
        struct backend_args *bargs = 
                (struct backend_args *)args.backend_args;

        memcpy(req->buf, bargs->memmap + req->offset, req->size);
        return 0;
}

/*
 * Called on write requests
 */
int backend_write(struct bdtun_txreq *req) {
        struct backend_args *bargs = 
                (struct backend_args *)args.backend_args;

        memcpy(bargs->memmap + req->offset, req->buf, req->size);
        return 0;
}
