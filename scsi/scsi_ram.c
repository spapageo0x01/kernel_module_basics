/*
 * scsi_ram.c - A RAM-based SCSI driver for Linux.
 *
 * This driver is intended to run as fast as possible, hence the options
 * to discard writes and reads.
 * By default, it'll allocate half a gigabyte of RAM to use as a ramdisc;
 * you can change this with the `capacity' module parameter.
 *
 * (C) Copyright 2012 Intel Corporation
 * Author: Matthew Wilcox <willy <at> linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#undef DEBUG
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_dbg.h>

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Matthew Wilcox <willy <at> linux.intel.com>");

#define DRV_NAME "scsi_ram"

static unsigned int sector_size = 512;
module_param(sector_size, uint, 0444);
MODULE_PARM_DESC(sector_size, "Size of sectors");

static unsigned int capacity = 1024 * 1024;
module_param(capacity, uint, 0444);
MODULE_PARM_DESC(capacity, "Number of logical blocks in device");

static int throw_away_writes;
module_param(throw_away_writes, int, 0644);
MODULE_PARM_DESC(throw_away_writes, "Discard all writes to the device");

static int throw_away_reads;
module_param(throw_away_reads, int, 0644);
MODULE_PARM_DESC(throw_away_reads, "Don't actually read data from the device");

static int use_thread = 0;
module_param(use_thread, int, 0644);
MODULE_PARM_DESC(use_thread, "Use a separate thread to do data accesses");


static void copy_buffer(struct scsi_cmnd *cmnd, char *buf, int len)
{
	char *p;
	struct scatterlist *sg;
	int i;

	scsi_for_each_sg(cmnd, sg, scsi_sg_count(cmnd), i) {
		int tocopy = sg->length;
		if (tocopy > len)
			tocopy = len;

		p = kmap_atomic(sg_page(sg));
		memcpy(p + sg->offset, buf, tocopy);
		kunmap_atomic(p);

		len -= tocopy;
		if (!len)
			break;
		buf += tocopy;
	}

	scsi_set_resid(cmnd, len);
}

static char inquiry[57] = {
	0, 0, 5, 0x22, 52, 0, 0, 0x0a,			/* 0-7 */
	'L', 'i', 'n', 'u', 'x', ' ', ' ', ' ',		/* 8-15 */
	'R', 'A', 'M', ' ', 'D', 'r', 'i', 'v',		/* 16-23 */
	'e', ' ', ' ', ' ', ' ', ' ', ' ', ' ',		/* 24-31 */
	'0', '.', '0', '1', 0, 0, 0, 0,			/* 32-39 */
	0, 0, 0, 0, 0, 0, 0, 0,				/* 40-47 */
	0, 0, 0, 0, 0, 0, 0, 0,				/* 48-55 */
	0						/* 56 */
};

static char report_luns[] = {
	0, 0, 0, 8, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0
};

/*
 * SCSI requires quantities to be written MSB.  They're frequently misaligned,
 * so don't mess about with cpu_to_beN, just access it byte-wise
 */
static void scsi_ram_put_u32(unsigned char *addr, unsigned int data)
{
	addr[0] = data >> 24;
	addr[1] = data >> 16;
	addr[2] = data >> 8;
	addr[3] = data;
}

static unsigned int scsi_ram_get_u16(unsigned char *addr)
{
	unsigned int data;
	data = addr[0] << 8;
	data |= addr[1];

	return data;
}

static unsigned int scsi_ram_get_u24(unsigned char *addr)
{
	unsigned int data;
	data = addr[0] << 16;
	data |= addr[1] << 8;
	data |= addr[2];

	return data;
}

static unsigned int scsi_ram_get_u32(unsigned char *addr)
{
	unsigned int data;
	data = addr[0] << 24;
	data |= addr[1] << 16;
	data |= addr[2] << 8;
	data |= addr[3];

	return data;
}


