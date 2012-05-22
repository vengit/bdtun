#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <argp.h>
#include <bdtun.h>
#include <assert.h>
#include <sys/select.h>

#include "bdtun_backend.h"
#include "config.h"

#define BDTUN_CTRLDEV "/dev/bdtun"

// Global variable for holding configuration, and global state
struct arguments args = {0};

// Exit condition, signal handler uses it
int volatile exitflag = 1;

// These are used by argp
const char *argp_program_version = PACKAGE_STRING;
const char *argp_program_bug_address = PACKAGE_BUGREPORT;

static struct argp_option options[] = {

{0,             0,      0,              0,
        "Backend options", 1},

{0,             0,      0,              0,
        "Device options"},

{"size",        's',    "SIZE",         0,
        "device size in bytes"},

{"block-size",  'b',    "BLOCKSIZE",    0,
        "block size in bytes, default is 512"},

{"keep-dev",    'k',    0,              0,
        "on exit, don't destruct the blockdev"},

{0,             0,      0,              0,
        "Service options"},

{"daemon",      'd',    0,              0,
        "Daemonize the service process"},
 
{"syslog",      'l',    0,              0,
        "Writes log messages to syslog"},
 
{"quiet",       'q',    0,              0,
        "Writes log messages to console, daemon mode can't be set"},


{0}
};

static struct argp_child children[] = {{0, 0, 0, 1}, {0}};

/*
 * Function for argp-style argument parsing
 */
static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
        struct arguments *args = (struct arguments *)state->input;

        uint64_t tmp = 0;
        char *endptr = 0;
        
        switch (key) {
        case 's':
                tmp = strtoll(arg, &endptr, 10);
                if (*endptr) {
                        argp_error(state, "invalid number: %s", arg);
                }
                args->size = tmp;
                break;
        case 'b':
                tmp = strtoll(arg, &endptr, 10);
                if (*endptr) {
                        argp_error(state, "invalid number: %s", arg);
                }
                if (tmp < 512) {
                        argp_error(state,
                        "block size must be at least 512");
                }
                if (tmp > 4096) {
                        argp_error(state,
                        "block size must be at most 4096");
                }
                if (tmp & (tmp - 1)) {
                        argp_error(state,
                        "block size must be a power of two");
                }
                args->blocksize = tmp;
                break;
        case 'k':
                args->keepdev = 1;
                break;
        case 'd':
                args->daemon = 1;
                args->syslog = 1;
                break;
        case 'q':
                args->quiet = 1;
                break;
        case 'l':
                args->syslog = 1;
                break;
        case ARGP_KEY_ARG:
                if (state->arg_num == 0) {
                        if (strlen(arg) >= 32) {
                                argp_error(state, 
                                "tunnel name must be shorter than %d",
                                32);
                        }
                        args->tunnel = arg;
                } else {
                        argp_error(state, "too many arguments");
                }
                break;
        case ARGP_KEY_SUCCESS:
                if (state->arg_num < 1) {
                        argp_error(state, "not enough arguments");
                }
                break;
        case ARGP_KEY_INIT:
                if (children->argp != 0) {
                        state->child_inputs[0] = args->backend_args;
                }
                break;
        }
        
        return 0;
}

// Argp parameters
static struct argp argp = {
        options,
        parse_opt,
        "<tunnel name>",
        "Sets up a backend service for a block device",
};

/*
 * This can be called from backend_init() to set up additional argp
 * parsing. Additional parser can use void *backend_args in the global
 * arguments structure for custom state and configuration.
 */
void set_argp(struct argp *backend_argp) {
        children->argp = backend_argp;
        argp.children = children;
}

/*
 * Daemonizes the process, sets up a new session, and closes std*
 */
