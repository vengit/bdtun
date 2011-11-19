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
#include <linux/spinlock.h>

#include "bdtun.h"
#include "commands.h"

MODULE_LICENSE("GPL");

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
        int header_transferred;
};

/*
 * Device class for char device registration
 */
struct class *chclass;

/*
 * BDTun device structure
 */
struct bdtun {
        /* It's a linked list of devices */
        struct list_head list;
        
        /* character device related */
        struct cdev ch_dev;
        int ch_major;
        struct device *dev_chclass;
        
        struct list_head bio_out_list;
        struct list_head bio_in_list;
        wait_queue_head_t bio_list_out_queue;
        wait_queue_head_t bio_list_in_queue;
        
        /*
         * Acquire locks in this order:
         * first out, then in, then sem
         */
        spinlock_t bio_out_list_lock;
        spinlock_t bio_in_list_lock;
        struct semaphore sem;
        
        /* Block device related stuff*/
        unsigned long bd_size;
        int bd_block_size;
        int bd_nsectors;
        struct gendisk *bd_gd;
};

/*
 * Disk add work queue.
 */

struct bdtun_add_disk_work {
        struct gendisk *gd;
        struct work_struct work;
} add_disk_work;

struct workqueue_struct *add_disk_q;

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

static void bdtun_do_add_disk(struct work_struct *work)        {
        struct bdtun_add_disk_work *w = container_of(work, struct bdtun_add_disk_work, work);
        
        add_disk(w->gd);
}

/*
 * Request processing
 */
