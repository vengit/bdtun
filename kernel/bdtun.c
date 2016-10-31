/*
 * A block device taht forwards bios to userspace
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
#include <linux/mm.h>

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
};

/*
 * Device class for char device registration
 */
static struct class *chclass;
static struct device *ctrl_device;
static int ctrl_devnum;
static atomic_t ctrl_lock = ATOMIC_INIT(1);

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

        /* bios waiting for processing */
        struct list_head incoming_bio_list;
        struct list_head pending_bio_list;
        struct list_head *data_current_bio;
        wait_queue_head_t reader_queue;

        /* These locks should be grabbed every time the lists
         * itselves or the data_current_bio variable are manipulated.
         * Alwasy grab incoming first, then pending. */
        struct mutex incoming_bio_list_lock;
        struct mutex pending_bio_list_lock;

        /* When this flag is 1, the device will be removed shortly.
         * The device should be treated as nonexistent, e.g. shouldn't
         * let to be opened. */
        int removing;
        /* Use count of the device. Incremented on open,
         * decremented on close. Ops on block and tunnel devices
         * counted. */
        int ucnt;
        /* This lock should be held when manipulating the above two
         * variables. */
        spinlock_t ru_lock;

        /* This variable signals the completion of the add_disk call. */
        atomic_t add_disk_finished;

        /* Block device related stuff */
        uint64_t bd_size;
        uint64_t bd_block_size;
        uint64_t bd_nsectors;
        int capabilities;
        struct gendisk *bd_gd;
};

/*
 * Disk add work queue.
 */
struct bdtun_add_disk_work {
        struct bdtun *dev;
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
 * 
 * TODO: we really need to define this? Is there a kernel macro
 * for this?
 */
#define KERNEL_SECTOR_SIZE 512

static void bdtun_do_add_disk(struct work_struct *work)
{
        struct bdtun_add_disk_work *w = container_of(work, struct bdtun_add_disk_work, work);

        PDEBUG("bdtun_do_add_disk: called\n");

        add_disk(w->dev->bd_gd);
        atomic_set(&w->dev->add_disk_finished, 1);
        kfree(w);

        PDEBUG("bdtun_do_add_disk: finished\n");
}

/*
 * Request processing
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
#       define MKREQ_RETTYPE int
#       define MKREQ_RETVAL 0
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
#       define MKREQ_RETTYPE blk_qc_t
#       define MKREQ_RETVAL BLK_QC_T_NONE;
#else
#       define MKREQ_RETTYPE void
#       define MKREQ_RETVAL
#endif

static MKREQ_RETTYPE bdtun_make_request(struct request_queue *q, struct bio *bio)
{
        struct bdtun *dev = (struct bdtun *)(q->queuedata);
        struct bdtun_bio_list_entry *new;
        const int rw = bio_data_dir(bio);
        int cpu;

        PDEBUG("bdtun_make_request: called\n");

        new = kmalloc(sizeof(struct bdtun_bio_list_entry), GFP_KERNEL);

        if (!new) {
                PDEBUG("bdtun_make_request: could not allocate bio list entry\n");
                bio_io_error(bio);
                return MKREQ_RETVAL;
        }

        new->start_time = jiffies;
        new->bio = bio;

        mutex_lock(&dev->incoming_bio_list_lock);
        list_add_tail(&new->list, &dev->incoming_bio_list);
        mutex_unlock(&dev->incoming_bio_list_lock);

        wake_up(&dev->reader_queue);
        PDEBUG("bdtun_make_request: request queued\n");

        cpu = part_stat_lock();
        part_stat_inc(cpu, &dev->bd_gd->part0, ios[rw]);
        part_stat_add(cpu, &dev->bd_gd->part0, sectors[rw], bio_sectors(bio));
        part_inc_in_flight(&dev->bd_gd->part0, rw);
        part_stat_unlock();
        PDEBUG("bdtun_make_request: written disk stats (finished)\n");

        return MKREQ_RETVAL;
}

static int bdtun_open(struct block_device *bdev, fmode_t mode)
{
        struct bdtun *dev = (struct bdtun *)(bdev->bd_disk->queue->queuedata);

        PDEBUG("bdtun_open: called\n");

        spin_lock(&dev->ru_lock);
        if (dev->removing) {
                spin_unlock(&dev->ru_lock);
                PDEBUG("bdtun_open: device is being removed\n");
                return -ENOENT;
        }
        dev->ucnt++;
        spin_unlock(&dev->ru_lock);

        PDEBUG("bdtun_open: finished\n");
        return 0;
}

static void bdtun_release(struct gendisk *gd, fmode_t mode)
{
        struct bdtun *dev = (struct bdtun *)(gd->queue->queuedata);

        PDEBUG("bdtun_release: called\n");

        spin_lock(&dev->ru_lock);
        dev->ucnt--;
        spin_unlock(&dev->ru_lock);

        PDEBUG("bdtun_release: finished\n");
}

static int bdtun_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
        long size;
        struct bdtun *dev = bdev->bd_disk->private_data;

        PDEBUG("bdtun_getgeo: called\n");

        /* We are a virtual device, so we have to make up something.
         * We claim to have 16 sectors, 4 head, and the appropriate
         * number of cylinders
         */
         size = dev->bd_size*(dev->bd_block_size/KERNEL_SECTOR_SIZE);
         geo->cylinders = (size & 0x3f) >> 6;
         geo->heads = 4;
         geo->sectors = 16;
         geo->start = 0;

