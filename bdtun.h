#include <linux/blk_types.h>

#ifdef __cplusplus
extern "C" {
#endif

#undef PDEBUG
#ifdef BDTUN_DEBUG
#  ifdef __KERNEL__
#    define PDEBUG(fmt, args...) printk(KERN_DEBUG "bdtun: " fmt, ## args)
#  else
#    define PDEBUG(fmt, args...) printf(fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...)
#endif

/*
 * Device management commands
 */
#define BDTUN_COMM_CREATE 0
#define BDTUN_COMM_REMOVE 1
#define BDTUN_COMM_LIST 2
#define BDTUN_COMM_INFO 3
#define BDTUN_COMM_RESIZE 4
#define BDTUN_RQ_READ 5
#define BDTUN_RQ_WRITE 6
#define BDTUN_RQ_COMPLETE 7

/*
 * Block tunnel transfer requests
 */
struct bdtun_txreq {
        unsigned long flags;
        unsigned long offset;
        unsigned long size;
        char *buf;
};

#define BDTUN_TXREQ_HEADER_SIZE sizeof(struct bdtun_txreq)-sizeof(char *)

/*
 * Information on a device pair
 */
struct bdtun_info {
        uint64_t bd_size;
        uint64_t bd_block_size;
        int bd_major;
        int bd_minor;
        int ch_major;
        int ch_minor;
};

struct bdtun_ctrl_command {
        char command;
        union {
                struct {
                        uint64_t blocksize;
                        uint64_t size;
                        char name[32];
                } create;
                struct {
                        char name[32];
                } info;
                struct {
                        size_t offset;
                        size_t maxdevices;
                } list;
                struct {
                        uint64_t blocksize;
                        uint64_t size;
                        char name[32];
                } resize;
                struct {
                        char name[32];
                } remove;
        };
};

// TODO: investigate various sizes
#define BDTUN_COMM_CREATE_SIZE sizeof(struct bdtun_ctrl_command)
#define BDTUN_COMM_INFO_SIZE   sizeof(struct bdtun_ctrl_command)
#define BDTUN_COMM_LIST_SIZE   sizeof(struct bdtun_ctrl_command)
#define BDTUN_COMM_REMOVE_SIZE sizeof(struct bdtun_ctrl_command)
#define BDTUN_COMM_RESIZE_SIZE sizeof(struct bdtun_ctrl_command)

int bdtun_read_request(int fd, struct bdtun_txreq *rq);

int bdtun_complete_request(int fd, struct bdtun_txreq *req);

int bdtun_create(int fd, const char *name, uint64_t blocksize, uint64_t size);

int bdtun_resize(int fd, const char *name, uint64_t blocksize, uint64_t size);

int bdtun_remove(int fd, const char *name);

int bdtun_info(int fd, const char *name, struct bdtun_info *info);

char **bdtun_list(int fd, size_t offset, size_t maxdevices);

#ifdef __cplusplus
}
#endif
