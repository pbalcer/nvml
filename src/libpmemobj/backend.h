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
 * backend.h -- internal definitions for backend
 */

enum backend_type {
	BACKEND_NOOP,
	BACKEND_PERSISTENT,
	/* BACKEND_VOLATILE, */
	MAX_BACKEND
};

enum bucket_obj_state {
	BUCKET_OBJ_STATE_UNKNOWN,
	BUCKET_OBJ_STATE_ALLOCATED,
	BUCKET_OBJ_STATE_FREE,

	MAX_BUCKET_OBJ_STATE
};

struct bucket_backend_operations {
	/*
	 * init_bucket_obj
	 *
	 * Based on the unique id of the object, fill in rest of the values.
	 */
	void (*init_bucket_obj)(struct bucket *bucket,
		struct bucket_object *obj);

	/*
	 * set_bucket_obj_state
	 *
	 * Actually allocate or free the object.
	 */
	bool (*set_bucket_obj_state)(struct bucket *bucket,
		struct bucket_object *obj, enum bucket_obj_state state);
};

struct arena_backend_operations {
	/*
	 * set_alloc_ptr
	 *
	 * Set the value to the location referenced by the pointer. Called by
	 * the interface functions to update the location to which the
	 * allocation/free is being made to.
	 */
	void (*set_alloc_ptr)(struct arena *arena,
		uint64_t *ptr, uint64_t value);

	/*
	 * set_guard
	 *
	 * Acquire all locks or set structures required to perform an allocation
	 * of the ptr in the arena.
	 */
	void (*set_guard)(struct arena *arena, enum guard_type type,
		uint64_t *ptr);

	/*
	 * clear_guard
	 *
	 * Release above mentioned procaustions.
	 */
	void (*clear_guard)(struct arena *arena);
};

struct pool_backend_operations {
	/*
	 * get_direct
	 *
	 * Return a valid memory pointer contained within the backend based
	 * on the value stored in the ptr offset. Called by the pdirect
	 * interface function.
	 */
	void *(*get_direct)(struct pmalloc_pool *pool, uint64_t ptr);

	/*
	 * create_bucket_classes
	 *
	 * Create classes in the pool that can support all the objects provided
	 * by the backend. Called once at pool initialization time.
	 */
	void (*create_bucket_classes)(struct pmalloc_pool *pool);

	/*
	 * fill_buckets
	 *
	 * Add objects to the non-null buckets in the pool. Called at pool
	 * initialization and when there are no more objects in the buckets.
	 */
	void (*fill_buckets)(struct pmalloc_pool *pool);

	/*
	 * locate_bucket_obj
	 *
	 * Fill in the bucket object values based on the data offset.
	 */
	bool (*locate_bucket_obj)(struct pmalloc_pool *pool,
		struct bucket_object *obj, uint64_t data_offset);

	/*
	 * copy_content
	 *
	 * Copy content of one object to another, used by reallocations.
	 */
	void (*copy_content)(struct pmalloc_pool *pool,
		struct bucket_object *dest, struct bucket_object *src);
};

struct backend {
	enum backend_type type;
	struct bucket_backend_operations *b_ops;
	struct arena_backend_operations *a_ops;
	struct pool_backend_operations *p_ops;
};

struct backend *backend_open(enum backend_type type, void *ptr, size_t size);
void backend_close(struct backend *backend);

void backend_init(struct backend *backend, enum backend_type type,
	struct bucket_backend_operations *b_ops,
	struct arena_backend_operations *a_ops,
	struct pool_backend_operations *p_ops);

bool backend_consistency_check(enum backend_type type,
	void *ptr, size_t size);
