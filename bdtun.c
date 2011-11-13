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
 * BDTun device structure
 */
struct bdtun {
        /* It's a linked list of devices */
        struct list_head list;
        
        /* character device related */
        struct cdev ch_dev;
        int ch_major;
        
        /* Buffers for communicating with the userland driver */
        int rq_buffersize;
        char *rq_buffer;
        char *rq_end;
        char *rq_rp;
        char *rq_wp;
        int res_buffersize;
        char *res_buffer;
        char *res_end;
        char *res_rp;
        char *res_wp;
        
        /* userland sync stuff */
        int ch_client_count;
        wait_queue_head_t rq_rqueue;
        wait_queue_head_t rq_wqueue;
        wait_queue_head_t res_rqueue;
        wait_queue_head_t res_wqueue;
        struct semaphore sem; /* semaphore for rarely modified fields */
        struct semaphore rq_sem; /* request queue sem */
        struct semaphore res_sem; /* response queue sem */
        struct workqueue_struct *wq;

        /* Block device related stuff*/
        unsigned long bd_size;
        int bd_block_size;
        int bd_nsectors;
        struct gendisk *bd_gd;
        
        /* bd sync stuff */
        spinlock_t bd_lock;
};

struct bdtun_work {
        int write;
        struct work_struct work;        
};

/*
 * Control device
 */
struct cdev ctrl_dev;

/*
 * Initialize device list
 */
LIST_HEAD(device_list);

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
 * Do the work: process an I/O request async. 
 */
void bdtun_do_work(struct work_struct *work) {
        //struct workparams *params = data;
        //printk(KERN_INFO "bdtun: doing work, write(%d)\n", params->write);
        //return NULL;
        struct bdtun_work *w = container_of(work, struct bdtun_work, work);
        printk(KERN_INFO "bdtun: doing some work, write(%d). But dunno what. Meh.\n", w->write);
        // OMG should we do thes here???
        // There was a freeze, but after commenting out, it remained.
        kfree(w);
}

/*
 * Handle an I/O request.
 */
static int bdtun_transfer(struct bdtun *dev, unsigned long offset, unsigned long nbytes, char *buffer, int write) {
        struct bdtun_work *w = kmalloc(sizeof(struct bdtun_work), GFP_ATOMIC);
        
        if (!w) {
                return -EIO;
        }
        
        INIT_WORK(&w->work, bdtun_do_work);
        w->write = write;
        
        if ((offset + nbytes) > dev->bd_size) {
                printk (KERN_NOTICE "bdtun: Beyond-end write (%ld %ld)\n", offset, nbytes);
                kfree(w);
                return -EIO;
        }
        
        queue_work(dev->wq, &w->work);
        
        //if (write) {
                // TODO: put transfer onto the queue
                //memcpy(dev->bd_data + offset, buffer, nbytes);
        //} else {
                // TODO: put transfer onto the queue
                //memcpy(buffer, dev->bd_data + offset, nbytes);
        //}
        return 0;
}

/*
 * Transfer one bio structure 
 */
static int bdtun_xfer_bio(struct bdtun *dev, struct bio *bio) {
        int i;
        struct bio_vec *bvec;
        unsigned long offset = bio->bi_sector << 9;
        
        bio_for_each_segment(bvec, bio, i) {
                char *buffer = __bio_kmap_atomic(bio, i, KM_USER0);
                bdtun_transfer(dev, offset, bio_cur_bytes(bio), buffer, bio_data_dir(bio) == WRITE);
                offset += bio_cur_bytes(bio);
                __bio_kunmap_atomic(bio, KM_USER0);
        }
        
        return 0;
}

/*
 * Request processing
 */
int bdtun_make_request(struct request_queue *q, struct bio *bio) {
        struct bdtun *dev = q->queuedata;
        int status;
        
        status = bdtun_xfer_bio(dev, bio);
        bio_endio(bio, status);
        
        return 0;
}

/*
 *  Get the "drive geometry"
 */