         PDEBUG("bdtun_getgeo: finished\n");

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
 * Called when tunnel character device is opened
 */
static int bdtunch_open(struct inode *inode, struct file *filp)
{
        struct bdtun *dev = container_of(inode->i_cdev, struct bdtun, ch_dev);

        PDEBUG("bdtunch_open: got device_open on char dev\n");

        spin_lock(&dev->ru_lock);
        if (dev->removing) {
                spin_unlock(&dev->ru_lock);
                PDEBUG("bdtunch_open: device is removing\n");
                return -EBUSY;
        }
        dev->ucnt++;
        spin_unlock(&dev->ru_lock);

        filp->private_data = (void *)dev;

        PDEBUG("bdtunch_open: finished\n");
        return 0;
}

/*
 *  Called when tunnel character device is closed
 */
static int bdtunch_release(struct inode *inode, struct file *filp)
{
        struct bdtun *dev = container_of(inode->i_cdev, struct bdtun, ch_dev);

        PDEBUG("bdtunch_release: got device_release on char dev\n");

        mutex_lock(&dev->incoming_bio_list_lock);
        mutex_lock(&dev->pending_bio_list_lock);
        list_splice_tail_init(&dev->pending_bio_list, &dev->incoming_bio_list);
        mutex_unlock(&dev->pending_bio_list_lock);
        mutex_unlock(&dev->incoming_bio_list_lock);

        spin_lock(&dev->ru_lock);
        dev->ucnt--;
        spin_unlock(&dev->ru_lock);

        PDEBUG("bdtunch_release: finished\n");

        return 0;
}

/*
 * Translates the flags stored in struct bio's bi_rw field
 * This is needed to ensure that the userspace will be compatible
 * with the kernel module even if the kernel is changed and the
 * module is recompiled.
 */
// TODO: make a macro that checks the existence of stuff and only
// compiles what necessary
static unsigned long bdtun_translate_bio_rw(unsigned long rw) {
        return
        (rw & REQ_WRITE ? BDTUN_REQ_WRITE : 0 ) |
        (rw & REQ_FAILFAST_DEV ? BDTUN_REQ_FAILFAST_DEV : 0 ) |
        (rw & REQ_FAILFAST_TRANSPORT ? BDTUN_REQ_FAILFAST_TRANSPORT : 0 ) |
        (rw & REQ_FAILFAST_DRIVER ? BDTUN_REQ_FAILFAST_DRIVER : 0 ) |
        (rw & REQ_SYNC ? BDTUN_REQ_SYNC : 0 ) |
        (rw & REQ_META ? BDTUN_REQ_META : 0 ) |
        // TODO: duck check instead of version check
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0)
        (rw & REQ_PRIO ? BDTUN_REQ_PRIO : 0 ) |
#endif
        (rw & REQ_DISCARD ? BDTUN_REQ_DISCARD : 0 ) |
        (rw & REQ_NOIDLE ? BDTUN_REQ_NOIDLE : 0 ) |
        (rw & REQ_RAHEAD ? BDTUN_REQ_RAHEAD : 0 ) |
        (rw & REQ_THROTTLED ? BDTUN_REQ_THROTTLED : 0 ) |
        (rw & REQ_SORTED ? BDTUN_REQ_SORTED : 0 ) |
        (rw & REQ_SOFTBARRIER ? BDTUN_REQ_SOFTBARRIER : 0 ) |
        (rw & REQ_FUA ? BDTUN_REQ_FUA : 0 ) |
        (rw & REQ_NOMERGE ? BDTUN_REQ_NOMERGE : 0 ) |
        (rw & REQ_STARTED ? BDTUN_REQ_STARTED : 0 ) |
        (rw & REQ_DONTPREP ? BDTUN_REQ_DONTPREP : 0 ) |
        (rw & REQ_QUEUED ? BDTUN_REQ_QUEUED : 0 ) |
        (rw & REQ_ELVPRIV ? BDTUN_REQ_ELVPRIV : 0 ) |
        (rw & REQ_FAILED ? BDTUN_REQ_FAILED : 0 ) |
        (rw & REQ_QUIET ? BDTUN_REQ_QUIET : 0 ) |
        (rw & REQ_PREEMPT ? BDTUN_REQ_PREEMPT : 0 ) |
        (rw & REQ_ALLOCED ? BDTUN_REQ_ALLOCED : 0 ) |
        (rw & REQ_COPY_USER ? BDTUN_REQ_COPY_USER : 0 ) |
        (rw & REQ_FLUSH ? BDTUN_REQ_FLUSH : 0 ) |
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0)
        (rw & REQ_FLUSH_SEQ ? BDTUN_REQ_FLUSH_SEQ : 0 ) |
