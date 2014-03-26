/*
 * Copyright (C) 2001-2003 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "dm.h"
#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "foolcache"

 /* TODOs:
merge the bitmaps



 */


struct foolcache_c {
	struct dm_dev *dev;
	unsigned long sectors;
	unsigned int block_size;
	unsigned int block_shift;
	unsigned int block_mask;
	unsigned long* bitmap;
	unsigned long* copying;
	unsigned int bitmap_sectors;
	struct dm_kcopyd_client* kcopyd_client;
	struct job_kcopyd* queue;
	spinlock_t qlock;
};

struct job_kcopyd {
	struct job_kcopyd* next;
	struct bio* bio;
	struct foolcache_c* fcc;
	unsigned int bi_size;
	unsigned long current_block, end_blocks;
	struct dm_io_region origin, cache;
};

static inline unsigned long sector2block(struct foolcache_c* fcc, sector_t sector)
{
	return sector >> fcc->block_shift;
}

static inline sector_t block2sector(struct foolcache_c* fcc, unsigned long block)
{
	return block << fcc->block_shift;
}

static int write_bitmap(struct foolcache_c* fcc)
{
	struct dm_io_region region = {
		.bdev = fcc->cache.bdev,
		.sector = fcc->sectors - (fcc->bitmap_sectors + 1),
		.count = fcc->bitmap_sectors,
	};
	struct dm_io_request io_req = {
		.bi_rw = WRITE,
		.mem.type = DM_IO_VMA,
		.mem.ptr.vma = fcc->bitmap,
		// .notify.fn = ,
		// .notify.context = ,
		.client = fcc->io_client,
	};
	return dm_io(&io_req, 1, &region, NULL);
}

static int read_bitmap(struct foolcache_c* fcc)
{
	char buf[512];
	const static char SIGNATURE[]="FOOLCACHE";
	struct dm_io_region region = {
		.bdev = fcc->cache.bdev,
		.sector = fcc->sectors - 1,
		.count = 1,
	};
	struct dm_io_request io_req = {
		.bi_rw = READ,
		.mem.type = DM_IO_VMA,
		.mem.ptr.vma = buf,
		// .notify.fn = ,
		// .notify.context = ,
		.client = fcc->io_client,
	};
	int r=dm_io(&io_req, 1, &region, NULL);
	if (r!=0) return r;

	r=strncmp(buf, SIGNATURE, sizeof(SIGNATURE)-1);
	if (r!=0) return r;

	io_req.mem.ptr.vma = malloc(fcc->bitmap_sectors*512);
	region.sector = fcc->sectors - (fcc->bitmap_sectors + 1);
	region.count = fcc->bitmap_sectors;
	r = dm_io(&io_req, 1, &region, NULL);
	if (r!=0)
	{
		free(io_req.mem.ptr.vma);
		return r;
	}

	fcc->bitmap=io_req.mem.ptr.vma;
	return 0;
}

static void job_bio_callback_done(unsigned long error, void *context)
{
	bio_endio(bio, error);
}

static void job_bio_callback_further(unsigned long error, void *context)
{
	struct bio* bio = context;
	if (unlikely(error))
	{
		bio_endio(bio, error);
	}

	bio->bi_bdev = fcc->origin->bdev;
	bio->bi_sector += bio->bi_size;
	bio->bi_size = job->bi_size - bio->bi_size;
	do_bio(fcc, bio, job_bio_callback_done, NULL);
}

static void do_bio(struct foolcache_c* fcc, struct bio* bio, io_notify_fn callback, void* ctx)
{
	struct dm_io_region region = {
		.bdev = bio->bdev,
		.sector = bio->bi_sector,
		.count = bio_sectors(bio),
	};
	struct dm_io_request io_req = {
		.bi_rw = READ,
		.mem.type = DM_IO_BVEC,
		.mem.ptr.bvec = bio->bi_io_vec + bio->bi_idx,
		.notify.fn = callback,
		.notify.context = ctx,
		.client = fcc->io_client,
	};
	BUG_ON(dm_io(&io_req, 1, &region, NULL));
}

void split_bio(struct bio* bio)
{
	unsigned int bi_size = bio->bi_size;
	bio->bi_bdev = fcc->cache->bdev;
	bio->bi_size = (fcc->last_caching_sector - bio->bi_sector) * 512;
	do_job_bio(fcc, bio, job_bio_callback_further, (void*)bi_size);
}

