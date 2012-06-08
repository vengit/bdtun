/*
 * A simple block device to forward requests to userspace
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/cdev.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/semaphore.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/version.h>

#include "../include/bdtun.h"

MODULE_LICENSE("GPL");

/*
 * A list in wich unprocessed bio-s are contained. The work items
 * in the work queue put bio-s into these list items and chain them
 * up to form a linked list. The list is processed upon reads and writes
 * on the char device. 
 */
struct bdtun_bio_list_entry {
        struct list_head list;
        struct bio *bio;
        unsigned long start_time;
        /* TODO: start_time mimics the similarly named field
         * in a request struct. This may be inaccurate. Check
         * where this field gets propagated to see if this is OK.
         */
};

/*
 * Device class for char device registration
 */
static struct class *chclass;
static struct device *ctrl_device;
static int ctrl_devnum;
static int ctrl_ucnt;
static struct mutex mutex;

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
        
        struct list_head bio_list;
        wait_queue_head_t reader_queue;
        
        spinlock_t bio_list_lock;
        spinlock_t lock;
        int removing;
        
        /* Use count */
        int ucnt;
        int ch_ucnt;
        
        /* Block device related stuff */
        uint64_t bd_size;
        uint64_t bd_block_size;
        uint64_t bd_nsectors;
        int capabilities;
        struct gendisk *bd_gd;
        struct mutex mutex;
};

/*
 * Disk add work queue.
 */

struct bdtun_add_disk_work {
        struct gendisk *gd;
        struct mutex *mutex;
        struct work_struct work;
};

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
        mutex_unlock(w->mutex);
        PDEBUG("Unlocked mutex in bdtun_do_add_disk\n");
        kfree(w);
}

/*
 * Request processing
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
#   define MKREQ_RETTYPE int
#   define MKREQ_RETVAL 0
#else
#   define MKREQ_RETTYPE void
#   define MKREQ_RETVAL 
#endif

/* TODO: only ONE request is delivered by the kernel,
 * this is a very big limitation */
static MKREQ_RETTYPE bdtun_make_request(struct request_queue *q, struct bio *bio)
{
        struct bdtun *dev = (struct bdtun *)(q->queuedata);
        struct bdtun_bio_list_entry *new;
        const int rw = bio_data_dir(bio);
        int cpu;
        
        spin_lock(&dev->lock);
        if (dev->removing) {
                spin_unlock(&dev->lock);
                bio_endio(bio, -EIO);
                return MKREQ_RETVAL;
        }
        spin_unlock(&dev->lock);
        
        new = kmalloc(sizeof(struct bdtun_bio_list_entry), GFP_KERNEL);
        
        PDEBUG("make_request called\n");
        
        if (!new) {
                PDEBUG("Could not allocate bio list entry\n");
                bio_endio(bio, -EIO);
                return MKREQ_RETVAL;
        }

        new->start_time = jiffies;
        new->bio = bio;
        
        spin_lock(&dev->bio_list_lock);
        list_add_tail(&new->list, &dev->bio_list);
        spin_unlock(&dev->bio_list_lock);
        
        wake_up(&dev->reader_queue);
        PDEBUG("request queued\n");

        cpu = part_stat_lock();
        part_stat_inc(cpu, &dev->bd_gd->part0, ios[rw]);
        part_stat_add(cpu, &dev->bd_gd->part0, sectors[rw], bio_sectors(bio));
        part_inc_in_flight(&dev->bd_gd->part0, rw);
        part_stat_unlock();

        return MKREQ_RETVAL;
}

static int bdtun_open(struct block_device *bdev, fmode_t mode)
{
        // TODO: what if the bdev structure is kfreed in the meantime we run??
        struct bdtun *dev = (struct bdtun *)(bdev->bd_disk->queue->queuedata);
        PDEBUG("bdtun_open()\n");
        spin_lock(&dev->lock);
        if (dev->removing) {
                spin_unlock(&dev->lock);
                return -ENOENT;
        }
        spin_unlock(&dev->lock);
        return 0;
}