#endif
        (rw & REQ_IO_STAT ? BDTUN_REQ_IO_STAT : 0 ) |
        (rw & REQ_MIXED_MERGE ? BDTUN_REQ_MIXED_MERGE : 0 ) |
        (rw & REQ_SECURE ? BDTUN_REQ_SECURE : 0);
}

/*
 * Sends metadata of the current bio in the incoming list, or data of
 * the data-current bio
 */
static ssize_t bdtunch_read(struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
        struct bdtun *dev = filp->private_data;
        struct bdtun_bio_list_entry *entry;
        struct bdtun_txreq *req;
        unsigned long pos = 0;
        struct bio_vec bvec;
        DEFINE_WAIT(wait);
        struct bvec_iter i;

        PDEBUG("bdtunch_read: called\n");

        /* Read request info */
        if (count == BDTUN_TXREQ_HEADER_SIZE) {
                PDEBUG("bdtunch_read: got header request\n");

                out_list_is_empty:

                PDEBUG("bdtunch_read: Preparing to wait\n");
                prepare_to_wait(&dev->reader_queue, &wait, TASK_INTERRUPTIBLE);

                PDEBUG("bdtunch_read: grabbing lock on incoming list\n");
                mutex_lock(&dev->incoming_bio_list_lock);

                if (list_empty(&dev->incoming_bio_list)) {
                        PDEBUG("bdtunch_read: list empty, releasing lock for out queue\n");
                        mutex_unlock(&dev->incoming_bio_list_lock);
                        PDEBUG("bdtunch_read: calling schedule\n");
                        schedule();
                        PDEBUG("bdtunch_read: awaken, finishing wait\n");
                        finish_wait(&dev->reader_queue, &wait);

                        PDEBUG("bdtunch_read: checking for pending signals\n");
                        if (signal_pending(current)) {
                                PDEBUG("bdtunch_read: signals are pending, returning -EINTR (finished)\n");
                                return -EINTR;
                        }

                        PDEBUG("bdtunch_read: no pending signals, checking out queue again\n");
                        goto out_list_is_empty;
                }

                PDEBUG("bdtunch_read: list containts bio-s, finishing wait\n");
                finish_wait(&dev->reader_queue, &wait);

                entry = list_entry(
                        dev->incoming_bio_list.next,
                        struct bdtun_bio_list_entry, list
                );

                req = (struct bdtun_txreq *)buf;
                req->id     = (uintptr_t)entry;
                req->flags  = bdtun_translate_bio_rw(entry->bio->bi_rw);
                req->offset = entry->bio->bi_iter.bi_sector * KERNEL_SECTOR_SIZE;
                req->size   = bio_sectors(entry->bio);

                mutex_lock(&dev->pending_bio_list_lock);
                list_move_tail(&entry->list, &dev->pending_bio_list);
                dev->data_current_bio = &dev->pending_bio_list;
                mutex_unlock(&dev->pending_bio_list_lock);

                mutex_unlock(&dev->incoming_bio_list_lock);

                PDEBUG("bdtunch_read: header request served (finished)\n");
                return count;
        }

        PDEBUG("bdtunch_read: got non-header request, getting current bio\n");
        mutex_lock(&dev->pending_bio_list_lock);
        if (dev->data_current_bio == NULL) {
            PDEBUG("bdtunch_read: data current bio is not set");
            mutex_unlock(&dev->pending_bio_list_lock);
            return -EIO;
        }
        entry = list_entry(
                dev->data_current_bio,
                struct bdtun_bio_list_entry, list
        );
        mutex_unlock(&dev->pending_bio_list_lock);

        /* Read payload */
        if (bio_data_dir(entry->bio) == WRITE && count == bio_sectors(entry->bio))
        {
                PDEBUG("bdtunch_read: got write request, trasferring data\n");
                /* Transfer bio data. */
                bio_for_each_segment(bvec, entry->bio, i) {
                        void *kaddr = kmap(bvec.bv_page);

                        if(copy_to_user(buf+pos, kaddr+bvec.bv_offset,
                                        bvec.bv_len) != 0)
                        {
                                kunmap(bvec.bv_page);
                                PDEBUG("bdtunch_read: error copying data to user (finished)\n");
                                return -EIO;
                        }
                        if (bvec.bv_offset || bvec.bv_len != PAGE_SIZE) {
                            PDEBUG("bdtunch_read: WARNING!!! Misaligned data: offset: %d, size: %d\n", bvec.bv_offset, bvec.bv_len);
                        }
                        kunmap(bvec.bv_page);
                        pos += bvec.bv_len;
                }
                PDEBUG("bdtunch_read: transfer completed (finished)\n");
                return count;
        }

        /* Error */
        PDEBUG("bdtunch_read: request size is invalid, returning -EIO (finished)\n");

        return -EIO;
}

