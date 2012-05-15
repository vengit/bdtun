#include <argp.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "bdtun_backend.h"

struct backend_args {
        char *testopt;
};

static struct argp_option options[] = {
{"testopt",     't', "TESTOPT",           0, "Test option for backend"},
{0}
};

char * backend_program_name = "dummy_backend";

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

void backend_deinit() {
        return;
}

int backend_open() {
        char* testopt = ((struct backend_args *)args.backend_args)->testopt;
        if (testopt != 0) {
                PDEBUG("testopt value: %s\n", testopt);
        }
        return 0;
}

void backend_close() {
        return;
}

int backend_read(struct bdtun_txreq *req) {
        return 0;
}

int backend_write(struct bdtun_txreq *req) {
        return 0;
}
