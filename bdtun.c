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
#include <linux/semaphore.h>
#include <linux/workqueue.h>

#include "bdtun.h"
#include "commands.h"

MODULE_LICENSE("GPL");

#define COMM_BUFFER_SIZE 102400

/*
 * A work item for our work queue.
 * We need a work queue to get the bio-s out of
 * the interrupt context, and be able to sleep
 * while processing them.
 */
struct bdtun_work {
        struct bio *bio;
        struct bdtun *dev;
        struct work_struct work;        
};

/*
 * A list in wich unprocessed bio-s are contained. The work items
 * in the work queue put bio-s into these list items and chain them
 * up to form a linked list. The list is processed upon reads and writes
 * on the char device. 
 */
struct bdtun_bio_list_entry {
        struct list_head list;
        struct bio *bio;
};

/*
 * BDTun device structure
 */
struct bdtun {
        /* It's a linked list of devices */
        struct list_head list;
        
        /* character device related */
        struct cdev ch_dev;
        int ch_major;
        
        struct list_head bio_list;
        wait_queue_head_t bio_list_queue;
        struct semaphore bio_list_queue_sem;
        
        /* userland sync stuff */
        int ch_client_count;
        struct semaphore sem; /* semaphore for rarely modified fields */
        struct workqueue_struct *wq;

        /* Block device related stuff*/
        unsigned long bd_size;
        int bd_block_size;
        int bd_nsectors;
        struct gendisk *bd_gd;
};

/*
 * Control device
 */
struct cdev ctrl_dev;

/*
 * Initialize device list
 */
LIST_HEAD(device_list);

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
 * Do the work: process an I/O request async. 
 * Put this bio into a linked list, so the char device can pick them up.
 */
static void bdtun_do_work(struct work_struct *work) {
        struct bdtun_work *w = container_of(work, struct bdtun_work, work);
        
        // Constant TODO: We need to free this. Check twice. Seriously.
        struct bdtun_bio_list_entry *new = vmalloc(sizeof(struct bdtun_bio_list_entry));
        
        if (!new) {
                // TODO: is this right? Test with || 1
                bio_endio(w->bio, -EIO);
                kfree(w);
                return;
        }
        
        // TODO: we will need to look bio-s up very frequently, so we
        // better use some pointer-like stuff to identify bio-s even
        // (or especially) towards the charater device.
        
        while(down_interruptible(&w->dev->bio_list_queue_sem) != 0);
        list_add_tail(&new->list, &w->dev->bio_list);
        up(&w->dev->bio_list_queue_sem);
        
        // TODO: need to grab a lock here maybe?
        // bio_endio(w->bio, 0);
        
        // TODO: It Seeeeems ok to do this here. Find out is it really ok.
        kfree(w);
}

/*
 * Request processing
 */
static int bdtun_make_request(struct request_queue *q, struct bio *bio) {
        struct bdtun *dev = q->queuedata;
        struct bdtun_work *w = kmalloc(sizeof(struct bdtun_work), GFP_ATOMIC);
        
        if (!w) {
                // TODO: will this be a retry or what?
                // bio_endio(bio, -EIO); <- do we need this?
                return -EIO;
        }
        
        INIT_WORK(&w->work, bdtun_do_work);
        
        w->bio = bio;
        w->dev = dev;
        
        queue_work(dev->wq, &w->work);
        
        return 0;
}

/*
 *  Get the "drive geometry"
 */