static int daemonize(void) {
        pid_t pid, sid;
        
        // Forking
        pid = fork();
        if (pid < 0) {
                LOG_ERROR("daemonize: cannot fork\n");
                return -1;
        } else if (pid > 0) {
                exit(EXIT_SUCCESS);
        }
        PDEBUG("successfully forked\n");
        
        // Settings umask and sid
        umask(0);
        sid = setsid();
        if (sid < 0) {
                LOG_ERROR("daemonize: cannot create new session\n");
                return -1;
        }
        PDEBUG("successfully create new session\n");
        
        // Settings working dir, closing descriptors
        if ((chdir("/")) < 0) {
                LOG_ERROR("daemonize: cannot chdir to /\n");
                return -1;
        }
        PDEBUG("successfully chdir to /\n");
        
        // Closing standard descriptors
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        
        args.pid = getpid();
        return 0;
}

/*
 * Signal handler causing the service stop gracefully on specific
 * signals.
 */
static void signal_handler(int sig) {
        switch(sig) {
                case SIGINT:
                case SIGTERM:
                        exitflag = 0;
                        PDEBUG("exitflag set up\n");
                        break;
        }
}

/*
 * Sets up the signal handler defined before for specific signals
 */
static int setup_signals() {
        sigaddset(&args.sigmask, SIGTERM);
        if (signal(SIGTERM, signal_handler) == SIG_ERR) {
                LOG_ERROR("setup_signals: cannot set up TERM signal\n");
                return -1;
        }
        sigaddset(&args.sigmask, SIGINT);
        if (signal(SIGINT, signal_handler) == SIG_ERR) {
                LOG_ERROR("setup_signals: cannot set up INT signal\n");
                return -1;
        }
        return 0;
}

/*
 * Set up configuration defaults, calls argp parser, and initializes
 * global args structure
 */
static int get_arguments(int argc, char *argv[]) {
        char *devname;
        
        // Default values
        args.blocksize = 512;

        //TODO: factor out this "magic" 42 length contrained by kernel
        devname = (char *)malloc(sizeof(char) * 42);
        if (devname == 0) {
                LOG_ERROR("get_arguments: cannot allocate memory\n");
                return -1;
        }
        
        // Calling argp parser, filling up args global
        if (argp_parse(&argp, argc, argv, 0, 0, &args)) {
                LOG_ERROR(
                "get_arguments: cannot parse arguments (argp_parse)\n");
                return -1;
        }
        
        assert(args.tunnel != 0);
        
        // Tunnel naming convention: /dev/{name}_tun
        strcpy(devname, "/dev/");
        strcpy(devname + 5, args.tunnel);
        strcat(devname + 5, "_tun");
        
        args.devname = devname;
        
        assert(args.devname != 0);
        
        return 0;
}

/*
 * Sets up a new bdtun block device if neccessary, or sanity-checks an
 * existing one with the same name
 */
static int blockdev_init() {
        struct bdtun_info info;
        int ret;
        int ctrldev;

        assert(args.tunnel != 0);
        
        // Open control device
        ctrldev = open(BDTUN_CTRLDEV, O_RDWR);
        if (ctrldev < 0) {
                LOG_ERROR(
                "blockdev_init: could not open control device %s\n",
                        BDTUN_CTRLDEV);
                return -1;
        }
        PDEBUG("Main control device opened\n");
        
        // Get info about tunnel given by name at command line
        ret = bdtun_info(ctrldev, args.tunnel, &info);
        if (ret) {
                // This is the case, when tunnel has not created yet,
                // let's create it.
                PDEBUG("Cannot query device info, trying to create\n");
                if (!args.size) {
                        LOG_ERROR(
                        "blockdev_init: size must be given for creation\n");
                        return -1;
                }
                if (args.size % args.blocksize) {
                        LOG_ERROR("blockdev_init: size of %" PRIu64 " is not a multiple of "
                               "blocksize of %" PRIu64 "\n",
                               args.size, args.blocksize);
                        return -1;
                }
                
                // Create call
                ret = bdtun_create(ctrldev, args.tunnel, args.blocksize, args.size, args.capabilities);
                if (ret < 0) {
                        LOG_ERROR("blockdev_init: could not query or create tunnel %s\n", args.tunnel);
                        return -1;
                }
                PDEBUG("blockdev successfully created\n");
                args.create_tun = 1;
                
                // Query back tunnel
                ret = bdtun_info(ctrldev, args.tunnel, &info);
                if (ret) {
                        LOG_ERROR(
                        "blockdev_init: cannot query or create tunnel %s\n",
                        args.tunnel);
                        bdtun_remove(ctrldev, args.tunnel);
                        return -1;
                }
                PDEBUG("created blockdev successfully query back\n");
        } else {
                // This is the case, when tunnel is already exist, just
                // sanity-check it
                if (!args.size) {
                        args.size = info.bd_size;
                }
                if (!args.blocksize) {
                        args.blocksize = info.bd_block_size;
                }
                if (!args.capabilities) {
                        args.capabilities = info.capabilities;
                }
        }
        
        // At this point, all parameters must comply with command-line
        // parameters
        if (args.blocksize != info.bd_block_size) {
                LOG_ERROR("blockdev_init: tunneled device block size of %" PRIu64
                " doesn't match given block size of %" PRIu64 "\n",
                info.bd_block_size, args.blocksize);
                if (args.create_tun) bdtun_remove(ctrldev, args.tunnel);
                return -1;
        }

        if (args.size != info.bd_size) {
                LOG_ERROR("blockdev_init: tunneled device size of %" PRIu64
                " doesn't match given size of %" PRIu64 "\n",
                info.bd_size, args.size);
                if (args.create_tun) bdtun_remove(ctrldev, args.tunnel);
                return -1;
        }
        
        if (args.capabilities != info.capabilities) {
                LOG_ERROR(
                "blockdev_init: tunneled device capabilities %d doesn't match given capabilities %d\n",
                info.capabilities, args.capabilities);
                if (args.create_tun) bdtun_remove(ctrldev, args.tunnel);
                return -1;
        }
        
        close(ctrldev);

        return 0;
}

