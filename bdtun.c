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
static struct class *chclass;
static struct device *ctrl_device;
static int ctrl_devnum;
static int ctrl_ucnt;
static struct spinlock ctrl_lock;

/*
 * BDTun device structure
 */
struct bdtun {
        /* It's a linked list of devices */
        struct list_head list;
        
        /* character device related */
        struct cdev ch_dev;
        int ch_num;
        struct device *ch_device;
        
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
        spinlock_t lock;
        int removing;
        
        /* Use count */
        int ucnt;
        int ch_ucnt;
        
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
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERNEL_SECTOR_SIZE 512

static void bdtun_do_add_disk(struct work_struct *work)
{
        struct bdtun_add_disk_work *w = container_of(work, struct bdtun_add_disk_work, work);
        
        add_disk(w->gd);
}

/*
 * Request processing
 */
static int bdtun_make_request(struct request_queue *q, struct bio *bio)
{
        struct bdtun *dev = q->queuedata;
        
        struct bdtun_bio_list_entry *new = kmalloc(sizeof(struct bdtun_bio_list_entry), GFP_ATOMIC);
        
        PDEBUG("make_request called\n");
        
        if (!new) {
                return -EIO;
        }
        
        new->bio = bio;
        new->header_transferred = 0;
        
        spin_lock_bh(&dev->bio_out_list_lock);
        list_add_tail(&new->list, &dev->bio_out_list);
        spin_unlock_bh(&dev->bio_out_list_lock);
        
        wake_up(&dev->bio_list_out_queue);
        PDEBUG("request queued\n");
        
        return 0;
}

static int bdtun_open(struct block_device *bdev, fmode_t mode)
{
        struct bdtun *dev = bdev->bd_disk->queue->queuedata;
        PDEBUG("bdtun_open()\n");
        spin_lock(&dev->lock);
        if (dev->removing) {
                spin_unlock(&dev->lock);
                return -ENOENT;
        }
        dev->ucnt++;
        PDEBUG("device counter is %d for %s\n", dev->cnt, dev->bd_gd->disk_name);
        spin_unlock(&dev->lock);
        return 0;
}

static int bdtun_release(struct gendisk *gd, fmode_t mode)
{
        struct bdtun *dev = gd->queue->queuedata;
        PDEBUG("bdtun_release()\n");
        spin_lock(&dev->lock);
        dev->ucnt--;
        PDEBUG("device counter is %d for %s\n", dev->cnt, dev->bd_gd->disk_name);
        spin_unlock(&dev->lock);
        return 0;
}

static int bdtun_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
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
        .owner   = THIS_MODULE,
        .open    = bdtun_open,
        .release = bdtun_release,
        .getgeo  = bdtun_getgeo
};

/*
 * Character device
 */

static int bdtunch_open(struct inode *inode, struct file *filp)
{
        struct bdtun *dev = container_of(inode->i_cdev, struct bdtun, ch_dev);

        PDEBUG("got device_open on char dev\n");
        spin_lock(&dev->lock);
        if (dev->ch_ucnt) {
                spin_unlock(&dev->lock);
                return -EBUSY;
        }
        if (dev->removing) {
                spin_unlock(&dev->lock);
                return -EBUSY;
        }
        dev->ch_ucnt++;
        dev->ucnt++;
        spin_unlock(&dev->lock);
        filp->private_data = dev;
        return 0;
}

static int bdtunch_release(struct inode *inode, struct file *filp)
{
        struct bdtun *dev = container_of(inode->i_cdev, struct bdtun, ch_dev);

        PDEBUG("got device_release on char dev\n");
        spin_lock(&dev->lock);
        dev->ch_ucnt--;
        dev->ucnt--;
        spin_unlock(&dev->lock);
        return 0;
}

/*
 * Takes a bio, and sends it to the char device.
 * We don't know if the bio will complete at this point.
 * Writes to the char device will complete the bio-s.
 */
