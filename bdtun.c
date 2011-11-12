/*
 * A simple block device to forward requests to userspace
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/cdev.h>
#include <linux/wait.h>
#include <linux/list.h>

#include "bdtun.h"

MODULE_LICENSE("GPL");

static int logical_block_size = 512;
module_param(logical_block_size, int, 0);

static int nsectors = 1024;
module_param(nsectors, int, 0);

static DECLARE_WAIT_QUEUE_HEAD(my_queue);
int flag = 0;

/*
 * BDTun device structure
 */
struct bdtun {
        /* It's a linked list of devices */
        struct list_head list;
        /* character device related */
        struct cdev ch_dev;
        int ch_major;
        int ch_minor;
        /* Buffer for communicating with the userland driver */
        int buffersize;
        char *bufbegin;
        char *bufend;
        char *rdp;
        char *wrp;
        struct fasync_struct *async_queue;
        /* userland sync stuff */
        wait_queue_head_t readq;
        wait_queue_head_t writeq;
        struct semaphore sem;
        /* Block device related stuff*/
        unsigned long bd_size;
        int bd_block_size;
        int bd_nsectors;
        u8 *bd_data;
        struct gendisk *bd_gd;
        /* bd sync stuff */
        spinlock_t bd_lock;
};

/*
 * Control device
 */
struct cdev ctrl_dev;

/*
 * Initialize device list
 */
LIST_HEAD(device_list);

// TODO: dynamically request and create character / block device pairs

// TODO: master character device for controlling devices

/*
 * TODO: master device commands
 * 
 * bdtun create <name> <size>
 * bdtun destroy <name>
 * bdtun resize <name> <size>
 */

/*
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERNEL_SECTOR_SIZE 512

/*
 * Handle an I/O request.
 */
static void bdtun_transfer(struct bdtun *dev, sector_t sector,
                unsigned long nsect, char *buffer, int write) {
        unsigned long offset = sector * logical_block_size;
        unsigned long nbytes = nsect * logical_block_size;

        if ((offset + nbytes) > dev->bd_size) {
                printk (KERN_NOTICE "sbd: Beyond-end write (%ld %ld)\n", offset, nbytes);
                return;
        }
        if (write) {
                flag = 1;
                wake_up_interruptible(&my_queue);
                memcpy(dev->bd_data + offset, buffer, nbytes);
        } else {
                flag = 1;
                wake_up_interruptible(&my_queue);
                memcpy(buffer, dev->bd_data + offset, nbytes);
        }
}

static void bdtun_request(struct request_queue *q) {
        struct request *req;

        req = blk_fetch_request(q);
        while (req != NULL) {
                if (req == NULL || (req->cmd_type != REQ_TYPE_FS)) {
                        printk (KERN_NOTICE "Skip non-CMD request\n");
                        __blk_end_request_all(req, -EIO);
                        continue;
                }
                bdtun_transfer(req->rq_disk->private_data, blk_rq_pos(req), blk_rq_cur_sectors(req),
                                req->buffer, rq_data_dir(req));
                if ( ! __blk_end_request_cur(req, 0) ) {
                        req = blk_fetch_request(q);
                }
        }
}

int bdtun_getgeo (struct block_device *bdev, struct hd_geometry *geo) {
        long size;
        struct bdtun *dev = bdev->bd_disk->private_data;
        
        /* We are a virtual device, so we have to make up something.
         * We claim to have 16 sectors, 4 head, and the appropriate
         * number of cylinders
         */
         size = dev->bd_size*(logical_block_size/KERNEL_SECTOR_SIZE);
         geo->cylinders = (size & 0x3f) >> 6;
         geo->heads = 4;
         geo->sectors = 16;
         geo->start = 0;
         return 0;
}

/*
 * The block device operations structure.
 */
static struct block_device_operations bdtun_ops = {
        .owner = THIS_MODULE,
        .getgeo = bdtun_getgeo
};

/*
 * Character device
 */

int bdtunch_open(struct inode *inode, struct file *file)
{
        // TODO: only allow one process, to open the device
        printk(KERN_DEBUG "bdtun: got device_open on char dev\n");
        return 0;
}

int bdtunch_release(struct inode *inode, struct file *file)
{
        printk(KERN_DEBUG "bdtun: got device_release on char dev\n");
        return 0;
}

ssize_t bdtunch_read(struct file *filp, char *buffer, size_t length, loff_t * offset)
{
        // TODO: wait on a message queue that contains records of what happened with the block device
        // TODO: if there is data, copy it to the process
        int size;
        char * data = "E!\n";
        
        wait_event_interruptible(my_queue, flag != 0);
        flag = 0;
        printk(KERN_DEBUG "bdtun: got device_read on char dev\n");
        size = min(length, (size_t)3);
        copy_to_user(buffer, data, size);
        return size;
}

