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
 * pmalloc.c -- implementation of pmalloc POSIX-like API
 *
 * The allocator is divided in two distinguishable and seprate parts:
 * the frontend and the backend.
 *
 * The frontend manages the volatile state. It keeps track of the memory chunks
 * and distributes those chunks in a threadsafe way through this API.
 * The main structure that represents the allocator instance is a pool, its the
 * object that is requried for all the functions apart from the open and check.
 * But the the most essential structure that holds the chunks information is a
 * bucket, each instance has a specific class that describes the size range of
 * a chunk this bucket can store the information of. The primary bucket
 * instances are stored in the pool and the secondary buckets present in the
 * arenas - which are used to improve thread scaling by decreasing lock
 * contention all throughout the allocation/free process. The difference
 * between the primary and secondary buckets is that the former actually
 * calls the backend to get the chunks and then distribute them across the
 * arenas.
 *
 * The backend provides the facilities to the frontend with neccessery to get
 * the actual memory addresses from the underlying operating system. Currently
 * the only usable backend is a persistent one which takes an address of a
 * memory-mapped file that resides on persistent memory aware file system and
 * guarantees that all the backend operations are power-fail safe and even
 * if operations are interrupted the backend file is always consistent.
 *
 * The frontend and backend both use a common bucket_object structure which is
 * uniquely identified by either data offset it carries or a unique key assigned
 * by the backend.
 */

#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/param.h>
#include <inttypes.h>
#include "pmalloc.h"
#include "out.h"
#include "util.h"
#include "bucket.h"
#include "arena.h"
#include "backend.h"
#include "pool.h"

/*
 * pool_open -- opens a new persistent pool
 */
struct pmalloc_pool *
pool_open(void *ptr, size_t size, int flags)
{
	LOG(3, "ptr %p size %zu flags %d", ptr, size, flags);

	enum backend_type btype = flags & POOL_OPEN_FLAG_NOOP ?
		BACKEND_NOOP : BACKEND_PERSISTENT;

	return pool_new(ptr, size, btype);
}

/*
 * pool_close -- closes a pool with any type of backend
 */
void
pool_close(struct pmalloc_pool *pool)
{
	LOG(3, "pool %p", pool);

	pool_delete(pool);
}

/*
 * pool_check -- checks consistency of the pool backend
 */
bool
pool_check(void *ptr, size_t size, int flags)
{
	LOG(3, "ptr %p size %zu flags %d", ptr, size, flags);

	enum backend_type btype = flags & POOL_CHECK_FLAG_NOOP ?
		BACKEND_NOOP : BACKEND_PERSISTENT;

	return backend_consistency_check(btype, ptr, size);
}

/*
 * alloc_from_bucket -- (internal) allocates an object from bucket
 */
static void
alloc_from_bucket(struct arena *arena, struct bucket *bucket,
	struct bucket_object *obj, uint64_t *ptr, size_t size)
{
	/*
	 * Each bucket holds collection of chunks that are a multiplication
	 * of the buckets class unit size, and each allocation can consist
	 * of several of those units. For example, if the bucket class unit
	 * size is 4 kilobytes and the allocations requires 11 kilobytes of
	 * free space than the units count is equal to 3 and the allocated
	 * object actually has 12 kilobytes.
	 */
	uint32_t units = bucket_calc_units(bucket, size);

	/*
	 * Failure to find free object would mean that arena_select_bucket
	 * didn't work properly.
	 */
	if (!bucket_get_object(bucket, obj, units)) {
		LOG(4, "Bucket OOM");
		return;
	}

	arena->a_ops->set_alloc_ptr(arena, ptr, obj->data_offset);

	if (!bucket_mark_allocated(bucket, obj)) {
		LOG(4, "Failed to mark object as allocated");
		return;
	}
}

/*
 * pmalloc -- acquires a new object from pool
 *
 * The result of the operation is written persistently to the location
 * referenced by ptr. The *ptr value must be NULL_OFFSET.
 */
