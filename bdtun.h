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
	char *bd_name;
	int bd_major;
	int bd_minor;
	uint64_t bd_size;
	size_t bd_block_size;
	char *ch_name;
	int ch_major;
	int ch_minor;
};

int bdtun_read_request(int fd, struct bdtun_txreq *rq);

int bdtun_complete_request(int fd, struct bdtun_txreq *req);

int bdtun_create(int fd, char *name, uint64_t blocksize, uint64_t size);

int bdtun_resize(int fd, char *name, uint64_t blocksize, uint64_t size);

int bdtun_remove(int fd, char *name);

int bdtun_info(int fd, char *name, struct bdtun_info *info);

char **bdtun_list(int fd, size_t offset, size_t maxdevices);

#ifdef __cplusplus
}
#endif