static ssize_t bdtunch_read(struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
        struct bdtun *dev = filp->private_data;
        struct bdtun_bio_list_entry *entry;
        struct bdtun_txreq *req;
        unsigned long pos = 0;
        struct bio_vec *bvec;
        DEFINE_WAIT(wait);
        int i;
        
        out_list_is_empty:
        
        PDEBUG("Preparing to wait\n");
        prepare_to_wait(&dev->bio_list_out_queue, &wait, TASK_INTERRUPTIBLE);
        
        PDEBUG("grabbing spinlock on out queue\n");
        spin_lock_bh(&dev->bio_out_list_lock);
        
        if (list_empty(&dev->bio_out_list)) {
                PDEBUG("list empty, releasing spinlock for out queue\n");
                spin_unlock_bh(&dev->bio_out_list_lock);
                PDEBUG("calling schedulle\n");
                schedule();
                PDEBUG("awaken, finishing wait\n");
                finish_wait(&dev->bio_list_out_queue, &wait);
                
                PDEBUG("checking for pending signals\n");
                if (signal_pending(current)) {
                        PDEBUG("signals are pending, returning -ERESTARTSYS\n");
                        return -ERESTARTSYS;
                }
                
                PDEBUG("no pending signals, checking out queue again\n");
                goto out_list_is_empty;
        }
        
        PDEBUG("out list containts bio-s, finishing wait\n");
        finish_wait(&dev->bio_list_out_queue, &wait);
        PDEBUG("getting first entry\n");
        entry = list_entry(dev->bio_out_list.next, struct bdtun_bio_list_entry, list);

        /* Validate request here to avoid queue manipulation on error */
        PDEBUG("validating request size\n");
        if (entry->header_transferred) {
                if (count != entry->bio->bi_size) {
                        PDEBUG("request size not equals bio size, returning -EIO\n");
                        spin_unlock_bh(&dev->bio_out_list_lock);
                        return -EIO;
                }
        } else {
                if (count != BDTUN_TXREQ_HEADER_SIZE) {
                        PDEBUG("request size not equals txreq header size (should be %lu), returning -EIO\n", BDTUN_TXREQ_HEADER_SIZE);
                        spin_unlock_bh(&dev->bio_out_list_lock);
                        return -EIO;
                }
        }
        PDEBUG("request size is OK\n");

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

        PDEBUG("releasing out queue spinlock\n");
        spin_unlock_bh(&dev->bio_out_list_lock);
        
        // TODO: need proper locking here. Use the semaphore.
        // TODO: maybe we need a semaphore in the structure itself?
        
        /* Do actual copying, if request size is valid. */
        if (entry->header_transferred) {
                /* Transfer bio data. */
                bio_for_each_segment(bvec, entry->bio, i) {
                        void *kaddr = kmap(bvec->bv_page);
                        // TODO: do direct IO here to speed things up
                        if(copy_to_user(buf+pos, kaddr+bvec->bv_offset, bvec->bv_len) != 0) {
                                PDEBUG("error copying data to user\n");
                                kunmap(bvec->bv_page);
                                return -EFAULT;
                        }
                        kunmap(bvec->bv_page);
                        pos += bvec->bv_len;
                }
        } else {
                /* Transfer command header */
                req = (struct bdtun_txreq *)buf;
                req->flags  = entry->bio->bi_rw;
                req->offset = entry->bio->bi_sector * KERNEL_SECTOR_SIZE;
                req->size   = entry->bio->bi_size;
                
                entry->header_transferred = 1;
        }
        
        return count;
}

