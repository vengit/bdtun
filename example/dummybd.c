#include <argp.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "bdtun_backend.h"

// Name of this backend
char * backend_program_name = "dummybd";

// Backend-specific global state and configuration
struct backend_args {
        char *testopt;
};

// Backend-specific argp settings
static struct argp_option options[] = {
{"testopt",     't', "TESTOPT",           0, "Test option for backend"},
{0}
};

static int parse_opt(int key, char *arg, struct argp_state *state)
{
        struct backend_args *args = (struct backend_args *)state->input;

        switch (key) {
        case 't':
                args->testopt = arg;
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
        
        assert(((struct backend_args *)args.backend_args)->testopt == 0);
        
        return 0;
}

/*
 * Do some deinitialization, if needed
 */
void backend_deinit() {
        return;
}

/*
 * Opens the backend
 */
int backend_open() {
        char* testopt = ((struct backend_args *)args.backend_args)->testopt;
        if (testopt != 0) {
                PDEBUG("testopt value: %s\n", testopt);
        }
        return 0;
}

/*
 * Closes the backend
 */
void backend_close() {
        return;
}

/*
 * Called on read requests
 */
int backend_read(struct bdtun_txreq *req) {
        return 0;
}

/*
 * Called on write requests
 */
int backend_write(struct bdtun_txreq *req) {
        return 0;
}
