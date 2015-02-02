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
 * obj_pmalloc_backend.c -- unit test for pmalloc backend interface
 */
#include <stdbool.h>
#include <assert.h>
#include "unittest.h"
#include "pmalloc.h"
#include "bucket.h"
#include "arena.h"
#include "backend.h"
#include "pool.h"
#include "backend_persistent.h"
#include "util.h"

#define	MOCK_BUCKET_OPS ((void *)0xABC)
#define	MOCK_ARENA_OPS ((void *)0xBCD)
#define	MOCK_POOL_OPS ((void *)0xCDE)

void
test_backend()
{
	struct backend mock_backend;
	backend_init(&mock_backend, BACKEND_NOOP, MOCK_BUCKET_OPS,
		MOCK_ARENA_OPS, MOCK_POOL_OPS);

	ASSERT(mock_backend.type == BACKEND_NOOP);
	ASSERT(mock_backend.b_ops == MOCK_BUCKET_OPS);
	ASSERT(mock_backend.a_ops == MOCK_ARENA_OPS);
	ASSERT(mock_backend.p_ops == MOCK_POOL_OPS);
}

#define	MOCK_POOL_SIZE_IDX 100
#define	MOCK_POOL_SIZE CHUNKSIZE * MOCK_POOL_SIZE_IDX

void
test_verify_design_compliance()
{
	ASSERT(sizeof (struct backend_pool_header) == 1024);
	ASSERT(sizeof (struct backend_info_slot) == 32);
	ASSERT(sizeof (struct backend_info_slot_alloc) == 32);
	ASSERT(sizeof (struct backend_info_slot_realloc) == 32);
	ASSERT(sizeof (struct backend_info_slot_free) == 32);
	ASSERT(sizeof (struct backend_chunk_header) == 16);
}

void
test_backend_persistent_consistency_check_false()
{
	struct backend_pool *mock_pool = MALLOC(MOCK_POOL_SIZE);

	memset(mock_pool, 0xAB, MOCK_POOL_SIZE);
	ASSERT(backend_persistent_consistency_check(mock_pool,
		MOCK_POOL_SIZE) == false);

	memset(mock_pool, 0, MOCK_POOL_SIZE);
	ASSERT(backend_persistent_consistency_check(mock_pool,
		MOCK_POOL_SIZE) == false);

	FREE(mock_pool);
}

void
test_backend_persistent_consistency_check_true()
{
	struct backend_pool *mock_pool = MALLOC(MOCK_POOL_SIZE);

	struct backend *mock_backend =
		backend_persistent_open(mock_pool, MOCK_POOL_SIZE);
	ASSERT(backend_persistent_consistency_check(mock_pool, MOCK_POOL_SIZE));

	backend_persistent_close(mock_backend);
	ASSERT(backend_persistent_consistency_check(mock_pool, MOCK_POOL_SIZE));

	FREE(mock_pool);
}

void
test_backend_persistent_fresh_init()
{
	struct backend_pool *mock_pool = MALLOC(MOCK_POOL_SIZE);
	struct backend *mock_backend =
		backend_persistent_open(mock_pool, MOCK_POOL_SIZE);

	ASSERT(mock_pool->primary_header.state == POOL_STATE_OPEN);
	ASSERT(memcmp(mock_pool->primary_header.signature,
		POOL_SIGNATURE, POOL_SIGNATURE_LEN) == 0);

	ASSERT(memcmp(&mock_pool->zone[0].backup_header,
		&mock_pool->primary_header,
		sizeof (struct backend_pool_header)) == 0);

	for (int i = 0; i < MAX_INFO_SLOT; ++i) {
		ASSERT(mock_pool->info_slot[i].type == 0);
	}

	ASSERT(mock_backend != NULL);
	ASSERT(mock_backend->type == BACKEND_PERSISTENT);

	backend_persistent_close(mock_backend);
	ASSERT(backend_persistent_consistency_check(mock_pool, MOCK_POOL_SIZE));
	FREE(mock_pool);
}

struct backend_pool_header valid_mock_hdr = {
	.signature = POOL_SIGNATURE,
	.state = POOL_STATE_CLOSED,
	.major = PERSISTENT_BACKEND_MAJOR,
	.size = MOCK_POOL_SIZE,
	.chunk_size = CHUNKSIZE,
	.chunks_per_zone = MAX_CHUNK,
	.reserved = {0},
	.checksum = 0
};

#define	MOCK_MINOR 999