static int bdtun_release(struct gendisk *gd, fmode_t mode)
{
        PDEBUG("bdtun_release()\n");
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
        // TODO: implicit cast, not nice
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
        prepare_to_wait(&dev->reader_queue, &wait, TASK_INTERRUPTIBLE);
        
        PDEBUG("grabbing spinlock on out queue\n");
        spin_lock(&dev->bio_list_lock);
        
        if (list_empty(&dev->bio_list)) {
                PDEBUG("list empty, releasing spinlock for out queue\n");
                spin_unlock(&dev->bio_list_lock);
                PDEBUG("calling schedulle\n");
                schedule();
                PDEBUG("awaken, finishing wait\n");
                finish_wait(&dev->reader_queue, &wait);
                
                PDEBUG("checking for pending signals\n");
                if (signal_pending(current)) {
                        PDEBUG("signals are pending, returning -ERESTARTSYS\n");
                        return -ERESTARTSYS;
                }
                
                PDEBUG("no pending signals, checking out queue again\n");
                goto out_list_is_empty;
        }
        
        PDEBUG("list containts bio-s, finishing wait\n");
        finish_wait(&dev->reader_queue, &wait);
        PDEBUG("getting first entry\n");
        entry = list_entry(dev->bio_list.next, struct bdtun_bio_list_entry, list);
        spin_unlock(&dev->bio_list_lock);

        // TODO: multithreading backend? At least, strict, strong, emphasized protocol definition notification
        if (count == BDTUN_TXREQ_HEADER_SIZE) {
                req = (struct bdtun_txreq *)buf;
                req->flags  = entry->bio->bi_rw;
                req->offset = entry->bio->bi_sector * KERNEL_SECTOR_SIZE;
                req->size   = entry->bio->bi_size;
        } else if (bio_data_dir(entry->bio) == WRITE &&
                   count == entry->bio->bi_size)
        {
                /* Transfer bio data. */
                bio_for_each_segment(bvec, entry->bio, i) {
                        void *kaddr = kmap(bvec->bv_page);
                        
                        // TODO: its too expensive, nocopy should be implemented
                        if(copy_to_user(buf+pos, kaddr+bvec->bv_offset,
                                        bvec->bv_len) != 0)
                        {
                                PDEBUG("error copying data to user\n");
                                kunmap(bvec->bv_page);
                                return -EIO;
                        }
                        kunmap(bvec->bv_page);
                        pos += bvec->bv_len;
                }
        } else {
                PDEBUG("request size is invalid, returning -EIO\n");
                return -EIO;
        }

        return count;
}

static ssize_t bdtunch_write(struct file *filp, const char *buf, size_t count, loff_t *offset)
{
        struct bdtun *dev = (struct bdtun *)(filp->private_data);
        struct bdtun_bio_list_entry *entry;
        struct bio_vec *bvec;
        unsigned long duration;
        unsigned long pos = 0;
        int cpu;
        int rw;
        int i;

        spin_lock(&dev->bio_list_lock);
        if (list_empty(&dev->bio_list)) {
                spin_unlock(&dev->bio_list_lock);
                PDEBUG("got write on empty list, returning -EIO");
                return -EIO;
        }
        entry = list_entry(dev->bio_list.next, struct bdtun_bio_list_entry, list);
        spin_unlock(&dev->bio_list_lock);

        /* Validate request size */
        if (count < 1 ||
            (bio_data_dir(entry->bio) == READ && !buf[0] && count != entry->bio->bi_size + 1) ||
            (bio_data_dir(entry->bio) == READ &&  buf[0] && count != 1) ||
            (bio_data_dir(entry->bio) == WRITE && count != 1)) {
                PDEBUG("invalid request size from user returning -EIO and resetting bio.\n");
                return -EIO;
        }
        
        /* Read completion byte */
        if (buf[0]) {
                PDEBUG("user process explicitly signaled failure, failing bio.\n");
                spin_lock(&dev->bio_list_lock);
                list_del(&entry->list);
                spin_unlock(&dev->bio_list_lock);
                bio_endio(entry->bio, -EIO);

                // TODO: factor this into a function.
                rw = bio_data_dir(entry->bio);
                duration = jiffies - entry->start_time;
                cpu = part_stat_lock();
                part_stat_add(cpu, &dev->bd_gd->part0, ticks[rw], duration);
                part_round_stats(cpu, &dev->bd_gd->part0);
                part_dec_in_flight(&dev->bd_gd->part0, rw);
                part_stat_unlock();

                kfree(entry);
                return 0;
        }
        
        buf++;
        
        /* Copy the data into the bio */
        if (bio_data_dir(entry->bio) == READ) {
                bio_for_each_segment(bvec, entry->bio, i) {
                        void *kaddr = kmap(bvec->bv_page);
                        // TODO: too expensive
                        if(copy_from_user(kaddr+bvec->bv_offset,
                                          buf+pos, bvec->bv_len) != 0)
                        {
                                PDEBUG("error copying data from user\n");
                                kunmap(bvec->bv_page);
                                /* We do not complete the bio here,
                                 * so the user process can try again */
                                return -EFAULT;
                        }
                        kunmap(bvec->bv_page);
                        pos += bvec->bv_len;
                }
        }

        spin_lock(&dev->bio_list_lock);
        list_del(&entry->list);
        spin_unlock(&dev->bio_list_lock);
        
        bio_endio(entry->bio, 0);
        
        // TODO: factor this into a function.
        rw = bio_data_dir(entry->bio);
        duration = jiffies - entry->start_time;
        cpu = part_stat_lock();
        part_stat_add(cpu, &dev->bd_gd->part0, ticks[rw], duration);
        part_round_stats(cpu, &dev->bd_gd->part0);
        part_dec_in_flight(&dev->bd_gd->part0, rw);
        part_stat_unlock();

        kfree(entry);
        
        return count;
}

