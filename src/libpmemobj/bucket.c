/*
 * Copyright (c) 2015, Intel Corporation
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
 *     * Neither the name of Intel Corporation nor the names of its
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
 */

#include <errno.h>
#include <sys/queue.h>

#include "libpmem.h"
#include "libpmemobj.h"
#include "util.h"
#include "out.h"
#include "redo.h"
#include "heap_layout.h"
#include "heap.h"
#include "cuckoo.h"
#include "bucket.h"
#include "ctree.h"
#include "lane.h"
#include "list.h"
#include "obj.h"
#include "valgrind_internal.h"

/*
 * The elements in the tree are sorted by the key and it's vital that the
 * order is by size, hence the order of the pack arguments.
 */
#define	CHUNK_KEY_PACK(z, c, b, s)\
((uint64_t)(s) << 48 | (uint64_t)(b) << 32 | (uint64_t)(c) << 16 | (z))

#define	CHUNK_KEY_GET_ZONE_ID(k)\
((uint16_t)((k & 0xFFFF)))

#define	CHUNK_KEY_GET_CHUNK_ID(k)\
((uint16_t)((k & 0xFFFF0000) >> 16))

#define	CHUNK_KEY_GET_BLOCK_OFF(k)\
((uint16_t)((k & 0xFFFF00000000) >> 32))

#define	CHUNK_KEY_GET_SIZE_IDX(k)\
((uint16_t)((k & 0xFFFF000000000000) >> 48))

struct list_block {
	LIST_ENTRY(list_block) list;
	struct memory_block block;
};

struct block_container_list {
	struct block_container super;
	LIST_HEAD(, list_block) blocks;
	pthread_mutex_t list_lock;
	struct cuckoo *map;
};

struct block_container_ctree {
	struct block_container super;
	struct ctree *tree;
};

#ifdef USE_VG_MEMCHECK
static void
bucket_vg_mark_noaccess(PMEMobjpool *pop, struct block_container *bc,
	struct memory_block m)
{
	if (On_valgrind) {
		size_t rsize = m.size_idx * bc->unit_size;
		void *block_data = heap_get_block_data(pop, m);
		VALGRIND_DO_MAKE_MEM_NOACCESS(pop, block_data, rsize);
	}
}
#endif

/*
 * bucket_insert_block -- inserts a new memory block into the container
 */
static int
bucket_tree_insert_block(struct block_container *bc, PMEMobjpool *pop,
	struct memory_block m)
{
	ASSERT(m.chunk_id < MAX_CHUNK);
	ASSERT(m.zone_id < UINT16_MAX);
	ASSERT(m.size_idx != 0);

	struct block_container_ctree *c = (struct block_container_ctree *)bc;

#ifdef USE_VG_MEMCHECK
	bucket_vg_mark_noaccess(pop, bc, m);
#endif

	uint64_t key = CHUNK_KEY_PACK(m.zone_id, m.chunk_id, m.block_off,
				m.size_idx);

	return ctree_insert(c->tree, key, 0);
}

/*
 * bucket_get_rm_block_bestfit --
 *	removes and returns the best-fit memory block for size
 */
static int
bucket_tree_get_rm_block_bestfit(struct block_container *bc,
	struct memory_block *m)
{
	uint64_t key = CHUNK_KEY_PACK(m->zone_id, m->chunk_id, m->block_off,
			m->size_idx);

	struct block_container_ctree *c = (struct block_container_ctree *)bc;

	if ((key = ctree_remove(c->tree, key, 0)) == 0)
		return ENOMEM;

	m->chunk_id = CHUNK_KEY_GET_CHUNK_ID(key);
	m->zone_id = CHUNK_KEY_GET_ZONE_ID(key);
	m->block_off = CHUNK_KEY_GET_BLOCK_OFF(key);
	m->size_idx = CHUNK_KEY_GET_SIZE_IDX(key);

	return 0;
}