void
test_backend_persistent_exisiting_closed_open()
{
	struct backend_pool *mock_pool = MALLOC(MOCK_POOL_SIZE);

	/*
	 * Write different minor version to verify that the header wasn't
	 * overwritten by pool open.
	 */
	valid_mock_hdr.minor = MOCK_MINOR;
	util_checksum(&valid_mock_hdr, sizeof (valid_mock_hdr),
		&valid_mock_hdr.checksum, 1);

	mock_pool->primary_header = valid_mock_hdr;
	ASSERT(backend_persistent_consistency_check(mock_pool, MOCK_POOL_SIZE));
	struct backend *mock_backend =
		backend_persistent_open(mock_pool, MOCK_POOL_SIZE);

	ASSERT(mock_pool->primary_header.state == POOL_STATE_OPEN);
	ASSERT(mock_pool->primary_header.minor == MOCK_MINOR);
	ASSERT(mock_pool->zone[0].backup_header.minor == MOCK_MINOR);

	backend_persistent_close(mock_backend);
	ASSERT(backend_persistent_consistency_check(mock_pool, MOCK_POOL_SIZE));
	FREE(mock_pool);
}

void
test_backend_persistent_recover_backup_open()
{
	struct backend_pool *mock_pool = MALLOC(MOCK_POOL_SIZE);

	valid_mock_hdr.minor = MOCK_MINOR;
	util_checksum(&valid_mock_hdr, sizeof (valid_mock_hdr),
		&valid_mock_hdr.checksum, 1);

	mock_pool->zone[0].backup_header = valid_mock_hdr;
	ASSERT(backend_persistent_consistency_check(mock_pool, MOCK_POOL_SIZE));
	struct backend *mock_backend =
		backend_persistent_open(mock_pool, MOCK_POOL_SIZE);

	ASSERT(mock_pool->primary_header.state == POOL_STATE_OPEN);
	ASSERT(mock_pool->primary_header.minor == MOCK_MINOR);
	ASSERT(mock_pool->zone[0].backup_header.minor == MOCK_MINOR);

	backend_persistent_close(mock_backend);
	ASSERT(backend_persistent_consistency_check(mock_pool, MOCK_POOL_SIZE));
	FREE(mock_pool);
}

#define	MOCK_DEST_VALUE 123
uint64_t mock_dest = MOCK_DEST_VALUE;

void
test_backend_persistent_open_slot_recovery_open()
{
	struct backend_pool *mock_pool = MALLOC(MOCK_POOL_SIZE);

	uint64_t *data = (uint64_t *)&mock_pool->zone[0].chunk_data[0].data;

	struct backend_info_slot_alloc mock_slot = {
		.type = INFO_SLOT_TYPE_ALLOC,
		.destination_addr =
			(uint64_t)data -
			(uint64_t)mock_pool
	};

	*data = (uint64_t)data - (uint64_t)mock_pool;

	valid_mock_hdr.minor = MOCK_MINOR;
	valid_mock_hdr.state = POOL_STATE_OPEN;
	util_checksum(&valid_mock_hdr, sizeof (valid_mock_hdr),
		&valid_mock_hdr.checksum, 1);

	mock_pool->info_slot[0] = *((struct backend_info_slot *)&mock_slot);

	mock_pool->primary_header = valid_mock_hdr;
	ASSERT(backend_persistent_consistency_check(mock_pool, MOCK_POOL_SIZE));
	struct backend *mock_backend =
		backend_persistent_open(mock_pool, MOCK_POOL_SIZE);

	ASSERT(mock_pool->info_slot[0].type == 0);
	ASSERT(*data == NULL_OFFSET);
	ASSERT(mock_pool->primary_header.state == POOL_STATE_OPEN);
	ASSERT(mock_pool->primary_header.minor == MOCK_MINOR);

	backend_persistent_close(mock_backend);
	ASSERT(mock_pool->primary_header.state == POOL_STATE_CLOSED);
	ASSERT(backend_persistent_consistency_check(mock_pool, MOCK_POOL_SIZE));
	FREE(mock_pool);
}

void
test_backend_persistent_open_invalid_major()
{
	struct backend_pool *mock_pool = MALLOC(MOCK_POOL_SIZE);

	valid_mock_hdr.major += 1;
	valid_mock_hdr.minor = MOCK_MINOR;
	util_checksum(&valid_mock_hdr, sizeof (valid_mock_hdr),
		&valid_mock_hdr.checksum, 1);

	mock_pool->primary_header = valid_mock_hdr;

	struct backend *mock_backend =
		backend_persistent_open(mock_pool, MOCK_POOL_SIZE);

	ASSERT(mock_backend == NULL);
	FREE(mock_pool);
}

