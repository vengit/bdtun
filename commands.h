/*
 * Device management commands
 */
#define BDTUN_COMM_CREATE_DEVICE 0
#define BDTUN_COMM_REMOVE_DEVICE 1
#define BDTUN_LIST_DEVICES 2
#define BDTUN_DEVICE_INFO 3

/*
 * Block tunnel transfer requests
 */
struct bdtun_txreq {
	unsigned long sector;
	unsigned long nsect;
	int write;
	char *buffer;
};
