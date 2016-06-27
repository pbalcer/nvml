/*
 * Copyright 2015-2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * bucket.c -- bucket implementation
 *
 * Buckets manage volatile state of the heap. They are the abstraction layer
 * between the heap-managed chunks/runs and memory allocations.
 *
 * Each bucket instance can have a different underlying container that is
 * responsible for selecting blocks - which means that whether the allocator
 * serves memory blocks in best/first/next -fit manner is decided during bucket
 * creation.
 */

#include <errno.h>
#include <pthread.h>

#include "libpmem.h"
#include "libpmemobj.h"
#include "util.h"
#include "out.h"
#include "sys_util.h"
#include "redo.h"
#include "heap_layout.h"
#include "memops.h"
#include "memblock.h"
#include "bucket.h"
#include "ctree.h"
#include "lane.h"
#include "pmalloc.h"
#include "heap.h"
#include "list.h"
#include "obj.h"
#include "valgrind_internal.h"
#include "container_ctree.h"

static struct {
	const struct block_container_ops *ops;
	struct block_container *(*create)(void);
	void (*delete)(struct block_container *c);
} block_containers[MAX_CONTAINER_TYPE] = {
	{NULL, NULL, NULL},
	{&container_ctree_ops, bucket_tree_create, bucket_tree_delete},
};

#ifdef USE_VG_MEMCHECK
/*
 * bucket_vg_mark_noaccess -- (internal) marks memory block as no access for vg
 */
static void
bucket_vg_mark_noaccess(PMEMobjpool *pop, struct bucket *b,
	struct memory_block m)
{
	if (On_valgrind) {
		size_t rsize = m.size_idx * b->unit_size;
		void *block_data = heap_get_block_data(pop, m);
		VALGRIND_DO_MAKE_MEM_NOACCESS(pop, block_data, rsize);
	}
}
#endif

static int
bucket_insert(PMEMobjpool *pop, struct bucket *b, struct memory_block m)
{
#ifdef USE_VG_MEMCHECK
	bucket_vg_mark_noaccess(pop, b, m);
#endif

	return block_containers[b->container->type].ops->
		insert(b->container, m);
}

static int
bucket_get_rm_exact(struct bucket *b, struct memory_block m)
{
	return block_containers[b->container->type].ops->
		get_rm_exact(b->container, m);
}

static int
bucket_get_rm_fit(PMEMobjpool *pop, struct bucket *b, struct memory_block *m)
{
	return block_containers[b->container->type].ops->
		get_rm_bestfit(b->container, m);
}

static int
run_insert(PMEMobjpool *pop, struct bucket *b, struct memory_block m)
{
#ifdef USE_VG_MEMCHECK
	bucket_vg_mark_noaccess(pop, b, m);
#endif
	struct bucket_run *r = (struct bucket_run *)b;
	if (r->nextfit_pos != -1)
		return 0;

	return block_containers[b->container->type].ops->
		insert(b->container, m);
}

static int
run_get_rm_fit(PMEMobjpool *pop, struct bucket *b, struct memory_block *m)
{
	struct bucket_run *r = (struct bucket_run *)b;
	if (r->nextfit_pos != -1) {
		if ((uint32_t)r->nextfit_pos + m->size_idx >= r->active.size_idx) {
			return ENOMEM;
		}
		m->block_off = (uint16_t)r->nextfit_pos;
		m->chunk_id = r->active.chunk_id;
		m->zone_id = r->active.zone_id;

		r->nextfit_pos += (int)m->size_idx;

		return 0;
	}

	return block_containers[b->container->type].ops->
		get_rm_bestfit(b->container, m);
}

static int
run_remove_active(struct bucket_run *run)
{
	run->nextfit_pos = -1;
	run->active = EMPTY_MEMORY_BLOCK;

	block_containers[run->super.container->type].ops->
		clear(run->super.container);

	return 0;
}

static int
run_set_active(struct bucket_run *run, struct memory_block m)
{
	ASSERTeq(m.block_off, 0);
	ASSERTeq(m.size_idx, run->bitmap_nallocs);

	run->nextfit_pos = 0;
	run->active = m;

	return 0;
}

/*
 * bucket_run_create -- (internal) creates a run bucket
 *
 * This type of bucket is responsible for holding memory blocks from runs, which
 * means that each object it contains has a representation in a bitmap.
 *
 * The run buckets also contain the detailed information about the bitmap
 * all of the memory blocks contained within this container must be
 * represented by. This is not to say that a single bucket contains objects
 * only from a single chunk/bitmap - a bucket contains objects from a single
 * TYPE of bitmap run.
 */
