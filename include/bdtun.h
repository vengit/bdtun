#ifndef __BDTUN_H
#define __BDTUN_H

#define BDTUN_REQ_WRITE               (1 << 1)
#define BDTUN_REQ_FAILFAST_DEV        (1 << 2)
#define BDTUN_REQ_FAILFAST_TRANSPORT  (1 << 3)
#define BDTUN_REQ_FAILFAST_DRIVER     (1 << 4)
#define BDTUN_REQ_SYNC                (1 << 5)
#define BDTUN_REQ_META                (1 << 6)
#define BDTUN_REQ_PRIO                (1 << 7)
#define BDTUN_REQ_DISCARD             (1 << 8)
#define BDTUN_REQ_NOIDLE              (1 << 9)

#define BDTUN_REQ_FAILFAST_MASK \
        (BDTUN_REQ_FAILFAST_DEV | BDTUN_REQ_FAILFAST_TRANSPORT | BDTUN_REQ_FAILFAST_DRIVER)
#define BDTUN_REQ_COMMON_MASK \
        (BDTUN_REQ_WRITE | BDTUN_REQ_FAILFAST_MASK | BDTUN_REQ_SYNC | BDTUN_REQ_META | BDTUN_REQ_PRIO | \
         BDTUN_REQ_DISCARD | BDTUN_REQ_NOIDLE | BDTUN_REQ_FLUSH | BDTUN_REQ_FUA | BDTUN_REQ_SECURE)
#define BDTUN_REQ_CLONE_MASK          BDTUN_REQ_COMMON_MASK

#define BDTUN_REQ_RAHEAD              (1 << 10)
#define BDTUN_REQ_THROTTLED           (1 << 11)

#define BDTUN_REQ_SORTED              (1 << 12)
#define BDTUN_REQ_SOFTBARRIER         (1 << 13)
#define BDTUN_REQ_FUA                 (1 << 14)
#define BDTUN_REQ_NOMERGE             (1 << 15)
#define BDTUN_REQ_STARTED             (1 << 16)
#define BDTUN_REQ_DONTPREP            (1 << 17)
#define BDTUN_REQ_QUEUED              (1 << 18)
#define BDTUN_REQ_ELVPRIV             (1 << 19)
#define BDTUN_REQ_FAILED              (1 << 20)
#define BDTUN_REQ_QUIET               (1 << 21)
#define BDTUN_REQ_PREEMPT             (1 << 22)
#define BDTUN_REQ_ALLOCED             (1 << 23)
#define BDTUN_REQ_COPY_USER           (1 << 24)
#define BDTUN_REQ_FLUSH               (1 << 25)
#define BDTUN_REQ_FLUSH_SEQ           (1 << 26)
#define BDTUN_REQ_IO_STAT             (1 << 27)
#define BDTUN_REQ_MIXED_MERGE         (1 << 28)
#define BDTUN_REQ_SECURE              (1 << 29)

#ifdef __cplusplus
extern "C" {
#endif

#undef PDEBUG
#ifdef BDTUN_DEBUG
#  ifdef __KERNEL__
#    define PDEBUG(fmt, args...) printk(KERN_WARNING "bdtun: " fmt, ## args)
#  else
#    define PDEBUG(fmt, args...) printf(fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...)
#endif

/*
 * Size constants
 */
#define BDTUN_DEVNAMES 32
#define BDTUN_RESPONSE_SIZE 1024
#define BDTUN_BD_MINORS 64

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
        uintptr_t id;
        unsigned long flags;
        unsigned long offset;
        unsigned long size;
};

#define BDTUN_TXREQ_HEADER_SIZE sizeof(struct bdtun_txreq)

/*
 * Device capabilities
 */
#define BDTUN_FLUSH   1
#define BDTUN_FUA     2
#define BDTUN_DISCARD 4
#define BDTUN_SECURE  8

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
        int capabilities;
};

struct bdtun_ctrl_command {
        char command;
        union {
                struct {
                        uint64_t blocksize;
                        uint64_t size;
                        int capabilities;
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

int bdtun_read_request(int fd, struct bdtun_txreq *req);

int bdtun_set_request(int fd, struct bdtun_txreq *req);

void *bdtun_mmap_request(int fd, struct bdtun_txreq *req);

ssize_t bdtun_get_request_data(int fd, struct bdtun_txreq *req, void *buf);

ssize_t bdtun_send_request_data(int fd, struct bdtun_txreq *req, void *buf);

ssize_t bdtun_complete_request(int fd, struct bdtun_txreq *req, void *buf);

ssize_t bdtun_fail_request(int fd, struct bdtun_txreq *req);

int bdtun_create(int fd, const char *name, uint64_t blocksize, uint64_t size, int capabilities);

int bdtun_resize(int fd, const char *name, uint64_t size);

int bdtun_remove(int fd, const char *name);

int bdtun_info(int fd, const char *name, struct bdtun_info *info);

int bdtun_list(int fd, size_t offset, size_t maxdevices, char ***buf);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // __BDTUN_H