/*
 * bucket_get_rm_block_exact -- removes exact match memory block
 */
static int
bucket_tree_get_rm_block_exact(struct block_container *bc,
	struct memory_block m)
{
	uint64_t key = CHUNK_KEY_PACK(m.zone_id, m.chunk_id, m.block_off,
			m.size_idx);

	struct block_container_ctree *c = (struct block_container_ctree *)bc;

	if ((key = ctree_remove(c->tree, key, 1)) == 0)
		return ENOMEM;

	return 0;
}

/*
 * bucket_get_block_exact -- finds exact match memory block
 */
static int
bucket_tree_get_block_exact(struct block_container *bc, struct memory_block m)
{
	uint64_t key = CHUNK_KEY_PACK(m.zone_id, m.chunk_id, m.block_off,
			m.size_idx);

	struct block_container_ctree *c = (struct block_container_ctree *)bc;

	return ctree_find(c->tree, key) == key ? 0 : ENOMEM;
}

/*
 * bucket_is_empty -- checks whether the bucket is empty
 */
static int
bucket_tree_is_empty(struct block_container *bc)
{
	struct block_container_ctree *c = (struct block_container_ctree *)bc;
	return ctree_is_empty(c->tree);
}

struct block_container_ops container_ctree_ops = {
	.insert = bucket_tree_insert_block,
	.get_rm_exact = bucket_tree_get_rm_block_exact,
	.get_rm_bestfit = bucket_tree_get_rm_block_bestfit,
	.get_exact = bucket_tree_get_block_exact,
	.is_empty = bucket_tree_is_empty
};

static struct block_container *
bucket_tree_create(size_t unit_size)
{
	struct block_container_ctree *bc =
		Malloc(sizeof (struct block_container_ctree));
	if (bc == NULL)
		goto error_container_malloc;

	bc->super.type = CONTAINER_CTREE;
	bc->super.unit_size = unit_size;

	bc->tree = ctree_new();
	if (bc->tree == NULL)
		goto error_ctree_new;

	return (struct block_container *)bc;

error_ctree_new:
	Free(bc);

error_container_malloc:
	return NULL;
}

static void
bucket_tree_delete(struct block_container *bc)
{
	struct block_container_ctree *c = (struct block_container_ctree *)bc;
	ctree_delete(c->tree);
	Free(bc);
}

/*
 * bucket_insert_block -- inserts a new memory block into the container
 */
static int
bucket_list_insert_block(struct block_container *bc, PMEMobjpool *pop,
	struct memory_block m)
{
	ASSERT(m.chunk_id < MAX_CHUNK);
	ASSERT(m.zone_id < UINT16_MAX);
	ASSERT(m.size_idx == 1);

	struct block_container_list *c = (struct block_container_list *)bc;

	int ret = 0;

	if ((ret = pthread_mutex_lock(&c->list_lock)) != 0) {
		LOG(2, "failed to acquire bucket list lock");
		return ret;
	}

	uint64_t key = CHUNK_KEY_PACK(m.zone_id, m.chunk_id, m.block_off,
				m.size_idx);

#ifdef USE_VG_MEMCHECK
	bucket_vg_mark_noaccess(pop, bc, m);
#endif

	struct list_block *lb = Malloc(sizeof (struct list_block));
	if (lb == NULL) {
		ret = ENOMEM;
		goto out;
	}

	lb->block = m;

	if (cuckoo_insert(c->map, key, lb) != 0) {
		Free(lb);
		ret = ENOMEM;
		goto out;
	}

	LIST_INSERT_HEAD(&c->blocks, lb, list);

out:
	if (pthread_mutex_unlock(&c->list_lock) != 0) {
		ERR("failed to release bucket list lock");
		ASSERT(0);
	}

	return ret;
}

/*
 * bucket_get_rm_block_bestfit --
 *	removes and returns the best-fit memory block for size
 */
static int
bucket_list_get_rm_block_bestfit(struct block_container *bc,
	struct memory_block *m)
{
	struct block_container_list *c = (struct block_container_list *)bc;