void
test_backend_persistent_open_invalid_size()
{
	struct backend_pool *mock_pool = MALLOC(MOCK_POOL_SIZE);

	valid_mock_hdr.size += 1;
	valid_mock_hdr.minor = MOCK_MINOR;
	util_checksum(&valid_mock_hdr, sizeof (valid_mock_hdr),
		&valid_mock_hdr.checksum, 1);

	mock_pool->primary_header = valid_mock_hdr;

	struct backend *mock_backend =
		backend_persistent_open(mock_pool, MOCK_POOL_SIZE);

	ASSERT(mock_backend == NULL);
	FREE(mock_pool);
}

#define	TEST_VAL_A 5
#define	TEST_VAL_B 10
uint64_t val = TEST_VAL_A;

bool mock_persist_called = false;

void
mock_persist(void *addr, size_t len)
{
	uint64_t *p_val = addr;
	ASSERT(p_val == &val);
	ASSERT(*p_val == TEST_VAL_B);
	mock_persist_called = true;
}

void
test_backend_persistent_set_ptr()
{
	struct backend_persistent mock_backend = {
		.persist = mock_persist
	};

	struct pmalloc_pool mock_pool = {
		.backend = (struct backend *)&mock_backend
	};

	struct arena mock_arena = {
		.pool = &mock_pool
	};

	persistent_set_alloc_ptr(&mock_arena, &val, TEST_VAL_B);
	ASSERT(val == TEST_VAL_B);
	ASSERT(mock_persist_called);
}

FUNC_WILL_RETURN(bucket_register_class, 1);

void
test_backend_persistent_bucket_classes()
{
	struct pmalloc_pool mock_pool;
	persistent_bucket_classes(&mock_pool);
}

void
noop_persist(void *addr, size_t len)
{
	/* no-op */
}

#define	MOCK_BUCKET_ID 0
struct bucket mock_bucket;

#define	CHUNK_MAX_SIZE_IDX MOCK_POOL_SIZE_IDX - 1
#define	CHUNK_0_SIZE_IDX 20
#define	CHUNK_1_SIZE_IDX CHUNK_MAX_SIZE_IDX - CHUNK_0_SIZE_IDX

#define	MOCK_BUCKET_OBJ_MAX_REAL_SIZE (CHUNK_MAX_SIZE_IDX) * CHUNKSIZE
#define	MOCK_BUCKET_OBJ_0_REAL_SIZE (CHUNK_0_SIZE_IDX) * CHUNKSIZE
#define	MOCK_BUCKET_OBJ_1_REAL_SIZE (CHUNK_1_SIZE_IDX) * CHUNKSIZE

#define	NEW_CHUNK_SIZE_IDX (CHUNK_0_SIZE_IDX / 2)

FUNC_WILL_RETURN(get_bucket_class_id_by_size, MOCK_BUCKET_ID);

static int inv_n = 0;

FUNC_WRAP_BEGIN(bucket_add_object, bool, struct bucket *bucket,
	struct bucket_object *obj)
FUNC_WRAP_ARG_EQ(bucket, &mock_bucket)
switch (inv_n++) {
case 0:
	ASSERT(obj->real_size > 1);
	break;
case 1:
	FUNC_WRAP_ARG_EQ(obj->real_size, MOCK_BUCKET_OBJ_0_REAL_SIZE)
	break;
case 2:
	FUNC_WRAP_ARG_EQ(obj->real_size, MOCK_BUCKET_OBJ_1_REAL_SIZE)
	break;
case 3:
	FUNC_WRAP_ARG_EQ(obj->size_idx, NEW_CHUNK_SIZE_IDX)
	break;
}

FUNC_WRAP_END(true)

void
test_backend_persistent_fill_buckets_new_obj()
{
	struct backend_pool *mock_backend_pool = MALLOC(MOCK_POOL_SIZE);

	struct backend_persistent mock_backend = {
		.persist = noop_persist,
		.zones_exhausted = 0,
		.pool_size = MOCK_POOL_SIZE,
		.pool = mock_backend_pool
	};

	struct pmalloc_pool mock_pool = {
		.backend = (struct backend *)&mock_backend,
		.buckets = {NULL},
	};
	mock_pool.buckets[MOCK_BUCKET_ID] = &mock_bucket;

	persistent_fill_buckets(&mock_pool);

	FREE(mock_backend_pool);
}