void bdtun_update_iostat(struct bdtun *dev, struct bdtun_bio_list_entry *entry)
{
        int rw = bio_data_dir(entry->bio);
        unsigned long duration = jiffies - entry->start_time;
        int cpu = part_stat_lock();

        PDEBUG("bdtun_update_iostat: called\n");

        part_stat_add(cpu, &dev->bd_gd->part0, ticks[rw], duration);
        part_round_stats(cpu, &dev->bd_gd->part0);
        part_dec_in_flight(&dev->bd_gd->part0, rw);
        part_stat_unlock();

        PDEBUG("bdtun_update_iostat: finished\n");
}

static ssize_t bdtunch_write(struct file *filp, const char *buf, size_t count, loff_t *offset)
{
        struct bdtun *dev = (struct bdtun *)(filp->private_data);
        struct bdtun_bio_list_entry *entry;
        struct bio_vec bvec;
        unsigned long pos = 0;
        struct bvec_iter i;

        PDEBUG("bdtunch_write: called\n");

        /* 2. It's a "set data-current bio" request */
        if (count == sizeof(uintptr_t)) {
                PDEBUG("bdtunch_write: setting current bio\n");
                mutex_lock(&dev->pending_bio_list_lock);
                dev->data_current_bio = (struct list_head *)*(uintptr_t *)buf;
                mutex_unlock(&dev->pending_bio_list_lock);
                PDEBUG("bdtunch_write: current bio set successfully (finished)\n");
                return count;
        }

        mutex_lock(&dev->pending_bio_list_lock);
        if (dev->data_current_bio == NULL) {
            mutex_unlock(&dev->pending_bio_list_lock);
            PDEBUG("bdtunch_write: data current bio is not set");
            return -EIO;
        }
        entry = list_entry(dev->data_current_bio, struct bdtun_bio_list_entry, list);
        mutex_unlock(&dev->pending_bio_list_lock);

        /* 1. It's a completion byte. */
        if (count == 1) {
                mutex_lock(&dev->pending_bio_list_lock);
                if (buf[0]) {
                        PDEBUG("bdtunch_write: user signaled failure, failing bio.\n");
                        bio_io_error(entry->bio);
                } else {
                        PDEBUG("bdtunch_write: user signaled completion, completing bio.\n");
                        bio_endio(entry->bio);
                }
                bdtun_update_iostat(dev, entry);
                /* If we're completing the current bio, step ahead. */
                list_del(&entry->list);
                mutex_unlock(&dev->pending_bio_list_lock);
                kfree(entry);

                PDEBUG("bdtunch_write: finished\n");
                return count;
        }

        /* 3. It's data */
        if (bio_data_dir(entry->bio) == READ &&
            count == bio_sectors(entry->bio)) {
                PDEBUG("bdtunch_write: got data read request\n");
                bio_for_each_segment(bvec, entry->bio, i) {
                        void *kaddr = kmap(bvec.bv_page);
                        if(copy_from_user(kaddr+bvec.bv_offset,
                                          buf+pos, bvec.bv_len) != 0)
                        {
                                PDEBUG("bdtunch_write: error copying data from user (finished)\n");
                                kunmap(bvec.bv_page);
                                /* We do not complete the bio here,
                                 * so the user process can try again */
                                return -EIO;
                        }
                        kunmap(bvec.bv_page);
                        pos += bvec.bv_len;
                }
                return count;
        }

        /* 4. It's an error */
        PDEBUG("bdtunch_write: invalid request size from user returning -EIO. (finished)\n");

        return -EIO;
}

