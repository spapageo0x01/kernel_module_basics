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

static struct block_device_operations block_ops = {
    .owner = THIS_MODULE
};

static int make_request(struct request_queue *q, struct bio* bio) {
	// TODO: Create the request queue and receive block commands
}

static int __init block_init(void) {
    printk(KERN_INFO "[Block] Init\n");
    return 0;
}
module_init(block_init);

static void __exit block_destroy(void) {
   printk(KERN_INFO "[Block] Destroy\n");
}
module_exit(block_destroy);
