/*
 * Device management commands
 */
#define BDTUN_COMM_CREATE_DEVICE 0
#define BDTUN_COMM_REMOVE_DEVICE 1
#define BDTUN_LIST_DEVICES 2
#define BDTUN_DEVICE_INFO 3
#define BDTUN_RQ_READ 4
#define BDTUN_RQ_WRITE 5
#define BDTUN_RQ_COMPLETE 6

/*
 * Block tunnel transfer requests
 */
struct bdtun_txreq {
	unsigned char write;
	unsigned long offset;
	unsigned long size;
};
