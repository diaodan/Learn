#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include "sbull.h"

MODULE_LICENSE("DUAL BSD/GPL");


static int sbull_open(struct block_device *bdev, fmode_t mode)
{
    INFO();
    return 0;
}

static void sbull_release(struct gendisk *gen, fmode_t mode)
{
    INFO();
}

static int sbull_ioctl(struct block_device *bdev, fmode_t mode,
                        unsigned int cmd, unsigned long arg)
{
    long size;
    struct hd_geometry geo;
    struct sbull_dev *dev = (struct sbull_dev *)bdev->bd_disk->private_data;

    INFO();
    switch (cmd) {
        case HDIO_GETGEO:
            INFO();
            size = dev->size;
            geo.cylinders = (size & ~0x3f) >> 6;
            geo.heads = 4;
            geo.sectors = 16;
            geo.start = 4;

            if (copy_to_user((void __user *)arg, &geo, sizeof(geo))) {
                return -EFAULT;
            }
            return 0;
    }

    return -ENOTTY;
}

static int sbull_media_changed(struct gendisk *gen)
{
    INFO();
    return 0;
}

static int sbull_revalidate(struct gendisk *gen)
{
    INFO();
    return 0;
}

static int sbull_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
    long size;
    struct sbull_dev *dev = (struct sbull_dev *)bdev->bd_disk->private_data;

    INFO();

    size = dev->size;
    geo->cylinders = (size & ~0x3f) >> 6;
    geo->heads = 4;
    geo->sectors = 16;
    geo->start = 4;
    return 0;
}



static struct block_device_operations sbull_ops = {
    .owner          = THIS_MODULE,
    .open           = sbull_open,
    .release        = sbull_release,
    .media_changed  = sbull_media_changed,
    .revalidate_disk= sbull_revalidate,
    .ioctl          = sbull_ioctl,
    .compat_ioctl   = sbull_ioctl,
    .getgeo         = sbull_getgeo,
};

static int sbull_transfer(struct sbull_dev *dev, unsigned long sector,
                    unsigned long nsect, char *buffer, int write)
{
    unsigned long offset = sector * KERNEL_SECTOR_SIZE;
    unsigned long nbytes = nsect * KERNEL_SECTOR_SIZE;
    INFO("%lu, %lu", offset, nbytes);

    if ((offset + nbytes) > dev->size) {
        INFO("Beyond-end write (%ld %ld)", offset, nbytes);
        return -EIO;
    }
    if (write) {
        memcpy(dev->data + offset, buffer, nbytes);
    } else {
        memcpy(buffer, dev->data + offset, nbytes);
    }
    return 0;
}


static void sbull_request(struct request_queue *q)
{
    struct request *req = NULL;
    struct sbull_dev *dev = NULL;
    unsigned int block, nsect;
    int ret;

    INFO();


    while ((req = blk_fetch_request(q)) != NULL) {

        dev = req->rq_disk->private_data;
        INFO("req->cmd_flags %u", req->cmd_flags);
        block = blk_rq_pos(req);
        nsect = blk_rq_sectors(req);
        ret = sbull_transfer(dev, block, nsect, req->buffer, rq_data_dir(req));
        if (ret < 0) {
            blk_end_request(req, -EIO, 0);
            return ;
        }

        
        spin_unlock(q->queue_lock);
        blk_end_request(req, 0, nsect * KERNEL_SECTOR_SIZE);
        spin_lock(q->queue_lock);
    }
}

static int sbull_xfer_bio(struct sbull_dev *dev, struct bio *bio)
{
    int i = 0;
    struct bio_vec *bvec = NULL;
    sector_t sector = bio->bi_sector;
    INFO();

    bio_for_each_segment(bvec, bio, i) {
        char *buffer = __bio_kmap_atomic(bio, i);
        INFO();
        sbull_transfer(dev, sector, bio_sectors(bio),
                        buffer, bio_data_dir(bio) == WRITE);
        sector += bio_sectors(bio);
        __bio_kunmap_atomic(bio);
    }
    return 0;
}

static void sbull_make_request(struct request_queue *q, struct bio *bio)
{
    struct sbull_dev *dev = (struct sbull_dev *)q->queuedata;
    int status;
    INFO();

    status = sbull_xfer_bio(dev, bio);
    bio_endio(bio, status);
}

static void setup_device(struct sbull_dev *dev, int which)
{
    memset(dev, 0, sizeof(struct sbull_dev));
    dev->size = nsectors * hardsect_size;
    dev->data = vmalloc(dev->size);
    if (dev->data == NULL) {
        INFO("vmalloc failed");
        return ;
    }
    INFO();

    spin_lock_init(&dev->lock);

    switch (request_mode) {
        case RM_NOQUEUE:
            dev->queue = blk_alloc_queue(GFP_KERNEL);
            if (dev->queue == NULL) {
                goto out_vfree;
            }
            blk_queue_make_request(dev->queue, sbull_make_request);
            break;
        case RM_SIMPLE:
            dev->queue = blk_init_queue(sbull_request, &dev->lock);
            if (dev->queue == NULL) {
                INFO("blk_init_queue failed");
                goto out_vfree;
            }
            break;

    }
    dev->queue->queuedata = dev;

    dev->gd = alloc_disk(SBULL_MINORS);
    if (dev->gd == NULL) {
        INFO("alloc_disk failed");
        goto out_vfree;
    }

    dev->gd->major = sbull_major;
    dev->gd->first_minor = which * SBULL_MINORS;
    dev->gd->fops = &sbull_ops;
    dev->gd->queue = dev->queue;
    dev->gd->private_data = dev;
    snprintf(dev->gd->disk_name, 32, "sbull%c", which + 'a');
    set_capacity(dev->gd, nsectors * (hardsect_size / KERNEL_SECTOR_SIZE));
    INFO();
    add_disk(dev->gd);
    INFO();
    return;

  out_vfree:
    if (dev->data) {
        vfree(dev->data);
        dev->data = NULL;
    }
}

static int sbull_init(void)
{
    int i = 0;
    INFO();
    sbull_major = register_blkdev(0, "sbull");
    if (sbull_major <= 0) {
        INFO("register_blkdev failed\n");
    }

    Devices = (struct sbull_dev *)kmalloc(sizeof(struct sbull_dev) * ndevices, GFP_KERNEL);
    if (Devices == NULL) {
        INFO("kmalloc failed");
        goto out_unregister;
    }
    for (i = 0; i < ndevices; i ++) {
        setup_device(Devices + i, i);
    }

    return 0;

  out_unregister:
    unregister_blkdev(sbull_major, "sbull");
    return 0;
}

static void sbull_exit(void)
{
    int i = 0;
    INFO();
    for (i = 0; i < ndevices; i ++) {
        struct sbull_dev *dev = Devices + i;

        if (dev->gd) {
            del_gendisk(dev->gd);
            put_disk(dev->gd);
            dev->gd = NULL;
        }
        if (dev->queue) {
            if (request_mode == RM_NOQUEUE) {
                blk_put_queue(dev->queue);
            } else {
                blk_cleanup_queue(dev->queue);
            }
            dev->queue = NULL;
        }
        if (dev->data) {
            vfree(dev->data);
            dev->data = NULL;
        }
    }
    if (sbull_major > 0) {
        unregister_blkdev(sbull_major, "sbull");
    }
}

module_init(sbull_init);
module_exit(sbull_exit);