unsigned int bdtunch_poll(struct file *filp, poll_table *wait) {
        struct bdtun *dev = filp->private_data;
        unsigned int mask = 0;

        PDEBUG("bdtunch_poll: called\n");

        poll_wait(filp, &dev->reader_queue, wait);

        mask |= POLLOUT | POLLWRNORM; /* writable */

        mutex_lock(&dev->incoming_bio_list_lock);
        if (!list_empty(&dev->incoming_bio_list)) {
                PDEBUG("bdtunch_poll: list is not empty, setting mask\n");
                mask |= POLLIN | POLLRDNORM; /* readable */
        }
        mutex_unlock(&dev->incoming_bio_list_lock);

        PDEBUG("bdtunch_poll: called\n");

        return mask;
}

/*
 * mmap call handler. Sets up the memory mapping to the current bio
 * according to the nopage method.
 */
static int bdtunch_mmap(struct file *filp, struct vm_area_struct *vma)
{
        struct bdtun *dev = (struct bdtun *)filp->private_data;
        unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
        struct bdtun_bio_list_entry *entry;
        struct bio_vec bvec;
        int pos = 0;
        struct bvec_iter i;

        PDEBUG("bdtunch_mmap: Offset isn't aligned: %ld\n", offset);

        if (offset & ~PAGE_MASK) {
                PDEBUG("bdtunch_mmap: Offset isn't aligned: %ld (finished)\n", offset);
                return -ENXIO;
        }

        if (!(vma->vm_flags & VM_SHARED))
        {
                PDEBUG("bdtunch_mmap: Mappings must be shared. (finished)\n");
                return -EINVAL;
        }

        vma->vm_flags |= VM_LOCKED;

        mutex_lock(&dev->pending_bio_list_lock);
        entry = list_entry(dev->data_current_bio, struct bdtun_bio_list_entry, list);
        mutex_unlock(&dev->pending_bio_list_lock);

        // Check size validity
        if (vma->vm_end - vma->vm_start != entry->bio->bi_vcnt * PAGE_SIZE) {
                PDEBUG("bdtunch_mmap: error: invalid vma size %lu instead of %lu (finished)\n",
                       vma->vm_end - vma->vm_start, entry->bio->bi_vcnt * PAGE_SIZE);
                return -EIO;
		}

        bio_for_each_segment(bvec, entry->bio, i) {
                if (remap_pfn_range(vma,
                        vma->vm_start + PAGE_SIZE * pos,
                        page_to_pfn(bvec.bv_page),
                        PAGE_SIZE, PAGE_SHARED) < 0)
                {
                        PDEBUG("bdtunch_mmap: remap_pfn_range failed (finished)\n");
                        return -EIO;
                }
                pos += 1;
        }

        vma->vm_private_data = (void *)dev;

        PDEBUG("bdtunch_mmap: finished\n");
        return 0;
}

/*
 * Operations on the tunnel's character device
 */
static struct file_operations bdtunch_ops = {
        .read    = bdtunch_read,
        .write   = bdtunch_write,
        .open    = bdtunch_open,
        .release = bdtunch_release,
        .poll    = bdtunch_poll,
        .mmap    = bdtunch_mmap
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

        PDEBUG("bdtun_create_k: called\n");

        /* Check if device exist */
        if (bdtun_find_device(name)) {
                PDEBUG("bdtun_create_k: device %s already exsits (finished)\n", name);
                return -EEXIST;
        }

        /* Check block size limits */
        if (block_size < 512 || block_size > PAGE_SIZE) {
                PDEBUG("bdtun_create_k: block size must be between 512 and PAGE_SIZE (%lu in this case) (finished)\n", PAGE_SIZE);
                return -EINVAL;
        }

        /* Check if block_size is a power of two */
        if (block_size & (block_size - 1)) {
                PDEBUG("bdtun_create_k: block size must be a power of two (finished)\n");
                return -EINVAL;
        }

        /* Check if size is a multiple of block_size */
        if (size % block_size) {
                PDEBUG("bdtun_create_k: size must be a multiple of the block size (finished)");
                return -EINVAL;
        }

        /*
         * Set up character device and workqueue name
         */
        PDEBUG("bdtun_create_k: setting up names\n");
        strncpy(charname, name, BDEVNAME_SIZE);
        strcat(charname, "_tun");
        strncpy(qname, name, BDEVNAME_SIZE);
        strcat(qname, "_q");

        /*
         * Allocate device structure
         */
        PDEBUG("bdtun_create_k: allocating device sructure\n");
        new = kmalloc(sizeof (struct bdtun), GFP_KERNEL);
        if (new == NULL) {
                PDEBUG("bdtun_create_k: could not allocate memory for device structure\n");
                error = -ENOMEM;
                goto out;
        }