unsigned int bdtunch_poll(struct file *filp, poll_table *wait) {
        struct bdtun *dev = filp->private_data;
        unsigned int mask = 0;
        
        poll_wait(filp, &dev->reader_queue, wait);
        
        mask |= POLLOUT | POLLWRNORM; /* writable */
        
        spin_lock(&dev->bio_list_lock);
        if (!list_empty(&dev->bio_list)) {
                PDEBUG("list is not empty, setting mask\n");
                mask |= POLLIN | POLLRDNORM; /* readable */
        }
        spin_unlock(&dev->bio_list_lock);
        
        return mask;
}

/*
 * Operations on character device
 */
static struct file_operations bdtunch_ops = {
        .read    = bdtunch_read,
        .write   = bdtunch_write,
        .open    = bdtunch_open,
        .release = bdtunch_release,
        .poll    = bdtunch_poll
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
static int bdtun_create_k(const char *name, uint64_t block_size, uint64_t size, int capabilities)
{
        struct bdtun *new;
        struct request_queue *queue;
        int error;
        int bd_major;
        int qflags = 0;
        char charname[BDEVNAME_SIZE + 5];
        char qname[BDEVNAME_SIZE + 3];
        struct bdtun_add_disk_work *add_disk_work;
        
        /* Check if device exist */
        
        // TODO: relying on the fact, that only one process can open the control chardev at the same time. But what if,
        // the process forks itself AFTER? We should use Locks here too
        if (bdtun_find_device(name)) {
                return -EEXIST;
        }
        
        if (block_size < 512 || block_size > PAGE_SIZE) {
                return -EINVAL;
        }
        
        /* Check if block_size is a power of two */
        if (block_size & (block_size - 1)) {
                return -EINVAL;
        }
        
        /* Check if size is a multiple of block_size */
        if (size % block_size) {
                return -EINVAL;
        }
        
        /*
         * Set up character device and workqueue name
         */
        PDEBUG("setting up names\n");
        strncpy(charname, name, BDEVNAME_SIZE);
        strcat(charname, "_tun");
        strncpy(qname, name, BDEVNAME_SIZE);
        strcat(qname, "_q");
        
        /*
         * Allocate device structure
         */
        PDEBUG("allocating device sructure\n");
        new = kmalloc(sizeof (struct bdtun), GFP_KERNEL);
        if (new == NULL) {
                PDEBUG("Could not allocate memory for device structure\n");
                error = -ENOMEM;
                goto out;
        }

        mutex_init(&new->mutex);
        if (mutex_lock_interruptible(&new->mutex)) {
                error = -ERESTARTSYS;
                goto out;
        }

        
        /*
         * Determine device size
         */
        new->bd_block_size = block_size;
        new->bd_nsectors   = size / block_size;
        new->bd_size       = size;
        
        /*
         * Semaphores and stuff like that 
         */
        spin_lock_init(&new->bio_list_lock);
        spin_lock_init(&new->lock);
        
        new->removing = 0;
        new->ucnt     = 0;
        new->ch_ucnt  = 0;
        
        /*
         * Wait queue
         */
        init_waitqueue_head(&new->reader_queue);
        
        /*
         * Bio list
         */
        INIT_LIST_HEAD(&new->bio_list);
        
        /*
         * Get a request queue.
         */
        PDEBUG("allocating queue\n");
        queue = blk_alloc_queue(GFP_KERNEL);

        if (queue == NULL) {
                PDEBUG("Could not allocate request queue\n");
                error = -ENOMEM;
                goto out_kfree;
        }
        
        PDEBUG("setting up queue parameters\n");
        // TODO: implicit cast, not nice
        new->capabilities = capabilities;
        queue->queuedata = new;
        blk_queue_make_request(queue, bdtun_make_request);
        blk_queue_logical_block_size(queue, block_size);
        if (capabilities & BDTUN_FLUSH) {
                qflags |= REQ_FLUSH;
        }
        if (capabilities & BDTUN_FUA) {
                qflags |= REQ_FUA;
        }
        if (qflags) {
                blk_queue_flush(queue, qflags);
        }
        if (capabilities & BDTUN_DISCARD) {
                blk_queue_discard(queue);
        }
        if (capabilities & BDTUN_SECURE) {
                blk_queue_secdiscard(queue);
        }
        
        /*
         * Get registered.
         */
        PDEBUG("registering blovk device\n");
        bd_major = register_blkdev(0, "bdtun");
        if (bd_major < 0) {
                PDEBUG("unable to get major number\n");
                error = bd_major;
                goto out_cleanup_queue;
        }

        /*
         * Initialize character device
         */
        PDEBUG("setting up char device\n");
        error = alloc_chrdev_region(&new->ch_num, 0, 1, charname);
        if (error) {
                PDEBUG("could not allocate character device number\n");
                goto out_unregister_blkdev;
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
         * Set up the gendisk structure.
         */
        PDEBUG("allocating the gendisk structure\n");
        // TODO: factor out BDTUN_BD_MINORS as a module parameter
        new->bd_gd = alloc_disk(BDTUN_BD_MINORS);
        if (!new->bd_gd) {
                PDEBUG("Unable to alloc_disk()\n");
                error = -ENOMEM;
                goto out_cdev_del;
        }
        
        new->bd_gd->major = bd_major;
        new->bd_gd->first_minor = 0;
        new->bd_gd->fops = &bdtun_ops;
        new->bd_gd->private_data = new;
        new->bd_gd->queue = queue;
        strcpy(new->bd_gd->disk_name, name);
        set_capacity(new->bd_gd, new->bd_size / KERNEL_SECTOR_SIZE);

        /*
         * Add device to the list
         */
        list_add_tail(&new->list, &device_list);
        
        /*
         * Register the disk in a work queue.
         */
        add_disk_work = kmalloc(sizeof(struct bdtun_add_disk_work), GFP_KERNEL);
        
        // TODO: what if an instant REMOVE operation arrives BEFORE bdtun_do_add_disk could run? synchronization
        // every other operation should hold the synch primitive, which relies on the existence of the device file
        INIT_WORK(&add_disk_work->work, bdtun_do_add_disk);
        add_disk_work->gd = new->bd_gd;
        add_disk_work->mutex = &new->mutex;
        queue_work(add_disk_q, &add_disk_work->work);
        
        PDEBUG("finished setting up device %s\n", name);
        
        try_module_get(THIS_MODULE);
        
        return 0;
        
        out_cdev_del:
                cdev_del(&new->ch_dev);
        out_unregister_chrdev_region:
                unregister_chrdev_region(new->ch_num, 1);
        out_unregister_blkdev:
                unregister_blkdev(bd_major, "bdtun");
        out_cleanup_queue:
                blk_cleanup_queue(queue);
        out_kfree:
                kfree(new);
                mutex_unlock(&new->mutex);
        out:
                return error;
}

static int bdtun_resize_k(const char *name, uint64_t size)
{
        struct bdtun *dev;
        int ret;
        
        dev = bdtun_find_device(name);
        
        if (dev == NULL) {
                PDEBUG("error removing '%s': no such device\n", name);
                return -ENOENT;
        }
        
        if (mutex_lock_interruptible(&dev->mutex)) {
                return -ERESTARTSYS;
        }
        
        set_capacity(dev->bd_gd, size / KERNEL_SECTOR_SIZE);
        // TODO: Needs to be run AFTER add_disk, needs to synchronize, see also bdtun_create_k TODO
        ret = revalidate_disk(dev->bd_gd);
        
        if (ret) {
                PDEBUG("could not revalidate_disk after capacity change\n");
                mutex_unlock(&dev->mutex);
                return ret;
        }
        
        dev->bd_size = size;
        mutex_unlock(&dev->mutex);
        
        return 0;
}


static void bdtun_remove_dev(struct bdtun *dev)
{
        /* Destroy character devices */
        PDEBUG("removing char device\n");
        unregister_chrdev_region(dev->ch_num, 1);
        cdev_del(&dev->ch_dev);
        PDEBUG("device shutdown finished\n");
        
        /* Unreg device object and class if needed */
        device_destroy(chclass, dev->ch_num);

        /* Destroy block devices */
        PDEBUG("removing block device\n");
        unregister_blkdev(dev->bd_gd->major, "bdtun");
        blk_cleanup_queue(dev->bd_gd->queue);
        del_gendisk(dev->bd_gd);
        put_disk(dev->bd_gd);
        
        module_put(THIS_MODULE);
}

static int bdtun_remove_k(const char *name)
{
        struct bdtun *dev;
        struct bdtun_bio_list_entry *entry;
        struct mutex *dmutex;
        
        dev = bdtun_find_device(name);
        
        if (dev == NULL) {
                PDEBUG("error removing '%s': no such device\n", name);
                return -ENOENT;
        }

        spin_lock(&dev->lock);
                
        // TODO: removing should run only once, dev->removing flag could be used to check this
        if (dev->ucnt) {
                spin_unlock(&dev->lock);
                return -EBUSY;
        }
        dev->removing = 1;
        spin_unlock(&dev->lock);
        
        while (!list_empty(&dev->bio_list)) {
                entry = list_entry(dev->bio_list.next, struct bdtun_bio_list_entry, list);
                bio_endio(entry->bio, -EIO);
                list_del(dev->bio_list.next);
                kfree(entry);
        }

        if (mutex_lock_interruptible(&dev->mutex)) {
                return -ERESTARTSYS;
        }

        dmutex = &dev->mutex;
        
        bdtun_remove_dev(dev);
        
        /* Unlink and free device structure */
        list_del(&dev->list);
        kfree(dev);
        mutex_unlock(dmutex);
        PDEBUG("device removed from list\n");

        return 0;
}

static int bdtun_info_k(char *name, struct bdtun_info *device_info)
{
        struct bdtun *dev;
        
        dev = bdtun_find_device(name);
        
        if (dev == NULL) {
                return -ENOENT;
        }
        
        device_info->bd_size       = dev->bd_size;
        device_info->bd_block_size = dev->bd_block_size;
        device_info->bd_major      = dev->bd_gd->major;
        device_info->bd_minor      = dev->bd_gd->first_minor;
        device_info->ch_major      = MAJOR(dev->ch_num);
        device_info->ch_minor      = MINOR(dev->ch_num);
        device_info->capabilities  = dev->capabilities;
        
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
                if (i > maxdevices || i > maxdevices_internal) {
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
        if (mutex_lock_interruptible(&mutex)) {
                return -ERESTARTSYS;
        }
        return 0;
}

static int ctrl_release(struct inode *inode, struct file *filp)
{
        PDEBUG("got device_release on master dev\n");
        mutex_unlock(&mutex);
        return 0;
}

// TODO: these can cause race-conditions. refactor with mutex
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
                ret = bdtun_create_k(c->create.name, c->create.blocksize, c->create.size, c->create.capabilities);
                
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
                if (count < BDTUN_COMM_RESIZE_SIZE) {
                        return -EIO;
                }
                
                ret = bdtun_resize_k(c->resize.name, c->resize.size);
                
                if (ret != 0) {
                        return ret;
                }
                
                break;
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
        
        mutex_init(&mutex);
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