/*
 * Destructs the block device, if it was created by this service
 */
static void blockdev_deinit() {
        int ctrldev;
        
        if (args.create_tun && !args.keepdev) {
                /* Open control device */
                ctrldev = open(BDTUN_CTRLDEV, O_RDWR);
                if (ctrldev < 0) {
                        LOG_ERROR(
                        "blockdev_deinit: could not open control device %s\n",
                                BDTUN_CTRLDEV);
                        return;
                }
                bdtun_remove(ctrldev, args.tunnel);
                PDEBUG("blockdev successfully removed\n");
                close(ctrldev);
        }
}

/*
 * Opens the control chardevice for the blockdev
 */
static int blockdev_open() {
        assert(args.devname != 0);
        
        if((args.bdtunchdev = open(args.devname, O_RDWR)) < 0) {
                LOG_ERROR(
                "blockdev_open: unable to open bdtun character device file %s\n",
                        args.devname);
                return -1;
        }
        PDEBUG("successfully open blockdev control device\n");
        return 0;
}

/*
 * Closes the control chardevice for the blockdev
 */
static int blockdev_close() {
        return close(args.bdtunchdev);
}

/*
 * Serving loop, sets up signal mask, and uses pselect for
 * signal-sensitive blocking. Calls backend_write and backend_read
 * respectively.
 */
