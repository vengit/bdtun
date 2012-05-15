#ifndef _BDTUN_BACKEND_H
#define _BDTUN_BACKEND_H

#include <stdio.h>
#include <argp.h>
#include <bdtun.h>
#include <syslog.h>

#define BDTUN_CTRLDEV "/dev/bdtun"

struct arguments {
        char *tunnel;
        char *devname;
        int create_tun;
        int bdtunchdev;
        int daemon;
        int syslog;
        int quiet;
        int capabilities;
        uint64_t size;
        uint64_t blocksize;
        int pid;
        sigset_t sigmask;
        void *backend_args;
};

extern struct arguments args;

#define LOG_ERROR(fmt, vargs...) if (args.syslog) { syslog(LOG_CRIT, fmt, ## vargs); };\
        if (!args.quiet) { fprintf(stderr, fmt, ## vargs); }

extern char * backend_program_name;

void set_argp(struct argp *backend_argp);

/*
 * These functions should be implemented by the backend
 */
int backend_init();
void backend_deinit();
int backend_open();
void backend_close();
int backend_read(struct bdtun_txreq *req);
int backend_write(struct bdtun_txreq *req);

#endif