int bdtun_getgeo (struct block_device *bdev, struct hd_geometry *geo) {
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

int bdtunch_open(struct inode *inode, struct file *file) {
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

int bdtunch_release(struct inode *inode, struct file *file) {
        struct bdtun *dev = file->private_data;
        
        printk(KERN_DEBUG "bdtun: got device_release on char dev\n");
        
        if (down_interruptible(&dev->sem)) {
                return -ERESTARTSYS;
        }
        
        dev->ch_client_count--;

        up(&dev->sem);
        
        return 0;
}

ssize_t bdtunch_read(struct file *filp, char *buf, size_t count, loff_t * f_pos) {
        struct bdtun *dev = filp->private_data;
        
        printk(KERN_DEBUG "bdtun: got device_read on char dev\n");

        if (down_interruptible(&dev->rq_sem)) {
                return -ERESTARTSYS;
        }

        while (dev->rq_rp == dev->rq_wp) { /* nothing to read */
                up(&dev->rq_sem); /* release the lock */
                if (filp->f_flags & O_NONBLOCK) {
                        return -EAGAIN;
                }
                
                if (wait_event_interruptible(dev->rq_rqueue, (dev->rq_rp != dev->rq_wp))) {
                        return -ERESTARTSYS; /* signal: tell the fs layer to handle it */
                }
                
                /* otherwise loop, but first reacquire the lock */
                if (down_interruptible(&dev->rq_sem)) {
                        return -ERESTARTSYS;
                }
        }
        
        /* ok, data is there, return something */
        
        if (dev->rq_wp > dev->rq_rp) {
                count = min(count, (size_t)(dev->rq_wp - dev->rq_rp));
        } else {
                /* the write pointer has wrapped, return data up to dev->end */
                count = min(count, (size_t)(dev->rq_end - dev->rq_rp));
        }
        
        if (copy_to_user(buf, dev->rq_rp, count)) {
                up (&dev->rq_sem);
                return -EFAULT;
        }
        
        dev->rq_rp += count;
        
        if (dev->rq_rp == dev->rq_end) {
                dev->rq_rp = dev->rq_buffer; /* wrapped */
        }
        
        up (&dev->rq_sem);
        
        /* finally, awake any writers and return */
        wake_up_interruptible(&dev->rq_wqueue);
        
        return count;
}

/* How much space is free? */
static int bdtunch_res_spacefree(struct bdtun *dev)
{
        if (dev->res_rp == dev->res_wp)
                return dev->res_buffersize - 1;
        return ((dev->res_rp + dev->res_buffersize - dev->res_wp) % dev->res_buffersize) - 1;
}

/*
 * Wait for space for writing; caller must hold device semaphore.  On
 * error the semaphore will be released before returning.
 */
static int bdtunch_getwritespace(struct bdtun *dev, struct file *filp)
{
        while (bdtunch_res_spacefree(dev) == 0) { /* full */
                DEFINE_WAIT(wait);
                
                up(&dev->res_sem);
                if (filp->f_flags & O_NONBLOCK)
                        return -EAGAIN;
                prepare_to_wait(&dev->res_wqueue, &wait, TASK_INTERRUPTIBLE);
                if (bdtunch_res_spacefree(dev) == 0)
                        schedule();
                finish_wait(&dev->res_wqueue, &wait);
                if (signal_pending(current))
                        return -ERESTARTSYS; /* signal: tell the fs layer to handle it */
                if (down_interruptible(&dev->sem))
                        return -ERESTARTSYS;
        }
        return 0;
}

ssize_t bdtunch_write(struct file *filp, const char *buf, size_t count, loff_t *offset) {
        struct bdtun *dev = filp->private_data;
        int result;
 
        if (down_interruptible(&dev->res_sem)) {
                return -ERESTARTSYS;
        }

        /* Make sure there's space to write */
        result = bdtunch_getwritespace(dev, filp);
        if (result)
                return result; /* scull_getwritespace called up(&dev->sem) */

        /* ok, space is there, accept something */
        count = min(count, (size_t)bdtunch_res_spacefree(dev));
        if (dev->res_wp >= dev->res_rp)
                count = min(count, (size_t)(dev->res_end - dev->res_wp)); /* to end-of-buf */
        else /* the write pointer has wrapped, fill up to rp-1 */
                count = min(count, (size_t)(dev->res_rp - dev->res_wp - 1));
        if (copy_from_user(dev->res_wp, buf, count)) {
                up (&dev->sem);
                return -EFAULT;
        }
        dev->res_wp += count;
        if (dev->res_wp == dev->res_end)
                dev->res_wp = dev->res_buffer; /* wrapped */
        up(&dev->res_sem);

        /* finally, awake any reader */
        wake_up_interruptible(&dev->res_rqueue);  /* blocked in read() and select() */

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
int bdtun_create(char *name, int logical_block_size, size_t size) {
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
         * Allocate the communication buffer
         */
        new->rq_buffersize = COMM_BUFFER_SIZE;
        new->rq_buffer = vmalloc(COMM_BUFFER_SIZE);
        
        if (new->rq_buffer == NULL) {
                goto vfree;
        }
        
        new->rq_end = new->rq_buffer;
        new->rq_wp  = new->rq_buffer;
        new->rq_rp  = new->rq_buffer;
        
        new->ch_client_count = 0;
        
        /*
         * Determine device size
         */
        new->bd_block_size = logical_block_size;
        new->bd_nsectors   = size / logical_block_size; // Size is just an approximate.
        new->bd_size       = new->bd_nsectors * logical_block_size;
        
        /*
         * Locks, stuff like that 
         */
        spin_lock_init(&new->bd_lock);
        sema_init(&new->rq_sem, 1);
        sema_init(&new->res_sem, 1);

        /*
         * Request processing work queue
         */        
        new->wq = alloc_workqueue(qname, 0, 0);
        if (!new->wq) {
                goto vfree_rqbuf;
        }
        
        /*
         * Get a request queue.
         */
        queue = blk_alloc_queue(GFP_KERNEL);
        if (!queue) {
        }
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
        vfree_rqbuf:
                vfree(new->rq_buffer);
        vfree:
                vfree(new);
                return -ENOMEM;

        
}

int bdtun_remove(char *name) {
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
        vfree(dev->rq_buffer);
        vfree(dev);
        printk(KERN_NOTICE "bdtun: device removed from list\n");
        
        return 0;
}

int bdtun_info(char *name, struct bdtun_info *device_info) {
        struct bdtun *dev = bdtun_find_device(name);
        strncpy(device_info->name, name, 32);
        device_info->capacity = dev->bd_size;
        return 0;
}

void bdtun_list(char **ptrs, int offset, int maxdevices) {
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