static int event_loop() {
        struct bdtun_txreq req;
        int ret;
        fd_set fds;
	sigset_t orig_mask;

        assert(args.bdtunchdev >= 0);
        
        // Blocks all relevant signals, which can modify exitflag
        if (sigprocmask(SIG_BLOCK, &args.sigmask, &orig_mask) < 0) {
                LOG_ERROR("event_loop: cannot block signals\n");
                return -1;
        }
        
        LOG_INF("Successfully initialized, serving\n");
                
        while (exitflag) {
                // pselect setup
                FD_ZERO(&fds);
                FD_SET(args.bdtunchdev, &fds);
                
                PDEBUG("blocking on pselect\n");
                ret = pselect(args.bdtunchdev + 1, &fds, 0, 0, 0, &orig_mask);
                if (ret < 0) {
                        if (errno == EINTR) {
                                // Any other signal
                                PDEBUG("Signal\n");
                                continue;
                        } else {
                                // Error condition, which isn't caused by signals
                                LOG_ERROR(
                                "event_loop: cannot pselect, error code: %d",
                                        errno);
                                return -1;
                        }
                }
                PDEBUG("out of pselect\n");
                if (FD_ISSET(args.bdtunchdev, &fds)) {
                        
                        // Read bio request from kernel-space
                        if ((ret = bdtun_read_request(args.bdtunchdev, &req))) {
                                LOG_ERROR(
                                "event_loop: cannot get request from device: %d\n",
                                        ret);
                                return -1;
                        }

                        if (req.flags & REQ_WRITE) {
                                // It's a write request
                                if ((ret = backend_write(&req)) < 0) {
                                        LOG_ERROR(
                                        "event_loop: cannot write data to backend: %d\n",
                                                ret);
                                        return -1;
                                }
                        } else {
                                // It's a read request
                                if ((ret = backend_read(&req)) < 0) {
                                        LOG_ERROR(
                                        "event_loop: cannot read data from backend: %d\n",
                                                ret);
                                        return -1;
                                }
                        }
                        
                        // Completing request towards kernel space
                        ret = bdtun_complete_request(args.bdtunchdev,
                                                        &req);
                        if (ret) {
                                LOG_ERROR(
                                "event_loop: unable to signal completion on write: %d\n",
                                        ret);
                                return -1;
                        }
                }
        }
        
        LOG_INF("Got a signal, shutting down...\n");
        
        return 0;
}

/*
 * Entry point, this will set up backend and blockdev, sets up signals,
 * syslog, daemonizes itself if neccessary, and calls the event loop for
 * handling IO requests
 */
int main(int argc, char *argv[]) {
        assert(args.quiet == 0);
        
        sigemptyset(&args.sigmask);
        
        setlogmask (LOG_UPTO (LOG_INFO));
        openlog(backend_program_name, LOG_CONS | LOG_PID | LOG_NDELAY, LOG_DAEMON);
        
        if (backend_init()) {
                LOG_ERROR("Cannot initialize backend\n");
                exit(EXIT_FAILURE);
        }
        PDEBUG("backend init done\n");
        
        if (get_arguments(argc, argv)) {
                LOG_ERROR("Cannot parse arguments\n");
                exit(EXIT_FAILURE);
        }
        PDEBUG("argument parse done\n");

        LOG_INF("Starting up service\n");
        
        if (blockdev_init()) {
                LOG_ERROR("Cannot initialize blockdev\n");
                exit(EXIT_FAILURE);
        }
        PDEBUG("blockdev init done\n");
        
        if (blockdev_open()) {
                LOG_ERROR("Cannot open blockdev control device\n");
                blockdev_deinit();
                backend_deinit();
                exit(EXIT_FAILURE);
        }
        PDEBUG("blockdev open done\n");
        
        if (backend_open()) {
                LOG_ERROR("Cannot open backend\n");
                blockdev_close();
                blockdev_deinit();
                backend_deinit();
                exit(EXIT_FAILURE);
        }
        PDEBUG("backend open done\n");
        
        if (args.daemon) {
                args.quiet = 1;
                if (daemonize()) {
                        LOG_ERROR("Cannot daemonize\n");
                        backend_close();
                        blockdev_close();
                        blockdev_deinit();
                        backend_deinit();
                        exit(EXIT_FAILURE);
                }
                PDEBUG("daemonization\n");
        } else {
                args.pid = getpid();
                PDEBUG("not daemonizing\n");
        }
        PDEBUG("daemonization (or getpid) done\n");
        
        if (setup_signals()) {
                LOG_ERROR("Cannot set up signals\n");
                backend_close();
                blockdev_close();
                blockdev_deinit();
                backend_deinit();
                exit(EXIT_FAILURE);
        }
        PDEBUG("signal setup done\n");
        
        if (event_loop()) {
                LOG_ERROR(
                        "Event loop left with fatal error condition\n");
        }
        PDEBUG("left event loop\n");
        
        backend_close();
        PDEBUG("backend closed\n");
        
        blockdev_close();
        PDEBUG("blockdev closed\n");
        
        blockdev_deinit();
        PDEBUG("blockdev deinitialized\n");
        
        backend_deinit();
        PDEBUG("backend deinitialized\n");

        LOG_INF("Stopped service\n");
        
        closelog();
        
        exit(EXIT_SUCCESS);
}
