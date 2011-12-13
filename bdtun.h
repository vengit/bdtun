#include <linux/blk_types.h>

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
