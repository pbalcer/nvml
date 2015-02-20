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
 * backend_noop.h -- internal definitions for noop backend
 */

struct backend_noop {
	struct backend super;
};

struct backend *backend_noop_open(void *ptr, size_t size);
void backend_noop_close(struct backend *backend);
bool backend_noop_consistency_check(void *ptr, size_t size);

void noop_bucket_classes(struct pmalloc_pool *pool);
void noop_fill_buckets(struct pmalloc_pool *pool);
void noop_set_alloc_ptr(struct arena *arena, uint64_t *ptr, uint64_t value);
void noop_init_bucket_obj(struct bucket *bucket, struct bucket_object *obj);
bool noop_set_bucket_obj_state(struct bucket *bucket,
	struct bucket_object *obj, enum bucket_obj_state state);
bool noop_locate_bucket_obj(struct pmalloc_pool *pool,
	struct bucket_object *obj, uint64_t data_offset);
void *noop_get_direct(struct pmalloc_pool *pool, uint64_t ptr);
void noop_copy_content(struct pmalloc_pool *pool, struct bucket_object *dest,
	struct bucket_object *src);
void noop_set_guard(struct arena *arena, enum guard_type type, uint64_t *ptr);
void noop_clear_guard(struct arena *arena);
