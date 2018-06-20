#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/delay.h>


MODULE_DESCRIPTION("Basic block device kernel module");
MODULE_AUTHOR("Spyros Papageorgiou");
MODULE_VERSION("0.1");
MODULE_LICENSE("GPL v2");

#define DRV_NAME "blk test"
#define ONE_GIG 2097152

struct block_dev {
	spinlock_t lock;
	struct gendisk *gd;
	struct request_queue *queue;
};

static const struct block_device_operations block_ops = {
	.owner = THIS_MODULE
};


static void blk_drv_err(const char *msg)
{
	printk(KERN_ERR "[%s] %s\n", DRV_NAME, msg);
}

static void blk_drv_info(const char *msg)
{
	printk(KERN_INFO "[%s] %s\n", DRV_NAME, msg);
}


static void make_request(struct request_queue *queue, struct bio *bio)
{
	printk(KERN_ERR "%s called", __func__);
	bio_endio(bio, 0);
}

static int __init block_init(void)
{
	int ret = 0;
	int major_num = 0;

	blk_drv_info("Initialization started");

	//Allocate memory for device metadata
	blk_dev = vmalloc(sizeof(struct block_device));
	if (!blk_dev) {
		do {
			msleep(500);
			blk_dev = vmalloc(sizeof(struct block_device));
		} while (!blk_dev);
	}

	//Initialize spinlock
	spin_lock_init(&blk_dev->lock);

	//Initialize request queue (blk_init_queue)
	blk_dev->queue = blk_alloc_queue(GFP_KERNEL);
	if (blk_dev->queue == NULL) {
		blk_drv_err("Failed to allocate request queue");
		ret = -EINVAL;
		goto error_out;
	}

	blk_queue_make_request(blk_dev->queue, make_request);

	blk_queue_logical_block_size(blk_dev->queue, PAGE_SIZE);
	blk_queue_physical_block_size(blk_dev->queue, PAGE_SIZE);

	major_num = register_blkdev(major_num, "spap");
	if (major_num <= 0) {
		blk_drv_err("Failed to get major number");
		ret = -EINVAL;
		goto error_out;
	}

	blk_dev->gd = alloc_disk(1);
	if (!blk_dev->gd) {
		blk_drv_err("Unable to allocate gendisk structure");
		ret = -ENOMEM;
		goto error_out;
	}

	blk_dev->gd->major = major_num;
	blk_dev->gd->fops = &block_ops;
	snprintf(blk_dev->gd->disk_name, 32, "spap");
	blk_dev->gd->first_minor = 0;
	blk_dev->gd->queue = blk_dev->queue;
	set_capacity(blk_dev->gd, ONE_GIG);
	add_disk(blk_dev->gd);

	blk_drv_info("Started device !");

	return ret;

error_out:
	vfree(blk_dev);
	// blk_cleanup_queue(blk_dev->queue);

	return ret;
}
module_init(block_init);


static void __exit block_destroy(void)
{
	blk_drv_info("Destroying device");
	del_gendisk(blk_dev->gd);
	put_disk(blk_dev->gd);
	vfree(blk_dev);
}
module_exit(block_destroy);
