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

MODULE_LICENSE("GPL");

#define BD_MAJOR 0
#define CH_MAJOR 240
#define CH_MINOR 0

static int bd_major_num = BD_MAJOR;
module_param(bd_major_num, int, BD_MAJOR);

static int ch_major_num = CH_MAJOR;
module_param(ch_major_num, int, CH_MAJOR);

static int logical_block_size = 512;
module_param(logical_block_size, int, 0);

static int nsectors = 1024; /* How big the drive is */
module_param(nsectors, int, 0);

struct cdev mycdev;

// TODO: is this wait queue OK?
static DECLARE_WAIT_QUEUE_HEAD(my_queue);
int flag = 0;

/*
 * MongoBD device structure
 */
struct mongobd {
        wait_queue_head_t readq;
        wait_queue_head_t writeq;
        int buffersize;
        char *bufbegin;
        char *bufend;
        char *rdp;
        char *wrp;
        struct fasync_struct *async_queue;
        struct semaphore sem;
        struct cdev cdev;
};

// TODO: dynamically request and create character / block device pairs

// TODO: master character device for controlling devices

/*
 * TODO: master device commands
 * 
 * mongobd create <name> <size>
 * mongobd destroy <name>
 * mongobd resize <name> <size>
 *  

/*
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERNEL_SECTOR_SIZE 512

/*
 * Our request queue.
 */
static struct request_queue *Queue;

/*
 * The internal representation of our device.
 */
static struct sbd_device {
        unsigned long size;
        spinlock_t lock;
        u8 *data;
        struct gendisk *gd;
} Device;

/*
 * Handle an I/O request.
 */
static void sbd_transfer(struct sbd_device *dev, sector_t sector,
                unsigned long nsect, char *buffer, int write) {
        unsigned long offset = sector * logical_block_size;
        unsigned long nbytes = nsect * logical_block_size;

        if ((offset + nbytes) > dev->size) {
                printk (KERN_NOTICE "sbd: Beyond-end write (%ld %ld)\n", offset, nbytes);
                return;
        }
        if (write) {
                // TODO: put a message onto the queue for the char device
                flag = 1;
                wake_up_interruptible(&my_queue);
                memcpy(dev->data + offset, buffer, nbytes);
        } else {
                // TODO: put a message onto the queue for the char device
                flag = 1;
                wake_up_interruptible(&my_queue);
                memcpy(buffer, dev->data + offset, nbytes);
        }
}

static void sbd_request(struct request_queue *q) {
        struct request *req;

        req = blk_fetch_request(q);
        while (req != NULL) {
                if (req == NULL || (req->cmd_type != REQ_TYPE_FS)) {
                        printk (KERN_NOTICE "Skip non-CMD request\n");
                        __blk_end_request_all(req, -EIO);
                        continue;
                }
                sbd_transfer(&Device, blk_rq_pos(req), blk_rq_cur_sectors(req),
                                req->buffer, rq_data_dir(req));
                if ( ! __blk_end_request_cur(req, 0) ) {
                        req = blk_fetch_request(q);
                }
        }
}

int sbd_getgeo(struct block_device * block_device, struct hd_geometry * geo) {
        long size;

        /* We have no real geometry, of course, so make something up. */
        size = Device.size * (logical_block_size / KERNEL_SECTOR_SIZE);
        geo->cylinders = (size & ~0x3f) >> 6;
        geo->heads = 4;
        geo->sectors = 16;
        geo->start = 0;
        return 0;
}

/*
 * The block device operations structure.
 */
static struct block_device_operations sbd_ops = {
                .owner  = THIS_MODULE,
                .getgeo = sbd_getgeo
};

/*
 * Character device
 */

int device_open(struct inode *inode, struct file *file)
{
        // TODO: only allow one process, to open the device
        printk(KERN_DEBUG "mongobd: got device_open on char dev\n");
        return 0;
}

int device_release(struct inode *inode, struct file *file)
{
        printk(KERN_DEBUG "mongobd: got device_release on char dev\n");
        return 0;
}

ssize_t device_read(struct file *filp, char *buffer, size_t length, loff_t * offset)
{
        // TODO: wait on a message queue that contains records of what happened with the block device
        // TODO: if there is data, copy it to the process
        int size;
        char * data = "E!\n";
        
        wait_event_interruptible(my_queue, flag != 0);
        flag = 0;
        printk(KERN_DEBUG "mongobd: got device_read on char dev\n");
        size = min(length, 3);
        copy_to_user(buffer, data, size);
        return size;
}

ssize_t device_write(struct file *filp, const char *buffer, size_t length, loff_t *offset)
{
        // TODO: FAIL.
        printk(KERN_DEBUG "mongobd: got device_write on char dev\n");
        return length;
}

static struct file_operations fops = {
        .read = device_read,
        .write = device_write,
        .open = device_open,
        .release = device_release
};

static int char1_init(void)
{
        int error;
        printk(KERN_INFO "mongobd: setting up char device\n");
        //A device regisztrálása
        cdev_init(&mycdev, &fops);
        mycdev.owner=THIS_MODULE;
        error=cdev_add(&mycdev,MKDEV(CH_MAJOR,CH_MINOR),1);
        if (error)
        {
                printk(KERN_NOTICE "mongobd: error setting up char device\n");
        }
        return 0;
}

static void char1_cleanup(void)
{
        printk(KERN_DEBUG "mongobd: removing char device\n");
        cdev_del(&mycdev);
}


static int __init sbd_init(void) {
        /*
         * Set up our internal device.
         */
        Device.size = nsectors * logical_block_size;
        spin_lock_init(&Device.lock);
        Device.data = vmalloc(Device.size);
        if (Device.data == NULL)
                return -ENOMEM;
        /*
         * Get a request queue.
         */
        Queue = blk_init_queue(sbd_request, &Device.lock);
        if (Queue == NULL) {
                vfree(Device.data);
                return -ENOMEM;
        }
        blk_queue_logical_block_size(Queue, logical_block_size);
        /*
         * Get registered.
         */
        bd_major_num = register_blkdev(bd_major_num, "sbd");
        if (bd_major_num <= 0) {
                printk(KERN_WARNING "sbd: unable to get major number\n");
                unregister_blkdev(bd_major_num, "sbd");
                vfree(Device.data);
                return -ENOMEM;
        }
        /*
         * And the gendisk structure.
         */
        Device.gd = alloc_disk(16);
        if (!Device.gd) {
                unregister_blkdev(bd_major_num, "sbd");
                vfree(Device.data);
                return -ENOMEM;
        }
        Device.gd->major = bd_major_num;
        Device.gd->first_minor = 0;
        Device.gd->fops = &sbd_ops;
        Device.gd->private_data = &Device;
        strcpy(Device.gd->disk_name, "sbd0");
        set_capacity(Device.gd, nsectors);
        Device.gd->queue = Queue;
        add_disk(Device.gd);

        /*
         * Initialize character device
         */
        return char1_init();
}

static void __exit sbd_exit(void)
{
        // Destroy character device
        char1_cleanup();
        
        // Destroy vlock device
        del_gendisk(Device.gd);
        put_disk(Device.gd);
        unregister_blkdev(bd_major_num, "sbd");
        blk_cleanup_queue(Queue);
        vfree(Device.data);
}

module_init(sbd_init);
module_exit(sbd_exit);