static ssize_t bdtunch_write(struct file *filp, const char *buf, size_t count, loff_t *offset)
{
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
                spin_lock_bh(&dev->bio_out_list_lock);
                list_add(&entry->list, &dev->bio_out_list);
                spin_unlock_bh(&dev->bio_out_list_lock);
                entry->header_transferred = 0;
                PDEBUG("invalid request size from user returning -EIO and re-queueing bio.\n");
                return -EIO;
        }
        
        // TODO: read the completion byte
        
        /* Copy the data into the bio */
        if (bio_data_dir(entry->bio) == READ) {
                bio_for_each_segment(bvec, entry->bio, i) {
                        void *kaddr = kmap(bvec->bv_page);
                        if(copy_from_user(kaddr+bvec->bv_offset, buf+pos, bvec->bv_len) != 0) {
                                PDEBUG("error copying data from user\n");
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

static struct bdtun *bdtun_find_device(const char *name)
{
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
static int bdtun_create_k(const char *name, int block_size, uint64_t size)
{
        struct bdtun *new;
        struct request_queue *queue;
        int error;
        int bd_major;
        char charname[BDTUN_DEVNAME_SIZE + 5];
        char qname[BDTUN_DEVNAME_SIZE + 3];
        
        /*
         * Set up character device and workqueue name
         */
        strncpy(charname, name, BDTUN_DEVNAME_SIZE);
        strcat(charname, "_tun");
        strncpy(qname, name, BDTUN_DEVNAME_SIZE);
        strcat(qname, "_q");
        
        /*
         * Allocate device structure
         */
        new = vmalloc(sizeof (struct bdtun));
        if (new == NULL) {
                PDEBUG("Could not allocate memory for device structure\n");
                error = -ENOMEM;
                goto out;
        }
        
        /*
         * Determine device size
         */
        new->bd_block_size = block_size;
        new->bd_nsectors   = size / block_size; // Incoming size is just an approximate.
        new->bd_size       = new->bd_nsectors * block_size;
        
        /*
         * Semaphores and stuff like that 
         */
        spin_lock_init(&new->bio_in_list_lock);
        spin_lock_init(&new->bio_out_list_lock);
        spin_lock_init(&new->lock);
        
        new->removing = 0;
        new->ucnt     = 0;
        new->ch_ucnt  = 0;
        
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
        
        /*
         * Get a request queue.
         */
        queue = blk_alloc_queue(GFP_KERNEL);

        if (queue == NULL) {
                PDEBUG("Could not allocate request queue\n");
                error = -ENOMEM;
                goto out_vfree;
        }
        
        // TODO: get bd features from command line parameters
        // e.g. there must be an extra "flags" param in create
        queue->queuedata = new;
        blk_queue_logical_block_size(queue, block_size);
        blk_queue_io_min(queue, block_size);
        blk_queue_make_request(queue, bdtun_make_request);
        blk_queue_flush(queue, REQ_FLUSH | REQ_FUA);
        blk_queue_discard(queue);
        
        /*
         * Get registered.
         */
        bd_major = register_blkdev(0, "bdtun");
        if (bd_major < 0) {
                PDEBUG("unable to get major number\n");
                error = bd_major;
                goto out_cleanup_queue;
        }
        
        /*
         * And the gendisk structure.
         */
        new->bd_gd = alloc_disk(BDTUN_BD_MINORS);
        if (!new->bd_gd) {
                PDEBUG("Unable to alloc_disk()\n");
                error = -ENOMEM;
                goto out_unregister_blkdev;
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
        PDEBUG("setting up char device\n");
        error = alloc_chrdev_region(&new->ch_num, 0, 1, charname);
        if (error) {
                PDEBUG("could not allocate character device number\n");
                goto out_del_disk;
        }
        
        /*
         * Register character device
         */
        cdev_init(&new->ch_dev, &bdtunch_ops);
        new->ch_dev.owner = THIS_MODULE;
        
        PDEBUG("char major %d\n", MAJOR(new->ch_num));
        error = cdev_add(&new->ch_dev, new->ch_num ,1);
        if (error) {
                PDEBUG("error setting up char device\n");
                goto out_unregister_chrdev_region;
        }
        
        /*
         * Add a device node
         */
        
        new->ch_device = device_create(chclass, NULL, new->ch_num, NULL, charname);
        if (IS_ERR(new->ch_device)) {
                PDEBUG("error setting up device object\n");
                error = -ENOMEM;
                goto out_cdev_del;
        }
        
        /*
         * Add device to the list
         */
        list_add_tail(&new->list, &device_list);
        
        /*
         * Register the disk in a tasklet.
         */
        INIT_WORK(&add_disk_work.work, bdtun_do_add_disk);
        add_disk_work.gd = new->bd_gd;
        queue_work(add_disk_q, &add_disk_work.work);
        
        PDEBUG("finished setting up device %s\n", name);
        
        try_module_get(THIS_MODULE);
        
        return 0;
        
        out_cdev_del:
                cdev_del(&new->ch_dev);
        out_unregister_chrdev_region:
                unregister_chrdev_region(new->ch_num, 1);
        out_del_disk:
                del_gendisk(new->bd_gd);
                put_disk(new->bd_gd);
        out_unregister_blkdev:
                unregister_blkdev(bd_major, "bdtun");
        out_cleanup_queue:
                blk_cleanup_queue(queue);
        out_vfree:
                vfree(new);
        out:
                return error;
}

static void bdtun_remove_dev(struct bdtun *dev)
{
        /* Destroy block devices */
        PDEBUG("removing block device\n");
        unregister_blkdev(dev->bd_gd->major, "bdtun");
        blk_cleanup_queue(dev->bd_gd->queue);
        del_gendisk(dev->bd_gd);
        put_disk(dev->bd_gd);
        
        /* Destroy character devices */
        PDEBUG("removing char device\n");
        unregister_chrdev_region(dev->ch_num, 1);
        cdev_del(&dev->ch_dev);
        PDEBUG("device shutdown finished\n");
        
        /* Unreg device object and class if needed */
        device_destroy(chclass, dev->ch_num);
        
        module_put(THIS_MODULE);
}

static int bdtun_remove_k(const char *name)
{
        struct bdtun *dev;
        
        dev = bdtun_find_device(name);
        
        if (dev == NULL) {
                PDEBUG("error removing '%s': no such device\n", name);
                return -ENOENT;
        }
        
        spin_lock(&dev->lock);
                
        if (dev->ucnt) {
                spin_unlock(&dev->lock);
                return -EBUSY;
        }
        dev->removing = 1;
        spin_unlock(&dev->lock);
        
        bdtun_remove_dev(dev);
        
        /* Unlink and free device structure */
        list_del(&dev->list);
        vfree(dev);
        PDEBUG("device removed from list\n");

        return 0;
}

static int bdtun_info_k(char *name, struct bdtun_info *device_info)
{
        struct bdtun *dev = bdtun_find_device(name);
        
        if (dev == NULL)
                return -ENOENT;
        
        device_info->bd_size       = dev->bd_size;
        device_info->bd_block_size = dev->bd_block_size;
        device_info->bd_major      = dev->bd_gd->major;
        device_info->bd_minor      = dev->bd_gd->first_minor;
        device_info->ch_major      = MAJOR(dev->ch_num);
        
        return 0;
}

static int bdtun_list_k(
        char *buf, const int bufsize, const int maxdevices_internal,
        size_t offset, size_t maxdevices)
{
        struct list_head *ptr;
        struct bdtun *entry;
        int i, bufpos, len;
        
        i = 0;
        bufpos = 0;
        list_for_each(ptr, &device_list) {
                if (offset > 0) {
                        offset--;
                        continue;
                }
                if (i >= maxdevices || i >= maxdevices_internal) {
                        break;
                }
                entry = list_entry(ptr, struct bdtun, list);
                len = strlen(entry->bd_gd->disk_name)+1;
                if (bufpos + len > bufsize) {
                        break;
                }
                memcpy(buf + bufpos, entry->bd_gd->disk_name, len);
                bufpos += len;
                i++;
        }
        
        return bufpos;
}

/*
 * Control device functions
 */
static int ctrl_open(struct inode *inode, struct file *filp)
{
        PDEBUG("got device_open on master dev\n");

        spin_lock(&ctrl_lock);
        if (ctrl_ucnt) {
                spin_unlock(&ctrl_lock);
                return -EBUSY;
        }
        ctrl_ucnt++;
        spin_unlock(&ctrl_lock);
        
        return 0;
}

static int ctrl_release(struct inode *inode, struct file *filp)
{
        PDEBUG("got device_release on master dev\n");

        spin_lock(&ctrl_lock);
        ctrl_ucnt--;
        spin_unlock(&ctrl_lock);

        return 0;
}

static char ctrl_response_buf[BDTUN_RESPONSE_SIZE];
static int ctrl_response_size = 0;

/*
 * Copies last response to user. A 0 sized answer means no answer.
 */
static ssize_t ctrl_read(struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
        int tmp = ctrl_response_size;
        
        if (ctrl_response_size < 0) {
                return -EIO;
        }
        
        if(copy_to_user(buf, ctrl_response_buf, ctrl_response_size) != 0) {
                PDEBUG("error copying data to user in ctrl_read\n");
                return -EFAULT;
        }
        
        ctrl_response_size = 0;
        
        return tmp;
}

static ssize_t ctrl_write(struct file *filp, const char *buf, size_t count, loff_t *offset)
{
        struct bdtun_ctrl_command *c;
        struct bdtun_info info;
        int ret;
        
        if (count < 1) {
                PDEBUG("received count < 1\n");
                return -EIO;
        }
        
        c = (struct bdtun_ctrl_command *) buf;
        
        PDEBUG("received command: %d\n", c->command);
        
        switch (c->command) {
        case BDTUN_COMM_CREATE:
                if (count < BDTUN_COMM_CREATE_SIZE) {
                        return -EIO;
                }
                ret = bdtun_create_k(c->create.name, c->create.blocksize, c->create.size);
                
                if (ret < 0) {
                        return ret;
                }
                
                break;
        case BDTUN_COMM_REMOVE:
                if (count < BDTUN_COMM_REMOVE_SIZE) {
                        return -EIO;
                }
                
                ret = bdtun_remove_k(c->remove.name);
                
                if (ret < 0) {
                        return ret;
                }
                
                break;
        case BDTUN_COMM_INFO:
                if (count < BDTUN_COMM_INFO_SIZE) {
                        return -EIO;
                }
                
                ret = bdtun_info_k(c->info.name, &info);
                
                if (ret != 0) {
                        return ret;
                }
                
                memcpy(ctrl_response_buf, &info, ctrl_response_size = sizeof(info));
                
                break;
        case BDTUN_COMM_LIST:
                if (count < BDTUN_COMM_LIST_SIZE) {
                        return -EIO;
                }
                
                ctrl_response_size = bdtun_list_k(
                        ctrl_response_buf, BDTUN_RESPONSE_SIZE,
                        BDTUN_DEVNAMES, c->list.offset, c->list.maxdevices
                );
                
                break;
        case BDTUN_COMM_RESIZE:
                // TODO: how to communicate this with the client?
        default:
                return -EIO;
        }
        
        return count;
}

/*
 * Operations on control device
 */
static struct file_operations ctrl_ops = {
        .read    = ctrl_read,
        .write   = ctrl_write,
        .open    = ctrl_open,
        .release = ctrl_release
};

/*
 * Initialize module
 */
static int __init bdtun_init(void)
{
        int error;
        
        spin_lock_init(&ctrl_lock);
        ctrl_ucnt = 0;
        
        /*
         * Set up a work queue for adding disks
         */
        add_disk_q = alloc_workqueue("bdtun_add_disk", 0, 0);
        if (!add_disk_q) {
                error = -ENOMEM;
                goto out_err;
        }
                
        /*
         * Set up a device class 
         */
        chclass = class_create(THIS_MODULE, "bdtun");
        // TODO: how do I get a proper error code here?
        if (IS_ERR(chclass)) {
                PDEBUG("error setting up device class\n");
                error = -ENOMEM;
                goto out_adq;
        }

        /*
         * Initialize master character device
         */
        PDEBUG("setting up char device\n");

        error = alloc_chrdev_region(&ctrl_devnum, 0, 1, "bdtun");
        if (error) {
                printk(KERN_ERR "could not allocate control device number\n");
                goto out_destroy_class;
        }
        cdev_init(&ctrl_dev, &ctrl_ops);
        ctrl_dev.owner = THIS_MODULE;
        error = cdev_add(&ctrl_dev, ctrl_devnum ,1);
        if (error) {
                PDEBUG("error setting up control device\n");
                goto out_unregister_chrdev_region;
        }
        
        /*
         * Add a device node
         */
        
        ctrl_device = device_create(chclass, NULL, ctrl_devnum, NULL, "bdtun");
        if (IS_ERR(ctrl_device)) {
                PDEBUG("error setting up control device object\n");
                error = -ENOMEM;
                goto out_cdev_del;
        }

        return 0;

        out_cdev_del:
                cdev_del(&ctrl_dev);
        out_unregister_chrdev_region:
                unregister_chrdev_region(ctrl_devnum, 1);
        out_destroy_class:
                class_destroy(chclass);
        out_adq:
                destroy_workqueue(add_disk_q);
        out_err:
                return -ENOMEM;
}

/*
 * Clean up on module remove
 */
static void __exit bdtun_exit(void)
{
        struct list_head *ptr;

        list_for_each(ptr, &device_list)
                bdtun_remove_dev(list_entry(ptr, struct bdtun, list));

        flush_workqueue(add_disk_q);
        destroy_workqueue(add_disk_q);
        device_destroy(chclass, ctrl_devnum);
        cdev_del(&ctrl_dev);
        unregister_chrdev_region(ctrl_devnum, 1);
        class_destroy(chclass);
}

module_init(bdtun_init);
module_exit(bdtun_exit);
