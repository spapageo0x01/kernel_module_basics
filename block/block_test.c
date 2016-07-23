#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/bio.h>

MODULE_DESCRIPTION("Basic block device kernel module");
MODULE_AUTHOR("Spyros Papageorgiou");
MODULE_VERSION("0.1");
MODULE_LICENSE("GPL v2");

#define DRV_NAME "blk test"
#define ONE_GIG 2097152

typedef struct block_dev {
	spinlock_t lock;
	struct gendisk* gd;
	struct request_queue *queue;
} block_device_t;
block_device_t *blk_dev;

static struct block_device_operations block_ops = {
	.owner = THIS_MODULE
};


static void make_request(struct request_queue *queue, struct bio *bio)
{	
	//printk(KERN_ERR "[%s] make_request called\n", DRV_NAME);
	bio_endio(bio, 0);
}


static int __init block_init(void)
{
	int ret = 0;
	int major_num = 0;
	printk(KERN_INFO "[%s] Initialization started.\n", DRV_NAME);

	//Allocate memory for device metadata
	blk_dev = vmalloc(sizeof(block_device_t));
	if (blk_dev == NULL) {
		printk(KERN_ERR "[%s] Failed to allocate device metadata.\n", DRV_NAME);
		return -ENOMEM;
	}

	//Initialize spinlock
	spin_lock_init(&blk_dev->lock);

	//Initialize request queue (blk_init_queue)
	blk_dev->queue = blk_alloc_queue(GFP_KERNEL);
	if (blk_dev->queue == NULL) {
		printk(KERN_ERR "[%s] Failed to allocate request queue.\n", DRV_NAME);
		ret = -EINVAL;
		goto error_out;
	}

	blk_queue_make_request(blk_dev->queue, make_request);

	blk_queue_logical_block_size(blk_dev->queue, PAGE_SIZE);
	blk_queue_physical_block_size(blk_dev->queue, PAGE_SIZE);
	

	major_num = register_blkdev(major_num, "spap");
	if (major_num <= 0) {
		printk(KERN_ERR "[%s] Failed to get major number", DRV_NAME);
		ret = -EINVAL;
		goto error_out;
	}

	blk_dev->gd = alloc_disk(1);
	if (!blk_dev->gd) {
		printk(KERN_ERR "[%s] Unable to allocate gendisk structure", DRV_NAME);
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
	printk(KERN_ERR "[%s] Started device!\n", DRV_NAME);

	return ret;

error_out:
	vfree(blk_dev);
	// blk_cleanup_queue(blk_dev->queue);

	return ret;
}
module_init(block_init);


static void __exit block_destroy(void) {
   printk(KERN_INFO "[%s] Destroy\n", DRV_NAME);
   del_gendisk(blk_dev->gd);
   put_disk(blk_dev->gd);
   vfree(blk_dev);
}
module_exit(block_destroy);
