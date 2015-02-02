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
 * backend_noop.c -- implementation of noop backend
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <libpmem.h>
#include "bucket.h"
#include "arena.h"
#include "backend.h"
#include "pool.h"
#include "util.h"
#include "out.h"
#include "backend_noop.h"

static struct bucket_backend_operations noop_bucket_ops = {
	.init_bucket_obj = noop_init_bucket_obj,
	.set_bucket_obj_state = noop_set_bucket_obj_state,
};

static struct arena_backend_operations noop_arena_ops = {
	.set_alloc_ptr = noop_set_alloc_ptr,
	.set_guard = noop_set_guard,
	.clear_guard = noop_clear_guard
};

static struct pool_backend_operations noop_pool_ops = {
	.fill_buckets = noop_fill_buckets,
	.create_bucket_classes = noop_bucket_classes,
	.get_direct = noop_get_direct,
	.locate_bucket_obj = noop_locate_bucket_obj,
	.copy_content = noop_copy_content
};

/*
 * noop_backend_open -- opens a backend with all no-op functions
 */
struct backend *
backend_noop_open(void *ptr, size_t size)
{
	struct backend_noop *backend = Malloc(sizeof (*backend));
	if (backend == NULL) {
		goto error_backend_malloc;
	}

	backend_init(&(backend->super), BACKEND_NOOP,
		&noop_bucket_ops, &noop_arena_ops, &noop_pool_ops);

	return (struct backend *)backend;

error_backend_malloc:
	return NULL;
}


/*
 * noop_backend_close -- closes a noop backend
 */
void
backend_noop_close(struct backend *backend)
{
	ASSERT(backend->type == BACKEND_NOOP);

	struct backend_noop *noop_backend =
		(struct backend_noop *)backend;

	Free(noop_backend);
}

/*
 * backend_noop_consistency_check -- returns true
 */
bool
backend_noop_consistency_check(void *ptr, size_t size)
{
	/* no-op */
	return true;
}

/*
 * noop_set_alloc_ptr -- no-op implementation of set_alloc_ptr
 */
void
noop_set_alloc_ptr(struct arena *arena, uint64_t *ptr,
	uint64_t value)
{
	/* no-op */
}

/*
 * noop_fill_buckets -- no-op implementation of fill_buckets
 */
void
noop_fill_buckets(struct pmalloc_pool *pool)
{
	/* no-op */
}

/*
 * noop_bucket_classes -- no-op implementation of create_bucket_classes
 */
void
noop_bucket_classes(struct pmalloc_pool *pool)
{
	/* no-op */
}

/*
 * noop_get_direct -- no-op implementation of init_bucket_obj
 */
void
noop_init_bucket_obj(struct bucket *bucket, struct bucket_object *obj)
{
	/* no-op */
}

/*
 * noop_set_bucket_obj_state -- no-op implementation of set_bucket_obj_state
 */
bool
noop_set_bucket_obj_state(struct bucket *bucket, struct bucket_object *obj,
	enum bucket_obj_state state)
{
	/* no-op */
	return true;
}

/*
 * noop_get_direct -- no-op implementation of get_direct
 */
void *
noop_get_direct(struct pmalloc_pool *pool, uint64_t ptr)
{
	/* no-op */
	return NULL;
}

/*
 * noop_locate_bucket_obj -- no-op implementation of locate_bucket_obj
 */
bool
noop_locate_bucket_obj(struct pmalloc_pool *pool, struct bucket_object *obj,
	uint64_t data_offset)
{
	/* no-op */
	return true;
}

/*
 * noop_copy_content -- no-op implementation of copy_content
 */
void
noop_copy_content(struct pmalloc_pool *pool, struct bucket_object *dest,
	struct bucket_object *src)
{
	/* no-op */
}

/*
 * noop_set_guard -- no-op implementation of set_guard
 */
void
noop_set_guard(struct arena *arena, enum guard_type type, uint64_t *ptr)
{
	/* no-op */
}

/*
 * noop_clear_guard -- no-op implementation of clear_guard
 */
void
noop_clear_guard(struct arena *arena)
{
	/* no-op */
}