static void copy_block_async(struct job_kcopyd* job);
void kcopyd_do_callback(int read_err, unsigned long write_err, void *context)
{
	bool no_intersection;
	struct job_kcopyd* job = context;
	struct foolcache_c* fcc = job->fcc;
	unsigned long block = job->current_block;

	if (unlikely(read_err!=0 && write_err!=0))
	{
		bio_endio(bio, -EIO);
		mempool_free(job, fcc->job_pool);
		return;
	}

	set_bit(fcc, block);
	block = find_next_missing_block(fcc, ++block);
	if (block!=-1)
	{
		job->current_block = block;
		copy_block_async(job);
		return;
	}

	// copy done, read from cache
	no_intersection = (job->end_block <= fcc->last_caching_block);
	mempool_free(job, fcc->job_pool);
	if (likely(no_intersection))
	{	// the I/O region doesn't involve the ender, do it as a whole
		bio->bi_bdev = fcc->cache->bdev;
		do_bio(fcc,bio, do_jobbio_callback_done);
	}
	else do_bio_split(bio);	// the I/O region involves the ender, do it seperately
}

struct job_kcopyd* queued_job_for_block(struct foolcache_c* fcc, unsigned long block)
{
	struct job_kcopyd* next;
	struct job_kcopyd* q = NULL;
	struct job_kcopyd* prev = NULL;
	struct job_kcopyd* node = fcc->queue;
	while (node)
	{
		next = node->next;
		if (block == node->current_block)
		{	// remove it from old queue, and insert it into new queue
			if (prev) prev->next = node->next;
			else fcc->queue = node->next;
			node->next = q;
			q = node;
		}
		prev = node;
		node = next;
	}
	return q;
}

void copy_block_callback(int read_err, unsigned long write_err, void *context)
{
	struct job_kcopyd* qjob;
	struct job_kcopyd* job = context;
	struct foolcache_c* fcc = job->fcc;
	unsigned long block = job->current_block;

	spin_lock_irq(fcc->qlock);
	clear_bit(block, fcc->copying);
	qjob = queued_job_for_block(fcc, block);
	spin_unlock_irq(fcc->qlock);

	while (qjob)
	{
		kcopyd_do_callback(read_err, write_err, qjob);
		qjob = qjob->next;
	}
	kcopyd_do_callback(read_err, write_err, job);
}

static void copy_block_async(struct job_kcopyd* job)
{
	struct foolcache_c* fcc = job->fcc;
	job->origin.sector = job->cache.sector = 
		(job->current_block << fcc->block_shift);

	spin_lock_irq(fcc->qlock);
	if (test_bit(job->current_block, fcc->copying))
	{	// the block is being copied by another thread, let's wait in queue
		job->next = fcc->queue;
		fcc->queue = job;
		spin_unlock_irq(fcc->qlock);
		return;
	}
	spin_unlock_irq(fcc->qlock);


// int dm_kcopyd_copy(struct dm_kcopyd_client *kc, struct dm_io_region *from,
// 		   unsigned num_dests, struct dm_io_region *dests,
// 		   unsigned flags, dm_kcopyd_notify_fn fn, void *context);
	dm_kcopyd_copy(fcc->kcopyd_client, &job->origin, 1, &job->cache, 
		0, kcopyd_callback, job);
}

unsigned long find_next_missing_block(struct foolcache_c* fcc, 
	unsigned long start, unsigned long end)
{
	if (end > fcc->last_caching_block) 
	{
		end = fcc->last_caching_block;
	}
	for (; start<=end; ++start)
	{
		if (!test_bit(start, fcc->bitmap))
		{
			return start;
		}
	}
	return -1;
}

static void fc_map(struct foolcache_c* fcc, struct bio* bio)
{
	unsigned long start_block = sector2block(bio->bi_sector);
	unsigned long end_block = sector2block(bio->bi_sector + bio->bi_size/512);
	unsigned long i = find_next_missing_block(fcc, start, end);

	if (i!=-1)
	{	// found a missing block
		struct job* = mempool_alloc(fcc->job_pool, GFP_NOIO);
		job->fcc = fcc;
		job->bio = bio;
		job->current_block = i;
		job->end_block = end_block;
		job->origin.bdev = fcc->origin;
		job->cache.bdev = fcc->cache;
		job->origin.count = fcc->block_size;
		job->cache.count = fcc->block_size;
		kcopyd(job);
		return DM_MAPIO_SUBMITTED;
	}

	if (end_block > fcc->last_caching_block)
	{
		do_bio_split(bio);
		return DM_MAPIO_SUBMITTED;
	}

	bio->bi_bdev = fcc->cache->bdev;
	return DM_MAPIO_REMAPPED;
}

/*
void fc_map_sync()
{
	for (block in request)
	{
		if (block is missing in cache)
		{
			//copy the block from origin to cache
			if (copying(block))
			{
				wait_in_queue;
			}
			else
			{
				copy(block, origin, cache);
				notify_queue;
			}
		}		
	}

	if (ender & request != [])
	{
		read_from_cache(request - ender);
		read_from_origin(request & ender);
	}
	else
	{
		read_from_cache(request);
	}
}
*/

