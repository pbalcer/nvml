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
 * bucket.c -- implementation of bucket
 *
 * Bucket is the structure that stores and manages objects. Besides the obvious
 * interfaces to retrieve or store the objects, it acts as a intermediary
 * between the frontend API and the backend in all things related to objects.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "bucket.h"
#include "arena.h"
#include "backend.h"
#include "pool.h"
#include "container.h"
#include "out.h"
#include "util.h"

/*
 * bucket_register_class - determines the bucket class for the size
 */
int
get_bucket_class_id_by_size(struct pmalloc_pool *p, size_t size)
{
	/* XXX */
	return 0;
}

/*
 * bucket_register_class - register a new bucket prototype class
 */
int
bucket_register_class(struct pmalloc_pool *p, struct bucket_class c)
{
	for (int i = 0; i < MAX_BUCKETS; ++i) {
		if (p->bucket_classes[i].unit_size == 0) {
			p->bucket_classes[i] = c;
			return i;
		}
	}

	return -1;
}

/*
 * bucket_unregister_class - unregister a bucket class
 *
 * This function does NOT affect existing buckets.
 */
bool
bucket_unregister_class(struct pmalloc_pool *p, int class_id)
{
	if (p->bucket_classes[class_id].unit_size == 0) {
		return false;
	}

	struct bucket_class empty = {0};
	p->bucket_classes[class_id] = empty;

	return true;
}

/*
 * bucket_new -- allocate and initialize new bucket object
 */
struct bucket *
bucket_new(struct pmalloc_pool *p, int class_id)
{
	/*
	 * This would mean the class is not registered, which should never
	 * happen assuming correct implementation.
	 */
	ASSERT(p->bucket_classes[class_id].unit_size != 0);

	struct bucket *bucket = Malloc(sizeof (*bucket));
	if (bucket == NULL) {
		goto error_bucket_malloc;
	}

	bucket->objects = container_new(DEFAULT_BUCKET_CONTAINER_TYPE);
	if (bucket->objects == NULL) {
		goto error_container_new;
	}

	bucket->class = p->bucket_classes[class_id];
	bucket->pool = p;
	bucket->b_ops = p->backend->b_ops;

	return bucket;

error_container_new:
	Free(bucket);
error_bucket_malloc:
	return NULL;
}

/*
 * bucket_delete -- deinitialize and free bucket object
 */
void
bucket_delete(struct bucket *bucket)
{
	container_delete(bucket->objects);
	Free(bucket);
}

/*
 * bucket_transfer_objects -- removes certain number of objects from the bucket
 *
 * Returns a NULL-terminated list of objects that can be then inserted into
 * another bucket.
 */
struct bucket_object **
bucket_transfer_objects(struct bucket *bucket)
{
	/* XXX */
	return NULL;
}

/*
 * bucket_object_locate -- locates a bucket object based on the pointer
 */
bool
bucket_object_locate(struct bucket_object *obj, struct pmalloc_pool *p,
	uint64_t ptr)
{
	return p->p_ops->locate_bucket_obj(p, obj, ptr);
}

/*
 * bucket_calc_units -- calculates the number of units needed for size
 */
uint32_t
bucket_calc_units(struct bucket *bucket, size_t size)
{
	return ((size - 1) / bucket->class.unit_size);
}

#define	OBJ_KEY(u, s) ((uint64_t)(u) << 32 | (s))

/*
 * bucket_get_object -- init an object with the required unit size
 */
bool
bucket_get_object(struct bucket *bucket, struct bucket_object *obj,
	uint32_t units)
{
	obj->unique_id = bucket->objects->c_ops->get_rm_ge(bucket->objects,
		OBJ_KEY(0, units));

	obj->size_idx = units;

	if (obj->unique_id == NULL_VAL) {
		return false;
	}

	bucket->b_ops->init_bucket_obj(bucket, obj);

	if (obj->size_idx >= units) {
		return true;
	}

	/* There's really no object of this size, put the max obj back. */
	bucket_add_object(bucket, obj);

	return false;
}

/*
 * bucket_mark_allocated -- marks underlying memory block as allocated
 */
bool
bucket_mark_allocated(struct bucket *bucket, struct bucket_object *obj)
{
	return bucket->b_ops->set_bucket_obj_state(bucket, obj,
		BUCKET_OBJ_STATE_ALLOCATED);
}

/*
 * bucket_add_object -- adds object to the bucket
 *
 * The memory block this object represents must be already freed, otherwise a
 * double allocation of the same memory may occur.
 */
bool
bucket_add_object(struct bucket *bucket, struct bucket_object *obj)
{
	return bucket->objects->c_ops->add(bucket->objects,
		OBJ_KEY(obj->unique_id, obj->size_idx), obj->unique_id);
}