static void scsi_ram_setup_sense(struct scsi_cmnd *cmnd, unsigned char key,
				unsigned char asc, unsigned char ascq)
{
	if (0) {
		cmnd->sense_buffer[0] = 0x72;
		cmnd->sense_buffer[1] = key;
		cmnd->sense_buffer[2] = asc;
		cmnd->sense_buffer[3] = ascq;
		cmnd->sense_buffer[7] = 0;
	} else {
		cmnd->sense_buffer[0] = 0x70;
		cmnd->sense_buffer[1] = 0;
		cmnd->sense_buffer[2] = key;
		cmnd->sense_buffer[7] = 11;
		cmnd->sense_buffer[12] = asc;
		cmnd->sense_buffer[13] = ascq;
	}
}


static void scsi_ram_inquiry(struct scsi_cmnd *cmnd)
{
	if (cmnd->cmnd[1] & 1) {
		switch (cmnd->cmnd[2]) {
		default:
			scsi_ram_setup_sense(cmnd, ILLEGAL_REQUEST, 0x24, 0);
			cmnd->result = SAM_STAT_CHECK_CONDITION;
		}
	} else {
		copy_buffer(cmnd, inquiry, sizeof(inquiry));
	}
}

static void scsi_ram_read_capacity(struct scsi_cmnd *cmnd)
{
	char buf[8];
	scsi_ram_put_u32(buf, capacity - 1);
	scsi_ram_put_u32(buf + 4, sector_size);
	copy_buffer(cmnd, buf, sizeof(buf));
}

static void scsi_ram_mode_sense(struct scsi_cmnd *cmnd)
{
	char buf[7];
	int mode_buf_length = 4;
	int page_code = cmnd->cmnd[2] & 0x3f;

	memset(buf, 0, sizeof(buf)); /* no FUA, WP or WCE */
	buf[0] = 3; /* Mode data length */

	/* Caching or all mode pages */
	if (page_code == 0x8 || page_code == 0x3f) {
		buf[0] = 6;
		buf[4] = 8;
		buf[5] = 1;
		mode_buf_length = 7;
		if (mode_buf_length > cmnd->cmnd[4])
			mode_buf_length = cmnd->cmnd[4];
	}
	scsi_sg_copy_from_buffer(cmnd, buf, mode_buf_length);
	cmnd->result = 0;
}


static struct page **scsi_ram_data_array;

/*
 * We could steal the pages we need from the requests as they come in, which
 * is what rd.c does.  However, that's not a realistic simulator of how a
 * device would work.  We want the request pages to get freed and go back into
 * the page allocator.
 */
static int scsi_ram_alloc_data(void)
{
	unsigned long pages = capacity / PAGE_SIZE * sector_size;
	unsigned int i;

	scsi_ram_data_array = vmalloc(pages * sizeof(void *));
	if (!scsi_ram_data_array)
		return -ENOMEM;

	for (i = 0; i < pages; i++) {
		struct page *page = alloc_page(GFP_KERNEL | __GFP_HIGHMEM);
		scsi_ram_data_array[i] = page;

		/* scsi_ram_free_data will be called on failure */
		if (!page)
			return -ENOMEM;
		clear_highpage(page);
	}

	return 0;
}

static void scsi_ram_free_data(void)
{
	unsigned long pages = capacity / PAGE_SIZE * sector_size;
	unsigned int i;

	if (!scsi_ram_data_array)
		return;

	for (i = 0; i < pages; i++) {
		struct page *page = scsi_ram_data_array[i];
		if (!page)
			break;
		__free_page(page);
	}

	vfree(scsi_ram_data_array);
}

static void scsi_ram_too_big(struct scsi_cmnd *cmnd, unsigned int start,
				unsigned int len)
{
	pr_warn("Request exceeded device capacity! %u %u\n", start, len);
	scsi_ram_setup_sense(cmnd, ILLEGAL_REQUEST, 0x21, 0);
	cmnd->result = SAM_STAT_CHECK_CONDITION;
}

