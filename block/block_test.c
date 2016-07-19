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

typedef struct block_device {
	spinlock_t *lock;
	struct request_queue *queue;
} block_device_t;
block_device_t *blk_dev;

static struct block_device_operations block_ops = {
	.owner = THIS_MODULE
};


static int make_request(struct request_queue *q, struct bio* bio)
{
	// TODO: Create the request queue and receive block commands
}


static int __init block_init(void)
{
	int ret;
	printk(KERN_INFO "[%s] Initialization started.\n", DRV_NAME);

	//Allocate memory for device metadata
	blk_dev = kmalloc(sizeof(device_t), GFP_KERNEL);
	if (blk_dev == NULL) {
		printk(KERN_ERR "[%s] Failed to allocate device metadata.\n", DRV_NAME);
		return -ENOMEM;
	}

	//Initialize spinlock & request queue
	spin_lock_init(blk_dev->lock);

	//Initialize request queue (blk_init_queue)
	blk_dev->queue = blk_init_queue(make_request, blk_dev->lock);
	if (blk_dev->queue == NULL) {
		printk(KERN_ERR "[%s] Failed to allocate request queue.\n", DRV_NAME);
		ret = -EINVAL;
		goto error_out;
	}

	// blk_queue_logical_block_size(blk_dev->queue, PAGE_SIZE);
	// blk_queue_physical_block_size(blk_dev->queue, PAGE_SIZE);
	// blk_queue_make_request(blk_dev->queue, make_request);

	//Create a device node


	return ret;

error_out:
	kfree(blk_dev);

	return ret;
}
module_init(block_init);


static void __exit block_destroy(void) {
   printk(KERN_INFO "[Block] Destroy\n");
}
module_exit(block_destroy);