void
test_backend_persistent_fill_buckets_exisiting_objs()
{
	struct backend_pool *mock_backend_pool = MALLOC(MOCK_POOL_SIZE);

	struct backend_zone *zone = &mock_backend_pool->zone[0];
	struct backend_chunk_header mock_hdr_0 = {
		.magic = CHUNK_HEADER_MAGIC,
		.flags = 0,
		.size_idx = CHUNK_0_SIZE_IDX,
		.type = CHUNK_TYPE_BASE,
		.type_specific = 0
	};
	struct backend_chunk_header mock_hdr_1 = {
		.magic = CHUNK_HEADER_MAGIC,
		.flags = 0,
		.size_idx = CHUNK_1_SIZE_IDX,
		.type = CHUNK_TYPE_BASE,
		.type_specific = 0
	};
	zone->chunk_header[0] = mock_hdr_0;
	zone->chunk_header[CHUNK_0_SIZE_IDX] = mock_hdr_1;

	struct backend_persistent mock_backend = {
		.persist = noop_persist,
		.zones_exhausted = 0,
		.pool_size = MOCK_POOL_SIZE,
		.pool = mock_backend_pool
	};

	struct pmalloc_pool mock_pool = {
		.backend = (struct backend *)&mock_backend,
		.buckets = {NULL},
	};
	mock_pool.buckets[MOCK_BUCKET_ID] = &mock_bucket;

	persistent_fill_buckets(&mock_pool);

	FREE(mock_backend_pool);
}

void
test_backend_persistent_init_obj()
{
	struct backend_pool *mock_backend_pool = MALLOC(MOCK_POOL_SIZE);

	struct backend_zone *zone = &mock_backend_pool->zone[0];
	struct backend_chunk_header mock_hdr_0 = {
		.magic = CHUNK_HEADER_MAGIC,
		.flags = 0,
		.size_idx = CHUNK_0_SIZE_IDX,
		.type = CHUNK_TYPE_BASE,
		.type_specific = 0
	};
	zone->chunk_header[0] = mock_hdr_0;

	struct backend_persistent mock_backend = {
		.persist = noop_persist,
		.zones_exhausted = 0,
		.pool_size = MOCK_POOL_SIZE,
		.pool = mock_backend_pool
	};

	struct pmalloc_pool mock_pool = {
		.backend = (struct backend *)&mock_backend,
		.buckets = {NULL},
	};

	mock_pool.buckets[MOCK_BUCKET_ID] = &mock_bucket;

	mock_bucket.pool = &mock_pool;

	struct bucket_object obj;
	obj.size_idx = NEW_CHUNK_SIZE_IDX;
	obj.unique_id = 0; /* zone 0 chunk 0 */
	persistent_init_bucket_obj(&mock_bucket, &obj);
	ASSERT(obj.size_idx == NEW_CHUNK_SIZE_IDX);
	ASSERT(zone->chunk_header[NEW_CHUNK_SIZE_IDX].magic ==
		CHUNK_HEADER_MAGIC);
	ASSERT(zone->chunk_header[NEW_CHUNK_SIZE_IDX].size_idx ==
		CHUNK_0_SIZE_IDX - NEW_CHUNK_SIZE_IDX);

	FREE(mock_backend_pool);
}

static void *
noop_memcpy(void *dest, void *src, size_t len)
{
	return dest;
}

static void *
noop_memset(void *dest, int c, size_t len)
{
	return dest;
}