static void *get_data_page(unsigned int pfn)
{
	return kmap_atomic(scsi_ram_data_array[pfn]);
}

static void put_data_page(void *addr)
{
	kunmap_atomic(addr);
}

static void *get_sg_page(struct page *page)
{
	return kmap_atomic(page);
}

static void put_sg_page(void *addr)
{
	kunmap_atomic(addr);
}

static void scsi_ram_read(struct scsi_cmnd *cmnd, unsigned int startB,
							unsigned int lenB)
{
	unsigned long start = startB * sector_size;
	unsigned long len = lenB * sector_size;
	struct scatterlist *sg;
	unsigned i, from_off = start % PAGE_SIZE, data_pfn = start / PAGE_SIZE;

	if (startB > capacity || (startB + lenB) > capacity)
		return scsi_ram_too_big(cmnd, startB, lenB);

	if (throw_away_reads)
		return;

	scsi_for_each_sg(cmnd, sg, scsi_sg_count(cmnd), i) {
		struct page *sgpage = sg_page(sg);
		unsigned int to_off = sg->offset;
		unsigned int sg_copy = sg->length;
		if (sg_copy > len)
			sg_copy = len;
		len -= sg_copy;

		while (sg_copy > 0) {
			char *vto, *vfrom;
			unsigned int page_copy;

			if (from_off > to_off)
				page_copy = PAGE_SIZE - from_off;
			else
				page_copy = PAGE_SIZE - to_off;
			if (page_copy > sg_copy)
				page_copy = sg_copy;

			vfrom = get_data_page(data_pfn);
			vto = get_sg_page(sgpage);
			memcpy(vto + to_off, vfrom + from_off, page_copy);
			put_sg_page(vto);
			put_data_page(vfrom);
			from_off += page_copy;
			if (from_off == PAGE_SIZE) {
				from_off = 0;
				data_pfn++;
			}
			to_off += page_copy;
			if (to_off == PAGE_SIZE) {
				to_off = 0;
				sgpage++;
			}
			sg_copy -= page_copy;
		}
		if (!len)
			break;
	}
}

static void scsi_ram_write(struct scsi_cmnd *cmnd, unsigned int startB,
							unsigned int lenB)
{
	unsigned long start = startB * sector_size;
	unsigned long len = lenB * sector_size;
	struct scatterlist *sg;
	unsigned i, to_off = start % PAGE_SIZE, data_pfn = start / PAGE_SIZE;

	if (startB > capacity || (startB + lenB) > capacity)
		return scsi_ram_too_big(cmnd, startB, lenB);

	if (throw_away_writes)
		return;

	scsi_for_each_sg(cmnd, sg, scsi_sg_count(cmnd), i) {
		struct page *sgpage = sg_page(sg);
		unsigned int from_off = sg->offset;
		unsigned int sg_copy = sg->length;
		if (sg_copy > len)
			sg_copy = len;
		len -= sg_copy;

		while (sg_copy > 0) {
			char *vto, *vfrom;
			unsigned int page_copy;

			if (from_off > to_off)
				page_copy = PAGE_SIZE - from_off;
			else
				page_copy = PAGE_SIZE - to_off;
			if (page_copy > sg_copy)
				page_copy = sg_copy;

			vfrom = get_sg_page(sgpage);
			vto = get_data_page(data_pfn);
			memcpy(vto + to_off, vfrom + from_off, page_copy);
			put_data_page(vto);
			put_sg_page(vfrom);

			from_off += page_copy;
			if (from_off == PAGE_SIZE) {
				from_off = 0;
				sgpage++;
			}
			to_off += page_copy;
			if (to_off == PAGE_SIZE) {
				to_off = 0;
				data_pfn++;
			}
			sg_copy -= page_copy;
		}
		if (!len)
			break;
	}
}

static void scsi_ram_read_6(struct scsi_cmnd *cmnd)
{
	unsigned int first_block = scsi_ram_get_u24(cmnd->cmnd + 1) & 0x1fffff;
	unsigned int length = cmnd->cmnd[4];
	if (!length)
		length = 256;
	scsi_ram_read(cmnd, first_block, length);
}