	int ret = 0;

	if ((ret = pthread_mutex_lock(&c->list_lock)) != 0) {
		LOG(2, "failed to acquire bucket list lock");
		return ret;
	}

	struct list_block *lb = LIST_FIRST(&c->blocks);
	if (lb == NULL) {
		ret = ENOMEM;
		goto out;
	}

	LIST_REMOVE(lb, list);

	ASSERT(m->size_idx == 1);

	*m = lb->block;

	uint64_t key = CHUNK_KEY_PACK(m->zone_id, m->chunk_id, m->block_off,
				m->size_idx);

	if (cuckoo_remove(c->map, key) != lb) {
		ERR("Bucket List/Map state mismatch");
		ASSERT(0);
	}

	Free(lb);

out:
	if (pthread_mutex_unlock(&c->list_lock) != 0) {
		ERR("failed to release bucket list lock");
		ASSERT(0);
	}

	return ret;
}

/*
 * bucket_get_rm_block_exact -- removes exact match memory block
 */
static int
bucket_list_get_rm_block_exact(struct block_container *b, struct memory_block m)
{
	struct block_container_list *c = (struct block_container_list *)b;
	uint64_t key = CHUNK_KEY_PACK(m.zone_id, m.chunk_id, m.block_off,
				m.size_idx);
	int ret = 0;
	if ((ret = pthread_mutex_lock(&c->list_lock)) != 0) {
		LOG(2, "failed to acquire bucket list lock");
		return ret;
	}

	struct list_block *lb = cuckoo_remove(c->map, key);
	if (lb == NULL) {
		ret = ENOMEM;
		goto out;
	}

	LIST_REMOVE(lb, list);

out:
	if (pthread_mutex_unlock(&c->list_lock) != 0) {
		ERR("failed to release bucket list lock");
		ASSERT(0);
	}

	return ret;
}

/*
 * bucket_get_block_exact -- finds exact match memory block
 */
static int
bucket_list_get_block_exact(struct block_container *b, struct memory_block m)
{
	struct block_container_list *c = (struct block_container_list *)b;
	uint64_t key = CHUNK_KEY_PACK(m.zone_id, m.chunk_id, m.block_off,
				m.size_idx);

	int ret = 0;
	if ((ret = pthread_mutex_lock(&c->list_lock)) != 0) {
		LOG(2, "failed to acquire bucket list lock");
		return ret;
	}

	ret = cuckoo_get(c->map, key) ? 0 : ENOMEM;

	if (pthread_mutex_unlock(&c->list_lock) != 0) {
		ERR("failed to release bucket list lock");
		ASSERT(0);
	}

	return ret;
}

/*
 * bucket_is_empty -- checks whether the bucket is empty
 */
static int
bucket_list_is_empty(struct block_container *bc)
{
	struct block_container_list *c = (struct block_container_list *)bc;

	int ret = 0;
	if ((ret = pthread_mutex_lock(&c->list_lock)) != 0) {
		LOG(2, "failed to acquire bucket list lock");
		return ret;
	}

	ret = LIST_EMPTY(&c->blocks);

	if (pthread_mutex_unlock(&c->list_lock) != 0) {
		ERR("failed to release bucket list lock");
		ASSERT(0);
	}

	return ret;
}

struct block_container_ops container_list_ops = {
	.insert = bucket_list_insert_block,
	.get_rm_exact = bucket_list_get_rm_block_exact,
	.get_rm_bestfit = bucket_list_get_rm_block_bestfit,
	.get_exact = bucket_list_get_block_exact,
	.is_empty = bucket_list_is_empty
};