void
test_backend_persistent_obj_state()
{
	struct backend_pool *mock_backend_pool = MALLOC(MOCK_POOL_SIZE);

	struct backend_zone *zone = &mock_backend_pool->zone[0];
	struct backend_chunk_header mock_hdr_0 = {
		.magic = CHUNK_HEADER_MAGIC,
		.flags = 0,
		.size_idx = CHUNK_0_SIZE_IDX,
		.type = CHUNK_TYPE_BASE,
		.type_specific = 0
	};
	zone->chunk_header[0] = mock_hdr_0;

	struct backend_persistent mock_backend = {
		.persist = noop_persist,
		.pmemcpy = noop_memcpy,
		.pmemset = noop_memset,
		.zones_exhausted = 0,
		.pool_size = MOCK_POOL_SIZE,
		.pool = mock_backend_pool
	};

	struct pmalloc_pool mock_pool = {
		.backend = (struct backend *)&mock_backend,
		.buckets = {NULL},
	};

	mock_pool.buckets[MOCK_BUCKET_ID] = &mock_bucket;

	mock_bucket.pool = &mock_pool;

	struct bucket_object obj;
	obj.unique_id = 0; /* zone 0 chunk 0 */
	obj.real_size = 0;

	ASSERT(persistent_set_bucket_obj_state(&mock_bucket, &obj,
		BUCKET_OBJ_STATE_ALLOCATED) == true);
	ASSERT(persistent_set_bucket_obj_state(&mock_bucket, &obj,
		BUCKET_OBJ_STATE_ALLOCATED) == false);
	ASSERT(zone->chunk_header[0].flags & CHUNK_FLAG_USED);

	ASSERT(persistent_set_bucket_obj_state(&mock_bucket, &obj,
		BUCKET_OBJ_STATE_FREE) == true);
	ASSERT(persistent_set_bucket_obj_state(&mock_bucket, &obj,
		BUCKET_OBJ_STATE_FREE) == false);
	ASSERT((zone->chunk_header[0].flags & CHUNK_FLAG_USED) == 0);

	FREE(mock_backend_pool);
}

void
test_backend_persistent_locate_obj()
{
	struct backend_pool *mock_backend_pool = MALLOC(MOCK_POOL_SIZE);

	struct backend_zone *zone = &mock_backend_pool->zone[0];
	struct backend_chunk_header mock_hdr_0 = {
		.magic = CHUNK_HEADER_MAGIC,
		.flags = 0,
		.size_idx = CHUNK_0_SIZE_IDX,
		.type = CHUNK_TYPE_BASE,
		.type_specific = 0
	};
	struct backend_chunk_header mock_hdr_1 = {
		.magic = CHUNK_HEADER_MAGIC,
		.flags = CHUNK_FLAG_USED,
		.size_idx = CHUNK_1_SIZE_IDX,
		.type = CHUNK_TYPE_BASE,
		.type_specific = 0
	};
	zone->chunk_header[0] = mock_hdr_0;
	zone->chunk_header[CHUNK_0_SIZE_IDX] = mock_hdr_1;

	uint64_t data_offset = (uint64_t)&zone->chunk_data[CHUNK_0_SIZE_IDX] -
		(uint64_t)mock_backend_pool;

	struct backend_persistent mock_backend = {
		.persist = noop_persist,
		.zones_exhausted = 0,
		.pool_size = MOCK_POOL_SIZE,
		.pool = mock_backend_pool
	};

	struct pmalloc_pool mock_pool = {
		.backend = (struct backend *)&mock_backend,
		.buckets = {NULL},
	};
	mock_pool.buckets[MOCK_BUCKET_ID] = &mock_bucket;

	struct bucket_object obj = {0};

	ASSERT(persistent_locate_bucket_obj(&mock_pool, &obj, data_offset));

	ASSERT(obj.size_idx == CHUNK_1_SIZE_IDX);

	FREE(mock_backend_pool);
}

#define	MOCK_BACKEND_POOL_PTR ((void *)0xABC)
#define	MOCK_TEST_OFFSET_PTR 123

void
test_backend_persistent_direct()
{
	struct backend_persistent mock_backend = {
		.pool = MOCK_BACKEND_POOL_PTR
	};
	struct pmalloc_pool mock_pool = {
		.backend = (struct backend *)&mock_backend
	};
	void *dptr = persistent_get_direct(&mock_pool, MOCK_TEST_OFFSET_PTR);
	ASSERTrange(dptr, MOCK_BACKEND_POOL_PTR, MOCK_TEST_OFFSET_PTR + 1);
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_pmalloc_backend");

	test_backend();
	test_verify_design_compliance();
	test_backend_persistent_consistency_check_false();
	test_backend_persistent_consistency_check_true();
	test_backend_persistent_fresh_init();
	test_backend_persistent_exisiting_closed_open();
	test_backend_persistent_recover_backup_open();
	test_backend_persistent_open_slot_recovery_open();
	test_backend_persistent_open_invalid_major();
	test_backend_persistent_open_invalid_size();
	test_backend_persistent_set_ptr();
	test_backend_persistent_bucket_classes();
	test_backend_persistent_fill_buckets_new_obj();
	test_backend_persistent_fill_buckets_exisiting_objs();
	test_backend_persistent_init_obj();
	test_backend_persistent_obj_state();
	test_backend_persistent_locate_obj();
	test_backend_persistent_direct();

	DONE(NULL);
}