/*
 * Construct a foolcache mapping
 */
static int foolcache_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct foolcache_c *fcc;
	unsigned long long tmp;

	if (argc != 2) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	fcc = kmalloc(sizeof(*fcc), GFP_KERNEL);
	if (fcc == NULL) {
		ti->error = "dm-foolcache: Cannot allocate foolcache context";
		return -ENOMEM;
	}

	if (sscanf(argv[1], "%llu", &tmp) != 1) {
		ti->error = "dm-foolcache: Invalid device sector";
		goto bad;
	}
	fcc->start = tmp;

	if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &fcc->dev)) {
		ti->error = "dm-foolcache: Device lookup failed";
		goto bad;
	}

	ti->num_flush_requests = 1;
	ti->num_discard_requests = 1;
	ti->private = fcc;
	return 0;

      bad:
	kfree(fcc);
	return -EINVAL;
}

static void foolcache_dtr(struct dm_target *ti)
{
	struct foolcache_c *fcc = ti->private;

	dm_put_device(ti, fcc->dev);
	kfree(fcc);
}

static sector_t linear_map_sector(struct dm_target *ti, sector_t bi_sector)
{
	struct foolcache_c *fcc = ti->private;

	return fcc->start + dm_target_offset(ti, bi_sector);
}

static void linear_map_bio(struct dm_target *ti, struct bio *bio)
{
	struct foolcache_c *fcc = ti->private;

	bio->bi_bdev = fcc->dev->bdev;
	if (bio_sectors(bio))
		bio->bi_sector = linear_map_sector(ti, bio->bi_sector);
}

static int foolcache_map(struct dm_target *ti, struct bio *bio,
		      union map_info *map_context)
{
	linear_map_bio(ti, bio);

	return DM_MAPIO_REMAPPED;
}

static int foolcache_status(struct dm_target *ti, status_type_t type,
			 char *result, unsigned int maxlen)
{
	struct foolcache_c *fcc = ti->private;

	switch (type) {
	case STATUSTYPE_INFO:
		result[0] = '\0';
		break;

	case STATUSTYPE_TABLE:
		snprintf(result, maxlen, "%s %llu", fcc->dev->name,
				(unsigned long long)fcc->start);
		break;
	}
	return 0;
}

static int foolcache_ioctl(struct dm_target *ti, unsigned int cmd,
			unsigned long arg)
{
	struct foolcache_c *fcc = ti->private;
	struct dm_dev *dev = fcc->dev;
	int r = 0;

	if (cmd==FIEMAP)
	{

	}

	/*
	 * Only pass ioctls through if the device sizes match exactly.
	 */
	if (fcc->start ||
	    ti->len != i_size_read(dev->bdev->bd_inode) >> SECTOR_SHIFT)
		r = scsi_verify_blk_ioctl(NULL, cmd);

	return r ? : __blkdev_driver_ioctl(dev->bdev, dev->mode, cmd, arg);
}

static int foolcache_merge(struct dm_target *ti, struct bvec_merge_data *bvm,
			struct bio_vec *biovec, int max_size)
{
	struct foolcache_c *fcc = ti->private;
	struct request_queue *q = bdev_get_queue(fcc->dev->bdev);

	if (!q->merge_bvec_fn)
		return max_size;

	bvm->bi_bdev = fcc->dev->bdev;
	bvm->bi_sector = linear_map_sector(ti, bvm->bi_sector);

	return min(max_size, q->merge_bvec_fn(q, bvm, biovec));
}

static int foolcache_iterate_devices(struct dm_target *ti,
				  iterate_devices_callout_fn fn, void *data)
{
	struct foolcache_c *fcc = ti->private;

	return fn(ti, fcc->dev, fcc->start, ti->len, data);
}

static struct target_type foolcache_target = {
	.name   = "foolcache",
	.version = {1, 0, 0},
	.module = THIS_MODULE,
	.ctr    = foolcache_ctr,
	.dtr    = foolcache_dtr,
	.map    = foolcache_map,
	.status = foolcache_status,
	.ioctl  = foolcache_ioctl,
	.merge  = foolcache_merge,
	.iterate_devices = foolcache_iterate_devices,
};

int __init dm_foolcache_init(void)
{
	int r = dm_register_target(&foolcache_target);

	if (r < 0)
		DMERR("register failed %d", r);

	return r;
}

void dm_foolcache_exit(void)
{
	dm_unregister_target(&linear_target);
}


/* Module hooks */
module_init(dm_foolcache_init);
module_exit(dm_foolcache_exit);

MODULE_DESCRIPTION(DM_NAME " foolcache target");
MODULE_AUTHOR("Huiba Li <lihuiba@gmail.com>");
MODULE_LICENSE("GPL");