static struct block_container *
bucket_list_create(size_t unit_size)
{
	struct block_container_list *bc =
		Malloc(sizeof (struct block_container_list));
	if (bc == NULL)
		goto error_container_malloc;

	bc->super.type = CONTAINER_LIST;
	bc->super.unit_size = unit_size;

	LIST_INIT(&bc->blocks);

	bc->map = cuckoo_new();
	if (bc->map == NULL)
		goto error_map_new;

	if (pthread_mutex_init(&bc->list_lock, NULL) != 0)
		goto error_lock_init;

	return (struct block_container *)bc;

error_lock_init:
	cuckoo_delete(bc->map);
error_map_new:
	Free(bc);

error_container_malloc:
	return NULL;
}

static void
bucket_list_delete(struct block_container *bc)
{
	struct block_container_list *c = (struct block_container_list *)bc;

	struct list_block *l = NULL;
	while ((l = LIST_FIRST(&c->blocks)) != NULL) {
		LIST_REMOVE(l, list);
		Free(l);
	}

	cuckoo_delete(c->map);
	Free(bc);
}

struct {
	struct block_container_ops *ops;
	struct block_container *(*create)(size_t unit_size);
	void (*delete)(struct block_container *c);
} block_containers[MAX_CONTAINER_TYPE] = {
	{},
	{&container_ctree_ops, bucket_tree_create, bucket_tree_delete},
	{&container_list_ops, bucket_list_create, bucket_list_delete},
};

static struct bucket *
bucket_run_create(size_t unit_size, int unit_max)
{
	struct bucket_run *b = Malloc(sizeof (*b));
	if (b == NULL)
		return NULL;

	b->unit_max = unit_max;
	b->bitmap_nallocs = RUN_NALLOCS(unit_size);
	int unused_bits = RUN_BITMAP_SIZE - b->bitmap_nallocs;
	int unused_values = unused_bits / BITS_PER_VALUE;
	b->bitmap_nval = MAX_BITMAP_VALUES - unused_values;

	unused_bits -= (unused_values * BITS_PER_VALUE);
	b->bitmap_lastval = (((1ULL << unused_bits) - 1ULL) <<
				(BITS_PER_VALUE - unused_bits));

	return (struct bucket *)b;
}

static struct bucket *
bucket_huge_create(size_t unit_size, int unit_max)
{
	struct bucket_huge *b = Malloc(sizeof (*b));

	return (struct bucket *)b;
}

static void
bucket_common_delete(struct bucket *b)
{
	Free(b);
}

struct {
	struct bucket *(*create)(size_t unit_size, int unit_max);
	void (*delete)(struct bucket *b);
} bucket_types[MAX_BUCKET_TYPE] = {
	{},
	{bucket_huge_create, bucket_common_delete},
	{bucket_run_create, bucket_common_delete}
};

/*
 * bucket_calc_units -- calculates the number of units requested size requires
 */
static uint32_t
bucket_calc_units(struct bucket *b, size_t size)
{
	ASSERT(size != 0);
	return ((size - 1) / b->unit_size) + 1;
}

/*
 * bucket_new -- allocates and initializes bucket instance
 */
struct bucket *
bucket_new(enum bucket_type type, enum block_container_type ctype,
	size_t unit_size, int unit_max)
{
	ASSERT(unit_size > 0);

	struct bucket *b = bucket_types[type].create(unit_size, unit_max);
	if (b == NULL)
		goto error_bucket_malloc;

	b->type = type;
	b->calc_units = bucket_calc_units;

	b->container = block_containers[ctype].create(unit_size);
	if (b->container == NULL)
		goto error_container_create;

	if ((errno = pthread_mutex_init(&b->lock, NULL)) != 0) {
		ERR("!pthread_mutex_init");
		goto error_mutex_init;
	}

	b->c_ops = block_containers[ctype].ops;
	b->unit_size = unit_size;

	return b;

error_mutex_init:
	block_containers[ctype].delete(b->container);
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
	if ((errno = pthread_mutex_destroy(&b->lock)) != 0)
		ERR("!pthread_mutex_destroy");

	block_containers[b->container->type].delete(b->container);
	bucket_types[b->type].delete(b);
}