void
pmalloc(struct pmalloc_pool *p, uint64_t *ptr, size_t size)
{
	LOG(3, "pool %p ptr %p size %zu", p, ptr, size);

	ASSERT(*ptr == NULL_OFFSET);

	struct arena *arena = pool_select_arena(p);
	if (arena == NULL) {
		LOG(4, "Failed to select arena");
		return;
	}

	if (!arena_guard_up(arena, ptr, GUARD_TYPE_MALLOC)) {
		LOG(4, "Failed to acquire arena guard");
		return;
	}

	struct bucket *bucket = arena_select_bucket(arena, size);
	if (bucket == NULL) {
		LOG(4, "Failed to select a bucket, OOM");
		goto error_select_bucket;
	}

	struct bucket_object obj = {0};
	alloc_from_bucket(arena, bucket, &obj, ptr, size);

error_select_bucket:
	if (!arena_guard_down(arena, ptr, GUARD_TYPE_MALLOC)) {
		LOG(4, "Failed to release arena guard");
		return;
	}
}

/*
 * pfree -- releases an object back to the pool
 *
 * If the operation is succesful a NULL is written persistently to the
 * location referenced by the ptr.
 */
void
pfree(struct pmalloc_pool *p, uint64_t *ptr)
{
	LOG(3, "pool %p ptr %p", p, ptr);

	if (*ptr == NULL_OFFSET)
		return;

	struct arena *arena = pool_select_arena(p);
	if (arena == NULL) {
		LOG(4, "Failed to select arena");
		return;
	}

	struct bucket_object obj = {0};
	if (!bucket_object_locate(&obj, p, *ptr)) {
		LOG(4, "Object already free (double free?)");
		return;
	}

	if (!arena_guard_up(arena, ptr, GUARD_TYPE_FREE)) {
		LOG(4, "Failed to acquire arena guard");
		return;
	}

	/* XXX recycle objects back to their respective arena buckets */
	if (!pool_recycle_object(p, &obj)) {
		LOG(4, "Failed to recycle object!");
		goto error_recycle_object;
	}

	arena->a_ops->set_alloc_ptr(arena, ptr, NULL_OFFSET);

error_recycle_object:
	if (!arena_guard_down(arena, ptr, GUARD_TYPE_FREE)) {
		LOG(4, "Failed to release arena guard");
		return;
	}
}

/*
 * prealloc - resizes or acquires an object from the pool
 */
void
prealloc(struct pmalloc_pool *p, uint64_t *ptr, size_t size)
{
	/* XXX change the interface to something more 'failure-friendly' */
	LOG(3, "pool %p ptr %p size %zu", p, ptr, size);

	if (size == 0) {
		pfree(p, ptr);
		return;
	}

	if (*ptr == NULL_OFFSET) {
		pmalloc(p, ptr, size);
		return;
	}

	struct bucket_object obj = {0};
	bucket_object_locate(&obj, p, *ptr);

	if (obj.real_size >= size) {
		/* no-op */
		return;
	}

	struct arena *arena = pool_select_arena(p);
	if (arena == NULL) {
		LOG(4, "Failed to select arena");
		return;
	}

	if (!arena_guard_up(arena, ptr, GUARD_TYPE_REALLOC)) {
		LOG(4, "Failed to acquire arena guard");
		return;
	}

	struct bucket *bucket = arena_select_bucket(arena, size);
	if (bucket == NULL) {
		LOG(3, "Failed to select a bucket, OOM");
		goto error_select_bucket;
	}

	/* XXX object extend - after support for multiple buckets */

	/*
	 * Doing the following two operations in the reverse order would
	 * result in a short period of time in which there's no valid
	 * object stored in the ptr.
	 */
	struct bucket_object new_obj = {0};
	alloc_from_bucket(arena, bucket, &new_obj, ptr, size);
	if (new_obj.size_idx == 0) { /* new allocation successful */
		LOG(3, "Failed to allocate a bigger object");
		goto error_new_alloc;
	}

	p->p_ops->copy_content(p, &new_obj, &obj);
	if (!pool_recycle_object(p, &obj)) {
		LOG(4, "Failed to recycle object!");
	}

error_new_alloc:
error_select_bucket:
	if (!arena_guard_down(arena, ptr, GUARD_TYPE_REALLOC)) {
		LOG(4, "Failed to release arena guard");
		return;
	}
}

/*
 * pdirect -- returns a direct memory pointer based on the ptr offset
 */
void *
pdirect(struct pmalloc_pool *p, uint64_t ptr)
{
	LOG(3, "pool %p ptr %"PRIu64"", p, ptr);

	ASSERT(p != NULL);
	return p->p_ops->get_direct(p, ptr);
}