static void scsi_ram_read_10(struct scsi_cmnd *cmnd)
{
	unsigned int first_block = scsi_ram_get_u32(cmnd->cmnd + 2);
	unsigned int length = scsi_ram_get_u16(cmnd->cmnd + 7);
	scsi_ram_read(cmnd, first_block, length);
}

static void scsi_ram_write_6(struct scsi_cmnd *cmnd)
{
	unsigned int first_block = scsi_ram_get_u24(cmnd->cmnd + 1) & 0x1fffff;
	unsigned int length = cmnd->cmnd[4];
	if (!length)
		length = 256;
	scsi_ram_write(cmnd, first_block, length);
}

static void scsi_ram_write_10(struct scsi_cmnd *cmnd)
{
	unsigned int first_block = scsi_ram_get_u32(cmnd->cmnd + 2);
	unsigned int length = scsi_ram_get_u16(cmnd->cmnd + 7);
	scsi_ram_write(cmnd, first_block, length);
}

static void scsi_ram_execute_command(struct scsi_cmnd *cmnd)
{
#ifdef DEBUG
	scsi_print_command(cmnd);
#endif

	switch (cmnd->cmnd[0]) {
	case INQUIRY:
		scsi_ram_inquiry(cmnd);
		break;
	case REPORT_LUNS:
		copy_buffer(cmnd, report_luns, sizeof(report_luns));
		break;
	case TEST_UNIT_READY:
		cmnd->result = 0;
		break;
	case READ_CAPACITY:
		scsi_ram_read_capacity(cmnd);
		break;
	case MODE_SENSE:
		scsi_ram_mode_sense(cmnd);
		break;
	case READ_6:
		scsi_ram_read_6(cmnd);
		break;
	case READ_10:
		scsi_ram_read_10(cmnd);
		break;
	case WRITE_6:
		scsi_ram_write_6(cmnd);
		break;
	case WRITE_10:
		scsi_ram_write_10(cmnd);
		break;
	default:
		cmnd->result = DID_ABORT << 16;
	}

	cmnd->scsi_done(cmnd);
}

struct scsi_ram_device {
	struct list_head commands;
	struct Scsi_Host *host;
	struct task_struct *thread;
};
static struct scsi_ram_device *scsi_ram_devices[16];

/* Overrides scsi_pointer */
struct scsi_ram_cmnd {
	struct list_head queue;
};

static int scsi_ram_device_thread(void *data)
{
	struct scsi_ram_device *ram_device = data;
	struct Scsi_Host *host = ram_device->host;
	unsigned long flags;

	while (!kthread_should_stop()) {
		struct scsi_cmnd *cmnd;
		struct scsi_ram_cmnd *ram_cmnd;

		spin_lock_irqsave(host->host_lock, flags);
		if (list_empty(&ram_device->commands)) {
			set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock_irqrestore(host->host_lock, flags);
			schedule();
			continue;
		}

		ram_cmnd = list_first_entry(&ram_device->commands,
						struct scsi_ram_cmnd, queue);
		list_del(&ram_cmnd->queue);
		spin_unlock_irqrestore(host->host_lock, flags);

		cmnd = container_of((struct scsi_pointer *)ram_cmnd,
							struct scsi_cmnd, SCp);

		if (cmnd->cmnd[0] == READ_10 || cmnd->cmnd[0] == WRITE_10) {
			cmnd->scsi_done(cmnd);
		} else {
			scsi_ram_execute_command(cmnd);
		}
	}
	__set_current_state(TASK_RUNNING);

	return 0;
}

static int scsi_ram_queuecommand(struct Scsi_Host *shost,
		struct scsi_cmnd *cmnd)
{
	struct scsi_ram_cmnd *ram_cmnd = (void *)&cmnd->SCp;
	struct scsi_ram_device *ram_device = scsi_ram_devices[cmnd->device->id];
	unsigned long flags;