static int bdtun_make_request(struct request_queue *q, struct bio *bio) {
        struct bdtun *dev = q->queuedata;
        
        struct bdtun_bio_list_entry *new = kmalloc(sizeof(struct bdtun_bio_list_entry), GFP_ATOMIC);
        
        if (!new) {
                return -EIO;
        }
        
        new->bio = bio;
        new->header_transferred = 0;
        
        spin_lock_bh(&dev->bio_out_list_lock);
        list_add_tail(&new->list, &dev->bio_out_list);
        spin_unlock_bh(&dev->bio_out_list_lock);
        
        wake_up(&dev->bio_list_out_queue);
        
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

static int bdtunch_open(struct inode *inode, struct file *filp) {
        struct bdtun *dev = container_of(inode->i_cdev, struct bdtun, ch_dev);
        filp->private_data = dev;
        printk(KERN_DEBUG "bdtun: got device_open on char dev\n");
        return 0;
}

static int bdtunch_release(struct inode *inode, struct file *filp) {
        printk(KERN_DEBUG "bdtun: got device_release on char dev\n");
        return 0;
}

/*
 * Takes a bio, and sends it to the char device.
 * We don't know if the bio will complete at this point.
 * Writes to the char device will complete the bio-s.
 */
static ssize_t bdtunch_read(struct file *filp, char *buf, size_t count, loff_t *f_pos) {
        struct bdtun *dev = filp->private_data;
        struct bdtun_bio_list_entry *entry;
        struct bdtun_txreq *req;
        unsigned long pos = 0;
        struct bio_vec *bvec;
        DEFINE_WAIT(wait);
        int i;
        
        out_list_is_empty:
        
        prepare_to_wait(&dev->bio_list_out_queue, &wait, TASK_INTERRUPTIBLE);
        
        spin_lock_bh(&dev->bio_out_list_lock);
        
        if (list_empty(&dev->bio_out_list)) {
                spin_unlock_bh(&dev->bio_out_list_lock);
                schedule();
                finish_wait(&dev->bio_list_out_queue, &wait);
                
                if (signal_pending(current)) {
                        return -ERESTARTSYS;
                }
                
                goto out_list_is_empty;
        }
        
        finish_wait(&dev->bio_list_out_queue, &wait);
        entry = list_entry(dev->bio_out_list.next, struct bdtun_bio_list_entry, list);

        /* Validate request here to avoid queue manipulation on error */
        if (entry->header_transferred) {
                if (count != entry->bio->bi_size) {
                        spin_unlock_bh(&dev->bio_out_list_lock);
                        return -EIO;
                }
        } else {
                if (count != sizeof(struct bdtun_txreq)) {
                        spin_unlock_bh(&dev->bio_out_list_lock);
                        return -EIO;
                }
        }

        /* Put bio into in list if needed */
        if (bio_data_dir(entry->bio) == READ || entry->header_transferred) {
                 /* Yes, we remove it from the out list, because the
                 * next write will complete it. */
                list_del_init(dev->bio_out_list.next);
                
                /* Ok, the "in" list is not empty, and we're holding the lock.
                 * Acquire the "in" spinlock too. This is why we order this
                 * way */
                spin_lock_bh(&dev->bio_in_list_lock);
                
                /* Take the first (the oldest) bio, and insert it to the end
                 * of the "out" waiting list */
                list_add_tail(&entry->list, &dev->bio_in_list);
                
                spin_unlock_bh(&dev->bio_in_list_lock);
                
                /* Wake up the waiting writer processes */
                wake_up(&dev->bio_list_in_queue);
        }

        spin_unlock_bh(&dev->bio_out_list_lock);
        
        // TODO: need proper locking here. Use the semaphore.
        // TODO: maybe we need a semaphore in the structure itself?
        
        /* Do actual copying, if request size is valid. */
        if (entry->header_transferred) {
                /* Transfer bio data. */
                bio_for_each_segment(bvec, entry->bio, i) {
                        void *kaddr = kmap(bvec->bv_page);
                        if(copy_to_user(buf+pos, kaddr+bvec->bv_offset, bvec->bv_len) != 0) {
                                // TODO: error handling
                                printk(KERN_WARNING "bdtun: error copying data to user\n");
                                return -EFAULT;
                        }
                        kunmap(bvec->bv_page);
                        pos += bvec->bv_len;
                }
        } else {
                /* Transfer command header */
                req = (struct bdtun_txreq *)buf;
                req->write  = bio_data_dir(entry->bio) == WRITE;
                req->offset = entry->bio->bi_sector * KERNEL_SECTOR_SIZE;
                req->size   = entry->bio->bi_size;
                
                entry->header_transferred = 1;
        }
        
        return count;
}

static ssize_t bdtunch_write(struct file *filp, const char *buf, size_t count, loff_t *offset) {
        struct bdtun *dev = filp->private_data;
        struct bdtun_bio_list_entry *entry;
        struct bio_vec *bvec;
        unsigned long pos = 0;
        DEFINE_WAIT(wait);
        int i;

        in_list_is_empty:
        
        prepare_to_wait(&dev->bio_list_in_queue, &wait, TASK_INTERRUPTIBLE);
        
        spin_lock_bh(&dev->bio_in_list_lock);
        
        if (list_empty(&dev->bio_in_list)) {
                spin_unlock_bh(&dev->bio_in_list_lock);
                schedule();
                finish_wait(&dev->bio_list_in_queue, &wait);
                
                if (signal_pending(current)) {
                        return -ERESTARTSYS;
                }
                
                goto in_list_is_empty;
        }
        finish_wait(&dev->bio_list_in_queue, &wait);
        
        entry = list_entry(dev->bio_in_list.next, struct bdtun_bio_list_entry, list);
        
        list_del_init(dev->bio_in_list.next); /* We might re-queue */
        spin_unlock_bh(&dev->bio_in_list_lock);
        
        /* Validate write request size */
        if ((bio_data_dir(entry->bio) == READ && count != entry->bio->bi_size) ||
            (bio_data_dir(entry->bio) == WRITE && count != 1)) {
                /* This is bad, because there is no way to recocer
                 * from this condition at this point. Maybe a re-queue
                 * into the out list would help. */
                spin_lock_bh(&dev->bio_out_list_lock);
                list_add(&entry->list, &dev->bio_out_list);
                spin_unlock_bh(&dev->bio_out_list_lock);
                bio_endio(entry->bio, -EIO);
                return -EIO;
        }
        
        // TODO: read the completion byte
        
        /* Copy the data into the bio */
        if (bio_data_dir(entry->bio) == READ) {
                bio_for_each_segment(bvec, entry->bio, i) {
                        void *kaddr = kmap(bvec->bv_page);
                        if(copy_from_user(kaddr+bvec->bv_offset, buf+pos, bvec->bv_len) != 0) {
                                // TODO: error handling
                                printk(KERN_WARNING "bdtun: error copying data from user\n");
                                kunmap(bvec->bv_page);
                                bio_endio(entry->bio, -1);
                                return -EFAULT;
                        }
                        kunmap(bvec->bv_page);
                        pos += bvec->bv_len;
                }
        }
        
        /* Complete the io request */
        bio_endio(entry->bio, 0);
        
        /* Free the list entry */
        kfree(entry);
        
        /* Tell the user process that the IO has been completed */
        return count;
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
static int bdtun_create(char *name, int block_size, size_t size) {
        struct bdtun *new;
        struct request_queue *queue;
        int error;
        int bd_major;
        int ch_num;
        char charname[37];
        char qname[35];
        
        /*
         * Set up character device and workqueue name
         */
        strncpy(charname, name, 32);
        strcat(charname, "_tun");
        strncpy(qname, name, 32);
        strcat(qname, "_q");
        
        /*
         * Allocate device structure
         */
        new = vmalloc(sizeof (struct bdtun));
        if (new == NULL) {
                return -ENOMEM;
        }
        
        /*
         * Determine device size
         */
        new->bd_block_size = block_size;
        new->bd_nsectors   = size / block_size; // Size is just an approximate.
        new->bd_size       = new->bd_nsectors * block_size;
        
        /*
         * Semaphores stuff like that 
         */
        spin_lock_init(&new->bio_in_list_lock);
        spin_lock_init(&new->bio_out_list_lock);
        
        /*
         * Wait queues
         */
        init_waitqueue_head(&new->bio_list_in_queue);
        init_waitqueue_head(&new->bio_list_out_queue);
        
        /*
         * Bio list
         */
        INIT_LIST_HEAD(&new->bio_in_list);
        INIT_LIST_HEAD(&new->bio_out_list);
        
        add_disk_q = alloc_workqueue("bdtun_add_disk", 0, 0);
        if (!add_disk_q) {
                goto vfree;
        }
        
        /*
         * Get a request queue.
         */
        queue = blk_alloc_queue(GFP_KERNEL);

        if (queue == NULL) {
                goto vfree_adq;
        }
        
        queue->queuedata = new;
        blk_queue_logical_block_size(queue, block_size);
        blk_queue_io_min(queue, block_size);
        blk_queue_make_request(queue, bdtun_make_request);
        
        /*
         * Get registered.
         */
        bd_major = register_blkdev(0, "bdtun");
        if (bd_major <= 0) {
                printk(KERN_WARNING "bdtun: unable to get major number\n");
                goto vfree_adq_unreg;
        }
        
        /*
         * And the gendisk structure.
         */
        new->bd_gd = alloc_disk(16);
        if (!new->bd_gd) {
                goto vfree_adq_unreg;
        }
        new->bd_gd->major = bd_major;
        new->bd_gd->first_minor = 0;
        new->bd_gd->fops = &bdtun_ops;
        new->bd_gd->private_data = new;
        new->bd_gd->queue = queue;
        strcpy(new->bd_gd->disk_name, name);
        set_capacity(new->bd_gd, new->bd_size / KERNEL_SECTOR_SIZE);

        /*
         * Initialize character device
         */
        printk(KERN_INFO "bdtun: setting up char device\n");
        if (alloc_chrdev_region(&ch_num, 0, 1, charname) != 0) {
                printk(KERN_WARNING "bdtun: could not allocate character device number\n");
                goto vfree_adq_unreg;
        }
        new->ch_major = MAJOR(ch_num);
        // register character device
        cdev_init(&new->ch_dev, &bdtunch_ops);
        new->ch_dev.owner = THIS_MODULE;
        printk(KERN_INFO "bdtun: char major %d\n", new->ch_major);
        error = cdev_add(&new->ch_dev, ch_num ,1);
        if (error) {
                printk(KERN_NOTICE "bdtun: error setting up char device\n");
                goto vfree_adq_unreg;
        }
        
        /*
         * Add a device node
         */
        
        new->dev_chclass = device_create(chclass, NULL, MKDEV(new->ch_major, 0), NULL, charname);
        if (IS_ERR(new->dev_chclass)) {
                printk(KERN_NOTICE "bdtun: error setting up device object\n");
                goto vfree_adq_unreg;
        }
        // TODO: unregister this!!!
        
        /*
         * Add device to the list
         */
        list_add_tail(&new->list, &device_list);
        
        printk(KERN_NOTICE "bdtun: module init finished\n");
        
        /*
         * Register the disk now.
         * 
         * This needs to be done at the end, so the char device will be
         * up, and we will be able to serve io requests.
         * 
         * It would also be a great idea to postpone this step until
         * the open() on the char device, so insmod won't hang and
         * run onto a timeout eventually.
         * 
         * NOTE: add_disk() does not return until the kernel finished
         * reading the partition table. add_disk() should be postponed
         * until the first read() on the tunnel char dev.
         * 
         * Even then it should be fired off in a tasklet, so the call
         * can go on and wait on the bio to-do list.
         * 
         * Hmm... a tasklet could be created right now.
         * 
         * Ok, I fed up with kernel panics and deadlocks, I'll go
         * and play HAM radio. That's it.
         */
        INIT_WORK(&add_disk_work.work, bdtun_do_add_disk);
        add_disk_work.gd = new->bd_gd;
        queue_work(add_disk_q, &add_disk_work.work);

        return 0;
        
        /*
         * "catch exceptions"
         */
        vfree_adq_unreg:
                unregister_blkdev(bd_major, "bdtun");
        vfree_adq:
                destroy_workqueue(add_disk_q);
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
        
        /* Destroy character devices */
        printk(KERN_DEBUG "bdtun: removing char device\n");
        unregister_chrdev_region(dev->ch_major, 1);
        cdev_del(&dev->ch_dev);
        printk(KERN_NOTICE "bdtun: device shutdown finished\n");
        
        /* Unreg device object and class if needed TODO: only if needed */
        device_destroy(chclass, MKDEV(dev->ch_major, 0));
        
        /* Unlink and free device structure */
        list_del(&dev->list);
        vfree(dev);
        printk(KERN_NOTICE "bdtun: device removed from list\n");
        
        return 0;
}

static int bdtun_info(char *name, struct bdtun_info *device_info) {
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
}

/*
 * Initialize module
 */
static int __init bdtun_init(void) {
        chclass = class_create(THIS_MODULE, "bdtun");
        if (IS_ERR(chclass)) {
                printk(KERN_NOTICE "bdtun: error setting up device class\n");
                // TODO: corretct error values throughout the code
                return -ENOMEM;
        }
        bdtun_create("bdtuna", 4096, 102400000);
        return 0;
}

/*
 * Clean up on module remove
 */
static void __exit bdtun_exit(void) {
        /*
         * Destroy work queue
         */
        flush_workqueue(add_disk_q);
        destroy_workqueue(add_disk_q);
        
        bdtun_remove("bdtuna");
        class_destroy(chclass);
}

module_init(bdtun_init);
module_exit(bdtun_exit);