        /*
         * Determine device size
         */
        new->bd_block_size = block_size;
        new->bd_nsectors   = size / block_size;
        new->bd_size       = size;

        /*
         * Init locks
         */
        mutex_init(&new->incoming_bio_list_lock);
        mutex_init(&new->pending_bio_list_lock);
        spin_lock_init(&new->ru_lock);

        new->ucnt  = 0;
        new->removing = 0;

        atomic_set(&new->add_disk_finished, 0);

        /*
         * Wait queue
         */
        init_waitqueue_head(&new->reader_queue);

        /*
         * Bio list
         */
        INIT_LIST_HEAD(&new->incoming_bio_list);
        INIT_LIST_HEAD(&new->pending_bio_list);
        new->data_current_bio = NULL;

        /*
         * Get a request queue.
         */
        PDEBUG("bdtun_create_k: allocating queue\n");
        queue = blk_alloc_queue(GFP_KERNEL);

        if (queue == NULL) {
                PDEBUG("bdtun_create_k: could not allocate request queue\n");
                error = -ENOMEM;
                goto out_kfree;
        }
        
        PDEBUG("bdtun_create_k: setting up queue parameters\n");

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
                //blk_queue_secdiscard(queue);
                // Supress 'computed value is not used' warning
                test_bit(QUEUE_FLAG_DISCARD, &(queue)->queue_flags);
                test_bit(QUEUE_FLAG_SECDISCARD, &(queue)->queue_flags);
        }
        
        /*
         * Get registered.
         */
        PDEBUG("bdtun_create_k: registering block device\n");
        bd_major = register_blkdev(0, "bdtun");
        if (bd_major < 0) {
                PDEBUG("bdtun_create_k: unable to get major number\n");
                error = bd_major;
                goto out_cleanup_queue;
        }

        /*
         * Initialize character device
         */
        PDEBUG("bdtun_create_k: setting up char device\n");
        error = alloc_chrdev_region(&new->ch_num, 0, 1, charname);
        if (error) {
                PDEBUG("bdtun_create_k: could not allocate character device number\n");
                goto out_unregister_blkdev;
        }
        
        /*
         * Register character device
         */
        cdev_init(&new->ch_dev, &bdtunch_ops);
        new->ch_dev.owner = THIS_MODULE;
        
        PDEBUG("bdtun_create_k: char major %d\n", MAJOR(new->ch_num));
        error = cdev_add(&new->ch_dev, new->ch_num ,1);
        if (error) {
                PDEBUG("bdtun_create_k: error setting up char device\n");
                goto out_unregister_chrdev_region;
        }
        
        /*
         * Add a device node
         */
        
        new->ch_device = device_create(chclass, NULL, new->ch_num, NULL, charname);
        if (IS_ERR(new->ch_device)) {
                PDEBUG("bdtun_create_k: error setting up device object\n");
                error = -ENOMEM;
                goto out_cdev_del;
        }
        
        /*
         * Set up the gendisk structure.
         */
        PDEBUG("bdtun_create_k: allocating the gendisk structure\n");
        // TODO: factor out BDTUN_BD_MINORS as a module parameter
        new->bd_gd = alloc_disk(BDTUN_BD_MINORS);
        if (!new->bd_gd) {
                PDEBUG("bdtun_create_k: unable to alloc_disk()\n");
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
        
        INIT_WORK(&add_disk_work->work, bdtun_do_add_disk);
        add_disk_work->dev = new;
        queue_work(add_disk_q, &add_disk_work->work);

        PDEBUG("bdtun_create_k: finished setting up device %s\n", name);

        try_module_get(THIS_MODULE);

        PDEBUG("bdtun_create_k: finished\n");
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
        out:
                PDEBUG("bdtun_create_k: finished\n");
                return error;
}

static int bdtun_resize_k(const char *name, uint64_t size)
{
        struct bdtun *dev;
        int ret;

        PDEBUG("bdtun_resize_k: called, resizing device %s\n", name);

        dev = bdtun_find_device(name);

        if (dev == NULL) {
                PDEBUG("bdtun_resize_k: error removing '%s': no such device (finished)\n", name);
                return -ENOENT;
        }

        set_capacity(dev->bd_gd, size / KERNEL_SECTOR_SIZE);
        ret = revalidate_disk(dev->bd_gd);

        if (ret) {
                PDEBUG("bdtun_resize_k: could not revalidate_disk after capacity change (finished)\n");
                return ret;
        }

        dev->bd_size = size;

        PDEBUG("bdtun_resize_k: finished");
        return 0;
}