static struct bucket *
bucket_run_create(size_t unit_size, unsigned unit_max)
{
	struct bucket_run *b = Malloc(sizeof(*b));
	if (b == NULL)
		return NULL;

	b->nextfit_pos = -1;
	b->active = EMPTY_MEMORY_BLOCK;

	b->super.insert = run_insert;
	b->super.get_rm_fit = run_get_rm_fit;
	b->super.get_rm_exact = bucket_get_rm_exact;
	b->set_active = run_set_active;
	b->remove_active = run_remove_active;

	b->super.type = BUCKET_RUN;
	b->unit_max = unit_max;

	/*
	 * Here the bitmap definition is calculated based on the size of the
	 * available memory and the size of a memory block - the result of
	 * dividing those two numbers is the number of possible allocations from
	 * that block, and in other words, the amount of bits in the bitmap.
	 */
	ASSERT(RUN_NALLOCS(unit_size) <= UINT32_MAX);
	b->bitmap_nallocs = (unsigned)(RUN_NALLOCS(unit_size));

	/*
	 * The two other numbers that define our bitmap is the size of the
	 * array that represents the bitmap and the last value of that array
	 * with the bits that exceed number of blocks marked as set (1).
	 */
	ASSERT(b->bitmap_nallocs <= RUN_BITMAP_SIZE);
	unsigned unused_bits = RUN_BITMAP_SIZE - b->bitmap_nallocs;

	unsigned unused_values = unused_bits / BITS_PER_VALUE;

	ASSERT(MAX_BITMAP_VALUES >= unused_values);
	b->bitmap_nval = MAX_BITMAP_VALUES - unused_values;

	ASSERT(unused_bits >= unused_values * BITS_PER_VALUE);
	unused_bits -= unused_values * BITS_PER_VALUE;

	b->bitmap_lastval = unused_bits ?
		(((1ULL << unused_bits) - 1ULL) <<
			(BITS_PER_VALUE - unused_bits)) : 0;

	return &b->super;
}

/*
 * bucket_huge_create -- (internal) creates a huge bucket
 *
 * Huge bucket contains chunks with either free or used types. The only reason
 * there's a separate huge data structure is the bitmap information that is
 * required for runs and is not relevant for huge chunks.
 */
static struct bucket *
bucket_huge_create(size_t unit_size, unsigned unit_max)
{
	struct bucket_huge *b = Malloc(sizeof(*b));
	if (b == NULL)
		return NULL;

	b->super.insert = bucket_insert;
	b->super.get_rm_fit = bucket_get_rm_fit;
	b->super.get_rm_exact = bucket_get_rm_exact;

	b->super.type = BUCKET_HUGE;

	return &b->super;
}

/*
 * bucket_common_delete -- (internal) deletes a bucket
 */
static void
bucket_common_delete(struct bucket *b)
{
	Free(b);
}

static struct {
	struct bucket *(*create)(size_t unit_size, unsigned unit_max);
	void (*delete)(struct bucket *b);
} bucket_types[MAX_BUCKET_TYPE] = {
	{NULL, NULL},
	{bucket_huge_create, bucket_common_delete},
	{bucket_run_create, bucket_common_delete}
};

/*
 * bucket_calc_units -- (internal) calculates the size index of a memory block
 *	whose size in bytes is equal or exceeds the value 'size' provided
 *	by the caller.
 */
static uint32_t
bucket_calc_units(struct bucket *b, size_t size)
{
	ASSERTne(size, 0);
	return CALC_SIZE_IDX(b->unit_size, size);
}

/*
 * bucket_new -- allocates and initializes bucket instance
 */
struct bucket *
bucket_new(uint8_t id, enum bucket_type type, enum block_container_type ctype,
	size_t unit_size, unsigned unit_max)
{
	ASSERT(unit_size > 0);

	struct bucket *b = bucket_types[type].create(unit_size, unit_max);
	if (b == NULL)
		goto error_bucket_malloc;

	b->id = id;
	b->calc_units = bucket_calc_units;

	b->container = block_containers[ctype].create();
	if (b->container == NULL)
		goto error_container_create;

	util_mutex_init(&b->lock, NULL);

	b->unit_size = unit_size;

	return b;

error_container_create:
	bucket_types[type].delete(b);
error_bucket_malloc:
	return NULL;
}

/*
 * bucket_delete -- cleanups and deallocates bucket instance
 */
void
bucket_delete(struct bucket *b)
{
	util_mutex_destroy(&b->lock);

	block_containers[b->container->type].delete(b->container);
	bucket_types[b->type].delete(b);
}
