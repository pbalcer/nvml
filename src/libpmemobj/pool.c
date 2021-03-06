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
 * pool.c -- implementation of pmalloc pool
 *
 * The pool manages the entire volatile state of the allocator and stores the
 * backend instance. It serves as a primary container for objects aswell as
 * provides the means for the frontend interface to select an appropriate arena
 * for the thread.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "util.h"
#include "out.h"
#include "bucket.h"
#include "arena.h"
#include "backend.h"
#include "pool.h"
#include "container.h"

/*
 * This variable is used to quickly select an arena for the thread, it is
 * shared between pools so the distribution of arenas may not always be ideal
 * for each pool.
 */
static __thread int arena_id = -1;

/*
 * create_default_buckets -- (internal) creates pool bucket for each class
 */
static void
create_default_buckets(struct pmalloc_pool *p)
{
	for (int i = 0; i < MAX_BUCKETS; ++i) {
		if (p->bucket_classes[i].unit_size != 0) {
			ASSERT(p->buckets[i] == NULL);
			p->buckets[i] = bucket_new(p, i);
		}
	}
}

/*
 * pool_new -- allocates and initializes new pool object
 */
struct pmalloc_pool *
pool_new(void *ptr, size_t size, enum backend_type type)
{
	struct pmalloc_pool *pool = Malloc(sizeof (*pool));
	if (pool == NULL) {
		goto error_pool_malloc;
	}

	pool->backend = backend_open(type, ptr, size);
	if (pool->backend == NULL) {
		goto error_backend_new;
	}

	pool->lock = Malloc(sizeof (*pool->lock));
	if (pool->lock == NULL) {
		goto error_lock_malloc;
	}

	if (pthread_mutex_init(pool->lock, NULL) != 0) {
		goto error_lock_init;
	}

	memset(pool->bucket_classes, 0, sizeof (pool->bucket_classes));
	memset(pool->buckets, 0, sizeof (pool->buckets));
	memset(pool->arenas, 0, sizeof (pool->arenas));

	pool->p_ops = pool->backend->p_ops;

	pool->p_ops->create_bucket_classes(pool);

	create_default_buckets(pool);

	pool->p_ops->fill_buckets(pool);

	return pool;

error_lock_init:
	Free(pool->lock);
error_lock_malloc:
	backend_close(pool->backend);
error_backend_new:
	Free(pool);
error_pool_malloc:
	return NULL;
}

/*
 * pool_delete -- deinitializes and frees pool object
 */
void
pool_delete(struct pmalloc_pool *p)
{
	for (int i = 0; i < MAX_BUCKETS; ++i) {
		if (p->buckets[i] != NULL)
			bucket_delete(p->buckets[i]);
	}

	for (int i = 0; i < MAX_ARENAS; ++i) {
		if (p->arenas[i] != NULL) {
			arena_delete(p->arenas[i]);
		}
	}

	if (pthread_mutex_destroy(p->lock) != 0) {
		LOG(4, "Failed to destroy pool lock");
	}

	backend_close(p->backend);

	Free(p);
}

/*
 * select_arena_id -- (internal) finds a least-used arena
 */
static int
select_arena_id(struct pmalloc_pool *p)
{
	int min_arena_threads = ~0;
	int id = -1;
	for (int i = 0; i < MAX_ARENAS; ++i) {
		if (p->arenas[i] == NULL) {
			id = i;
			break;
		}

		if (p->arenas[i]->associated_threads < min_arena_threads) {
			id = i;
			min_arena_threads = p->arenas[i]->associated_threads;
		}
	}

	return id;
}

/*
 * select_thread_arena_slow --
 * (internal) selects an arena from the pool which hopefully will provide the
 * least lock contention.
 */
static struct arena *
select_thread_arena_slow(struct pmalloc_pool *p)
{
	if (pthread_mutex_lock(p->lock) != 0) {
		LOG(4, "Failed to acquire pool mutex");
		return NULL;
	}

	if (arena_id == -1) {
		arena_id = select_arena_id(p);
	}

	if (p->arenas[arena_id] == NULL) {
		if ((p->arenas[arena_id] = arena_new(p, arena_id)) == NULL) {
			goto error_arena_new;
		}
	}

	p->arenas[arena_id]->associated_threads += 1;

error_arena_new:
	if (pthread_mutex_unlock(p->lock) != 0) {
		LOG(4, "Failed to release pool mutex");
	}

	return p->arenas[arena_id];
}

/*
 * pool_select_arena -- selects an arena associated with current thread
 */
struct arena *
pool_select_arena(struct pmalloc_pool *p)
{
	return (arena_id == -1 || p->arenas[arena_id] == NULL) ?
		select_thread_arena_slow(p) :
		p->arenas[arena_id];
}

/*
 * pool_recycle_object -- adds the object to the global pool bucket
 */
bool
pool_recycle_object(struct pmalloc_pool *p, struct bucket_object *obj)
{
	int class_id = get_bucket_class_id_by_size(p, obj->real_size);
	if (p->buckets[class_id] == NULL) {
		if ((p->buckets[class_id] = bucket_new(p, class_id)) == NULL)
			return false;
	}
	struct bucket *b = p->buckets[class_id];

	if (!b->b_ops->set_bucket_obj_state(b, obj, BUCKET_OBJ_STATE_FREE))
		return false;

	if (obj->unique_id != NULL_VAL && !bucket_add_object(b, obj))
		return false;

	return true;
}