	pr_debug("Queueing command\n");
	if (!ram_device)
		goto bad_device;

	if (use_thread) {
		spin_lock_irqsave(shost->host_lock, flags);
		if (list_empty(&ram_device->commands))
			wake_up_process(ram_device->thread);
		list_add_tail(&ram_cmnd->queue, &ram_device->commands);
		spin_unlock_irqrestore(shost->host_lock, flags);
	} else {
		if (cmnd->cmnd[0] == READ_10 || cmnd->cmnd[0] == WRITE_10) {
			cmnd->scsi_done(cmnd);
		} else {
			scsi_ram_execute_command(cmnd);
		}
	}

	return 0;

 bad_device:
	cmnd->result = DID_BAD_TARGET << 16;
	cmnd->scsi_done(cmnd);
	return 0;
}

static int scsi_ram_slave_alloc(struct scsi_device *sdev)
{
	struct scsi_ram_device *ram_device;

	pr_debug("slave_alloc %d:%d\n", sdev->id, sdev->lun);

	/* For the moment, create only device 0, lun 0 */
	if (sdev->id != 0)
		return 0;
	if (sdev->lun != 0)
		return 0;

	ram_device = kmalloc(sizeof(*ram_device), GFP_KERNEL);
	if (!ram_device)
		goto nomem;
	INIT_LIST_HEAD(&ram_device->commands);
	ram_device->host = sdev->host;
	if (scsi_ram_alloc_data())
		goto nomem;
	ram_device->thread = kthread_run(scsi_ram_device_thread, ram_device,
					 "scsi_ram_%d", sdev->id);
	if (IS_ERR(ram_device->thread))
		goto nomem;

	scsi_ram_devices[sdev->id] = ram_device;
	return 0;

 nomem:
	scsi_ram_free_data();
	kfree(ram_device);
	return -ENOMEM;
}

static void scsi_ram_slave_destroy(struct scsi_device *sdev)
{
	struct scsi_ram_device *ram_device = scsi_ram_devices[sdev->id];

	pr_debug("slave_destroy %d:%d\n", sdev->id, sdev->lun);
	if (!ram_device)
		return;
	if (sdev->lun != 0)
		return;

	kthread_stop(ram_device->thread);
	scsi_ram_free_data();
	kfree(ram_device);
	scsi_ram_devices[sdev->id] = NULL;
}

static int scsi_ram_eh_host_reset_handler(struct scsi_cmnd *scmd)
{
	pr_debug("eh_host_reset_handler\n");
	return 0;
}

static struct scsi_host_template scsi_ram_template = {
	.proc_name = DRV_NAME,
	.name = DRV_NAME,
	.queuecommand = scsi_ram_queuecommand,
	.eh_host_reset_handler = scsi_ram_eh_host_reset_handler,
	.slave_alloc = scsi_ram_slave_alloc,
	.slave_destroy = scsi_ram_slave_destroy,
	.can_queue = 64,
	.this_id = 7,
	.sg_tablesize = SG_ALL,
	.max_sectors = 1024,
	.cmd_per_lun = 64,
	.skip_settle_delay = 1,
	.use_clustering = DISABLE_CLUSTERING,
};

static struct Scsi_Host *scsi_ram_host;

static int __init scsi_ram_init(void)
{
	int error;

	scsi_ram_host = scsi_host_alloc(&scsi_ram_template, 0);
	if (!scsi_ram_host)
		return -ENOMEM;

	error = scsi_add_host(scsi_ram_host, NULL);
	if (error)
		goto free_host;

	scsi_scan_host(scsi_ram_host);
	return 0;

 free_host:
	scsi_host_put(scsi_ram_host);
	return error;
}

static void __exit scsi_ram_exit(void)
{
	scsi_remove_host(scsi_ram_host);
	scsi_host_put(scsi_ram_host);
}

module_init(scsi_ram_init);
module_exit(scsi_ram_exit);