static void bdtun_remove_dev(struct bdtun *dev)
{
        /* Destroy character devices */
        PDEBUG("bdtun_remove_dev: called, removing char device\n");
        unregister_chrdev_region(dev->ch_num, 1);
        cdev_del(&dev->ch_dev);
        PDEBUG("bdtun_remove_dev: device shutdown finished\n");
        
        /* Unreg device object and class if needed */
        device_destroy(chclass, dev->ch_num);

        /* Destroy block devices */
        PDEBUG("bdtun_remove_dev: removing block device\n");
        dev->bd_gd->queue->queuedata = NULL;
        unregister_blkdev(dev->bd_gd->major, "bdtun");
        blk_cleanup_queue(dev->bd_gd->queue);
        del_gendisk(dev->bd_gd);
        put_disk(dev->bd_gd);
        
        module_put(THIS_MODULE);

        PDEBUG("bdtun_remove_dev: finished\n");
}

static int bdtun_remove_k(const char *name)
{
        struct bdtun *dev;

        dev = bdtun_find_device(name);

        PDEBUG("bdtun_remove_k: called\n");

        if (dev == NULL) {
                PDEBUG("bdtun_remove_k: error removing '%s': no such device\n", name);
                return -ENOENT;
        }

        spin_lock(&dev->ru_lock);
        if (dev->removing) {
                spin_unlock(&dev->ru_lock);
                PDEBUG("bdtun_remove_k: won't remove: block device is already being removed (finished)\n");
                return -EBUSY;
        }
        if (dev->ucnt) {
                spin_unlock(&dev->ru_lock);
                PDEBUG("bdtun_remove_k: won't remove: block device is in use (finished)");
                return -EBUSY;
        }
        PDEBUG("bdtun_remove_k: setting the device state to 'removing'");
        dev->removing = 1;
        spin_unlock(&dev->ru_lock);

        bdtun_remove_dev(dev);
        list_del(&dev->list);
        kfree(dev);

        PDEBUG("bdtun_remove_k: device is now really removed.\n");

        return 0;
}

static int bdtun_info_k(char *name, struct bdtun_info *device_info)
{
        struct bdtun *dev;
        
        PDEBUG("bdtun_info_k: called\n");

        dev = bdtun_find_device(name);
        
        if (dev == NULL) {
                PDEBUG("bdtun_info_k: there is no such device (finished)\n");
                return -ENOENT;
        }
        
        device_info->bd_size       = dev->bd_size;
        device_info->bd_block_size = dev->bd_block_size;
        device_info->bd_major      = dev->bd_gd->major;
        device_info->bd_minor      = dev->bd_gd->first_minor;
        device_info->ch_major      = MAJOR(dev->ch_num);
        device_info->ch_minor      = MINOR(dev->ch_num);
        device_info->capabilities  = dev->capabilities;
        
        PDEBUG("bdtun_info_k: finished\n");

        return 0;
}

static int bdtun_list_k(
        char *buf, const int bufsize, const int maxdevices_internal,
        size_t offset, size_t maxdevices)
{
        struct list_head *ptr;
        struct bdtun *entry;
        int i, bufpos, len;

        PDEBUG("bdtun_list_k: called\n");

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

        PDEBUG("bdtun_list_k: finished\n");

        return bufpos;
}

/*
 * Control device functions
 */
static int ctrl_open(struct inode *inode, struct file *filp)
{
        PDEBUG("ctrl_open: got device_open on master dev\n");

        if (!atomic_dec_and_test(&ctrl_lock)) {
                atomic_inc(&ctrl_lock);
                PDEBUG("ctrl_open: control device is busy (finished)\n");
                return -EBUSY;
        }

        PDEBUG("ctrl_open: finished\n");

        return 0;
}

static int ctrl_release(struct inode *inode, struct file *filp)
{
        PDEBUG("ctrl_release: called, got device_release on master dev\n");

        atomic_inc(&ctrl_lock);

        PDEBUG("ctrl_release: finished\n");

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

        PDEBUG("ctrl_read: called");

        if (ctrl_response_size < 0) {
                PDEBUG("ctrl_read: control response size is invalid: %d (finished)\n", ctrl_response_size);
                return -EIO;
        }
        
        if(copy_to_user(buf, ctrl_response_buf, ctrl_response_size) != 0) {
                PDEBUG("error copying data to user in ctrl_read (finished)\n");
                return -EFAULT;
        }
        
        ctrl_response_size = 0;
        
        PDEBUG("ctrl_read: finished");

        return tmp;
}