static int bdtun_getgeo (struct block_device *bdev, struct hd_geometry *geo) {
        long size;
        struct bdtun *dev = bdev->bd_disk->private_data;
        
        /* We are a virtual device, so we have to make up something.
         * We claim to have 16 sectors, 4 head, and the appropriate
         * number of cylinders
         */
         size = dev->bd_size*(dev->bd_block_size/KERNEL_SECTOR_SIZE);
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

static int bdtunch_open(struct inode *inode, struct file *file) {
        struct bdtun *dev = file->private_data;
        
        printk(KERN_DEBUG "bdtun: got device_open on char dev\n");
        
        if (down_interruptible(&dev->sem)) {
                return -ERESTARTSYS;
        }
        
        if (dev->ch_client_count > 0) {
                return -EBUSY;
        }
        
        dev->ch_client_count++;
        
        up(&dev->sem);
        
        return 0;
}

static int bdtunch_release(struct inode *inode, struct file *file) {
        struct bdtun *dev = file->private_data;
        
        printk(KERN_DEBUG "bdtun: got device_release on char dev\n");
        
        if (down_interruptible(&dev->sem)) {
                return -ERESTARTSYS;
        }
        
        dev->ch_client_count--;

        up(&dev->sem);
        
        return 0;
}

/*
 * Takes a bio, and sends it to the char device.
 * We don't know if the bio will complete at this point.
 * Writes to the char device will complete the bio-s.
 */
static ssize_t bdtunch_read(struct file *filp, char *buf, size_t count, loff_t *f_pos) {
        struct bdtun *dev = filp->private_data;
        int res;
        
        printk(KERN_DEBUG "bdtun: got device_read on char dev\n");
        
        if (down_interruptible(&dev->bio_list_queue_sem)) {
                return -ERESTARTSYS;
        }
        
        // Testing: write something silly
        res = min((int)count, 3);
        memcpy(buf, "E!\n", res);
        
        printk(KERN_DEBUG "bdtun: sent something to the char device.\n");
        
        // TODO: we will need to queue the sent bio-s in a
        // different list.
        
        // TODO: we need to signal that somethin is in the queue.
        // perhaps an other semafor? reader-writer stuff how-to needed.
        
        up(&dev->bio_list_queue_sem);
        
        return 0;
}

static ssize_t bdtunch_write(struct file *filp, const char *buf, size_t count, loff_t *offset) {
        struct bdtun *dev = filp->private_data;
        struct bdtun_bio_list_entry *entry;
        
        if (down_interruptible(&dev->bio_list_queue_sem)) {
                return -ERESTARTSYS;
        }
        
        // Grab a bio, complete it, and blog about it.
        entry = list_entry(&dev->bio_list, struct bdtun_bio_list_entry, list);
        bio_endio(entry->bio, 0);
        printk(KERN_DEBUG "bdtun: successfully finished a bio.\n");
        
        up(&dev->bio_list_queue_sem);
        
        return 0;
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

static struct bdtun *bdtun_find_device(char *name) {
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
static int bdtun_create(char *name, int logical_block_size, size_t size) {
        struct bdtun *new;
        struct request_queue *queue;
        int error;
        int bd_major;
        int ch_major;
        char charname[36];
        char qname[34];
        
        /*
         * Set up character device and workqueue name
         */
        strncpy(charname, name, 32);
        strcat(charname, "_tun");
        strncpy(qname, name, 32);
        strcat(charname, "_q");
        
        /*
         * Allocate device structure
         */
        new = vmalloc(sizeof (struct bdtun));
        if (new == NULL) {
                return -ENOMEM;
        }
        
        /*
         * Set client count for char device
         */
        new->ch_client_count = 0;
        
        /*
         * Determine device size
         */
        new->bd_block_size = logical_block_size;
        new->bd_nsectors   = size / logical_block_size; // Size is just an approximate.
        new->bd_size       = new->bd_nsectors * logical_block_size;
        
        /*
         * Semaphores stuff like that 
         */
        sema_init(&new->sem, 1);
        sema_init(&new->bio_list_queue_sem, 1);
        
        /*
         * Bio list
         */
        INIT_LIST_HEAD(&new->bio_list);

        /*
         * Request processing work queue
         */        
        new->wq = alloc_workqueue(qname, 0, 0);
        if (!new->wq) {
                goto vfree;
        }
        
        /*
         * Get a request queue.
         */
        queue = blk_alloc_queue(GFP_KERNEL);
        if (!queue) {
        }
        queue->queuedata = new;
        blk_queue_make_request(queue, bdtun_make_request);

        if (queue == NULL) {
                goto vfree_wq;
        }
        
        blk_queue_logical_block_size(queue, logical_block_size);

        /*
         * Get registered.
         */
        bd_major = register_blkdev(0, "bdtun");
        if (bd_major <= 0) {
                printk(KERN_WARNING "bdtun: unable to get major number\n");
                goto vfree_wq_unreg;
        }
        
        /*
         * And the gendisk structure.
         */
        new->bd_gd = alloc_disk(16);
        if (!new->bd_gd) {
                goto vfree_wq_unreg;
        }
        new->bd_gd->major = bd_major;
        new->bd_gd->first_minor = 0;
        new->bd_gd->fops = &bdtun_ops;
        new->bd_gd->private_data = new;
        strcpy(new->bd_gd->disk_name, name);
        set_capacity(new->bd_gd, new->bd_nsectors);
        new->bd_gd->queue = queue;
        add_disk(new->bd_gd);

        /*
         * Initialize character device
         */
        printk(KERN_INFO "bdtun: setting up char device\n");
        if (alloc_chrdev_region(&ch_major, 0, 1, charname) != 0) {
                printk(KERN_WARNING "bdtun: could not allocate character device number\n");
                goto vfree_wq_unreg;
        }
        new->ch_major = ch_major;
        // register character device
        cdev_init(&new->ch_dev, &bdtunch_ops);
        new->ch_dev.owner = THIS_MODULE;
        error = cdev_add(&new->ch_dev, MKDEV(ch_major,0),1);
        if (error) {
                printk(KERN_NOTICE "bdtun: error setting up char device\n");
                goto vfree_wq_unreg;
        }
        
        /*
         * Add device to the list
         */
        list_add_tail(&new->list, &device_list);
        
        printk(KERN_NOTICE "bdtun: module init finished\n");
        return 0;
        
        /*
         * "catch exceptions"
         */
        vfree_wq_unreg:
                unregister_blkdev(bd_major, "bdtun");
        vfree_wq:
                destroy_workqueue(new->wq);
        vfree:
                vfree(new);
                return -ENOMEM;

        
}

static int bdtun_remove(char *name) {
        struct bdtun *dev;
        
        dev = bdtun_find_device(name);
        
        if (dev == NULL) {
                printk(KERN_NOTICE "bdtun: error removing '%s': no such device\n", name);
                return -ENOENT;
        }
        
        /* Destroy block devices */
        printk(KERN_DEBUG "bdtun: removing block device\n");
        unregister_blkdev(dev->bd_gd->major, "bdtun");
        blk_cleanup_queue(dev->bd_gd->queue);
        del_gendisk(dev->bd_gd);
        put_disk(dev->bd_gd);
        
        /* Get rid of the work queue */
        flush_workqueue(dev->wq);
        destroy_workqueue(dev->wq);
        
        /* Destroy character devices */
        printk(KERN_DEBUG "bdtun: removing char device\n");
        unregister_chrdev_region(dev->ch_major, 1);
        cdev_del(&dev->ch_dev);
        printk(KERN_NOTICE "bdtun: device shutdown finished\n");
        
        /* Unlink and free device structure */
        list_del(&dev->list);
        vfree(dev);
        printk(KERN_NOTICE "bdtun: device removed from list\n");
        
        return 0;
}

/*static int bdtun_info(char *name, struct bdtun_info *device_info) {
        struct bdtun *dev = bdtun_find_device(name);
        strncpy(device_info->name, name, 32);
        device_info->capacity = dev->bd_size;
        return 0;
}

static void bdtun_list(char **ptrs, int offset, int maxdevices) {
        struct list_head *ptr;
        struct bdtun *entry;
        int i;
        
        memset(ptrs, 0, maxdevices);
        i = 0;
        list_for_each(ptr, &device_list) {
                if (offset > 0) {
                        offset--;
                        continue;
                }
                if (i >= maxdevices) {
                        break;
                }
                entry = list_entry(ptr, struct bdtun, list);
                ptrs[i] = entry->bd_gd->disk_name;
                i++;
        }
}*/

/*
 * Initialize module
 */
static int __init bdtun_init(void) {
        bdtun_create("bdtuna", 512, 1000000);
        bdtun_create("bdtunb", 512, 10000000);
        return 0;
}

/*
 * Clean up on module remove
 */
static void __exit bdtun_exit(void) {
        bdtun_remove("bdtuna");
        bdtun_remove("bdtunb");
}

module_init(bdtun_init);
module_exit(bdtun_exit);