ssize_t bdtunch_write(struct file *filp, const char *buffer, size_t length, loff_t *offset)
{
        // TODO: put answer into the queue
        // TODO: enaugh concurrency-safety to take advantage
        // of possible master-slave backends (async read / writes)
        printk(KERN_DEBUG "bdtun: got device_write on char dev\n");
        return length;
}

/*
 * Operations on character device
 */
static struct file_operations bdtunch_ops = {
        .read    = bdtunch_read,
        .write   = bdtunch_write,
        .open    = bdtunch_open,
        .release = bdtunch_release
};

/*
 * Device list management auxilliary functions
 */

struct bdtun *bdtun_find_device(char *name) {
        struct list_head *ptr;
        struct bdtun *entry;
        
        list_for_each(ptr, &device_list) {
                entry = list_entry(ptr, struct bdtun, list);
                if (strcmp(entry->bd_gd->disk_name, name) == 0) {
                        return entry;
                }
        }
        return NULL;
}

/*
 *  Commands to manage devices
 */
int bdtun_create(char *name, size_t size) {
        struct bdtun *new;
        struct request_queue *queue;
        int error;
        int bd_major;
        
        new = vmalloc(sizeof (struct bdtun));
        if (new == NULL) {
                return -ENOMEM;
        }
        
        new->bd_block_size = logical_block_size;
        new->bd_nsectors   = nsectors;
        new->bd_size       = nsectors * logical_block_size;
        
        spin_lock_init(&new->bd_lock);

        new->bd_data = vmalloc(new->bd_size);

        if (new->bd_data == NULL) {
                vfree(new);
                return -ENOMEM;
        }
        
        /*
         * Get a request queue.
         */
        queue = blk_init_queue(bdtun_request, &new->bd_lock);
        
        if (queue == NULL) {
                vfree(new->bd_data);
                vfree(new);
                return -ENOMEM;
        }
        
        blk_queue_logical_block_size(queue, logical_block_size);

        /*
         * Get registered.
         */
        bd_major = register_blkdev(0, "bdtun");
        if (bd_major <= 0) {
                printk(KERN_WARNING "bdtun: unable to get major number\n");
                unregister_blkdev(bd_major, "bdtun");
                vfree(new->bd_data);
                vfree(new);
                return -ENOMEM;
        }
        
        /*
         * And the gendisk structure.
         */
        new->bd_gd = alloc_disk(16);
        if (!new->bd_gd) {
                unregister_blkdev(bd_major, "bdtun");
                vfree(new->bd_data);
                vfree(new);
                return -ENOMEM;
        }
        new->bd_gd->major = bd_major;
        new->bd_gd->first_minor = 0;
        new->bd_gd->fops = &bdtun_ops;
        new->bd_gd->private_data = new;
        strcpy(new->bd_gd->disk_name, "bdtuna");
        set_capacity(new->bd_gd, nsectors);
        new->bd_gd->queue = queue;
        add_disk(new->bd_gd);

        /*
         * Initialize character device
         */
        printk(KERN_INFO "bdtun: setting up char device\n");
        // register character device
        cdev_init(&new->ch_dev, &bdtunch_ops);
        new->ch_dev.owner = THIS_MODULE;
        error = cdev_add(&new->ch_dev, MKDEV(240,0),1);
        if (error) {
                printk(KERN_NOTICE "bdtun: error setting up char device\n");
        }
        
        /*
         * Add device to the list
         */
        list_add_tail(&new->list, &device_list);
        
        printk(KERN_NOTICE "bdtun: module init finished\n");
        return 0;
}

int bdtun_remove(char *name) {
        struct bdtun *dev;
        
        /* TODO: find block device in the list */
        dev = bdtun_find_device(name);
        
        if (dev == NULL) {
                return -ENOENT;
        }
        
        /* Destroy block devices */
        printk(KERN_DEBUG "bdtun: removing block device\n");
        unregister_blkdev(dev->bd_gd->major, "bdtun");
        blk_cleanup_queue(dev->bd_gd->queue);
        del_gendisk(dev->bd_gd);
        put_disk(dev->bd_gd);
        vfree(dev->bd_data);
        
        /* Destroy character devices */
        printk(KERN_DEBUG "bdtun: removing char device\n");
        cdev_del(&dev->ch_dev);
        printk(KERN_NOTICE "bdtun: device shutdown finished\n");
        
        /* Unlink and free device structure */
        list_del(&dev->list);
        vfree(dev);
        printk(KERN_NOTICE "bdtun: device removed from list\n");
        
        return 0;
}

int bdtun_info(char *name, struct bdtun_info *device_info) {
        // TODO: fill the device info based on the name
        return 0;
}

char **bdtun_list(void) {
        // TODO: return device names
        return NULL;
}

/*
 * Initialize module
 */
static int __init bdtun_init(void) {
        return bdtun_create("bdtuna", 1024);
}

/*
 * Clean up on module remove
 */
static void __exit bdtun_exit(void) {
        bdtun_remove("bdtuna");
}

module_init(bdtun_init);
module_exit(bdtun_exit);