static ssize_t ctrl_write(struct file *filp, const char *buf, size_t count, loff_t *offset)
{
        struct bdtun_ctrl_command *c;
        struct bdtun_info info;
        int ret;

        PDEBUG("ctrl_read: called");

        if (count < 1) {
                PDEBUG("ctrl_read: received count < 1 (finished)\n");
                return -EIO;
        }
        
        c = (struct bdtun_ctrl_command *) buf;
        
        PDEBUG("ctrl_read: received command: %d\n", c->command);
        
        switch (c->command) {
        case BDTUN_COMM_CREATE:
                if (count < BDTUN_COMM_CREATE_SIZE) {
                        PDEBUG("ctrl_read: invalid commad packet size (finished)\n");
                        return -EIO;
                }
                ret = bdtun_create_k(c->create.name, c->create.blocksize, c->create.size, c->create.capabilities);
                
                if (ret < 0) {
                        PDEBUG("ctrl_read: could not create device (finished)\n");
                        return ret;
                }
                
                break;
        case BDTUN_COMM_REMOVE:
                if (count < BDTUN_COMM_REMOVE_SIZE) {
                        PDEBUG("ctrl_read: Invalid commad packet size (finished)\n");
                        return -EIO;
                }
                
                ret = bdtun_remove_k(c->remove.name);
                
                if (ret < 0) {
                        return ret;
                }
                
                break;
        case BDTUN_COMM_INFO:
                if (count < BDTUN_COMM_INFO_SIZE) {
                        PDEBUG("ctrl_read: invalid commad packet size (finished)\n");
                        return -EIO;
                }
                
                ret = bdtun_info_k(c->info.name, &info);
                
                if (ret != 0) {
                        PDEBUG("ctrl_read: could not get info (finished)\n");
                        return ret;
                }
                
                memcpy(ctrl_response_buf, &info, ctrl_response_size = sizeof(info));
                
                break;
        case BDTUN_COMM_LIST:
                if (count < BDTUN_COMM_LIST_SIZE) {
                        PDEBUG("ctrl_read: invalid commad packet size (finished)\n");
                        return -EIO;
                }
                
                ctrl_response_size = bdtun_list_k(
                        ctrl_response_buf, BDTUN_RESPONSE_SIZE,
                        BDTUN_DEVNAMES, c->list.offset, c->list.maxdevices
                );
                
                break;
        case BDTUN_COMM_RESIZE:
                if (count < BDTUN_COMM_RESIZE_SIZE) {
                        PDEBUG("ctrl_read: invalid commad packet size (finished)\n");
                        return -EIO;
                }
                
                ret = bdtun_resize_k(c->resize.name, c->resize.size);
                
                if (ret != 0) {
                        return ret;
                }
                
                break;
        default:
                PDEBUG("ctrl_read: invalid commad (finished)\n");
                return -EIO;
        }

        PDEBUG("ctrl_read: finished\n");

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

        PDEBUG("__init: called\n");

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
                PDEBUG("__init: error setting up device class\n");
                error = -ENOMEM;
                goto out_adq;
        }

        /*
         * Initialize master character device
         */
        PDEBUG("__init: setting up char device\n");

        error = alloc_chrdev_region(&ctrl_devnum, 0, 1, "bdtun");
        if (error) {
                printk(KERN_ERR "could not allocate control device number\n");
                goto out_destroy_class;
        }
        cdev_init(&ctrl_dev, &ctrl_ops);
        ctrl_dev.owner = THIS_MODULE;
        error = cdev_add(&ctrl_dev, ctrl_devnum ,1);
        if (error) {
                PDEBUG("__init: error setting up control device\n");
                goto out_unregister_chrdev_region;
        }
        
        /*
         * Add a device node
         */
        
        ctrl_device = device_create(chclass, NULL, ctrl_devnum, NULL, "bdtun");
        if (IS_ERR(ctrl_device)) {
                PDEBUG("__init: error setting up control device object\n");
                error = -ENOMEM;
                goto out_cdev_del;
        }

        PDEBUG("__init: finished\n");
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
                PDEBUG("__init: Error during module initialization (finished)\n");
                return -ENOMEM;
}

/*
 * Clean up on module remove
 */
static void __exit bdtun_exit(void)
{
        struct list_head *ptr;

        PDEBUG("__exit: called\n");

        list_for_each(ptr, &device_list)
                bdtun_remove_dev(list_entry(ptr, struct bdtun, list));

        flush_workqueue(add_disk_q);
        destroy_workqueue(add_disk_q);
        device_destroy(chclass, ctrl_devnum);
        cdev_del(&ctrl_dev);
        unregister_chrdev_region(ctrl_devnum, 1);
        class_destroy(chclass);

        PDEBUG("__exit: finished\n");
}

module_init(bdtun_init);
module_exit(bdtun_exit);
