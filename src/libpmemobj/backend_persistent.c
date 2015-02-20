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
 * backend_persistent.c -- implementation of persistent backend
 *
 * This backend guarantees that the underlying memory-mapped file is always
 * in consistent state even if the application crashes in the middle of
 * any of the operations implemented here. It also makes sure that once
 * an allocation starts it is either finished or rolled back the next time
 * this backend is opened.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <libpmem.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include "bucket.h"
#include "arena.h"
#include "backend.h"
#include "util.h"
#include "out.h"
#include "backend_persistent.h"
#include "pool.h"
#include "container.h"
#include "pmalloc.h"

/* macros used to pack/unpack 32bit unique chunk id */
#define	UID_PACK(c, z) ((uint32_t)(c) << 16 | (z))
#define	UID_CHUNK_IDX(u) ((uint16_t)((u & 0xFFFF0000) >> 16))
#define	UID_ZONE_IDX(u) ((uint16_t)(u & 0xFFFF))

static struct bucket_backend_operations persistent_bucket_ops = {
	.init_bucket_obj = persistent_init_bucket_obj,
	.set_bucket_obj_state = persistent_set_bucket_obj_state,
};

static struct arena_backend_operations persistent_arena_ops = {
	.set_alloc_ptr = persistent_set_alloc_ptr,
	.set_guard = persistent_set_guard,
	.clear_guard = persistent_clear_guard
};

static struct pool_backend_operations persistent_pool_ops = {
	.fill_buckets = persistent_fill_buckets,
	.create_bucket_classes = persistent_bucket_classes,
	.get_direct = persistent_get_direct,
	.locate_bucket_obj = persistent_locate_bucket_obj,
	.copy_content = persistent_copy_content
};

/*
 * verify_header -- (internal) check if the header is consistent
 */
static bool
verify_header(struct backend_pool_header *h)
{
	if (util_checksum(h, sizeof (*h), &h->checksum, 0) != 1) {
		return false;
	}

	if (memcmp(h->signature, POOL_SIGNATURE, POOL_SIGNATURE_LEN) != 0) {
		return false;
	}

	return true;
}

/*
 * copy_header -- (internal) create a copy of a header
 */
static void
copy_header(struct backend_persistent *b, struct backend_pool_header *left,
	struct backend_pool_header *right)
{
	b->pmemcpy(left, right, sizeof (*left));
}

/*
 * recover_primary_header -- (internal) check backups for a valid header copy
 */
static bool
recover_primary_header(struct backend_persistent *b)
{
	for (int i = 0; i < b->max_zone; ++i) {
		if (verify_header(&b->pool->zone[i].backup_header)) {
			copy_header(b, &b->pool->primary_header,
				&b->pool->zone[i].backup_header);
			return true;
		}
	}

	return false;
}

/*
 * zero_info_slots -- (internal) zerofill all info slot structures
 */
static void
zero_info_slots(struct backend_persistent *b)
{
	b->pmemset(b->pool->info_slot, 0, sizeof (b->pool->info_slot));
}

/*
 * write_primary_pool_header -- (internal) create a fresh pool header
 */
static void
write_primary_pool_header(struct backend_persistent *b)
{
	struct backend_pool_header hdr = {
		.signature = POOL_SIGNATURE,
		.flags = 0,
		.state = POOL_STATE_CLOSED,
		.major = PERSISTENT_BACKEND_MAJOR,
		.minor = PERSISTENT_BACKEND_MINOR,
		.size = b->pool_size,
		.chunk_size = CHUNKSIZE,
		.chunks_per_zone = MAX_CHUNK,
		.reserved = {0},
		.checksum = 0
	};
	util_checksum(&hdr, sizeof (hdr), &hdr.checksum, 1);
	copy_header(b, &b->pool->primary_header, &hdr);
}

/*
 * write_backup_pool_headers -- (internal) copy primary header into backups
 */
static void
write_backup_pool_headers(struct backend_persistent *b)
{
	for (int i = 0; i < b->max_zone; ++i) {
		copy_header(b, &b->pool->zone[i].backup_header,
			&b->pool->primary_header);
	}
}

/*
 * write_pool_layout -- (internal) create a fresh pool layout
 */
static void
write_pool_layout(struct backend_persistent *b)
{
	zero_info_slots(b);
	write_primary_pool_header(b);
	write_backup_pool_headers(b);
}

/*
 * get_pool_state -- (internal) returns state of the pool
 */
static enum pool_state
get_pool_state(struct backend_persistent *b)
{
	return b->pool->primary_header.state;
}

/*
 * set_pool_state -- (internal) change pool state
 *
 * Writes the state into the primary header first and then waterfalls it into
 * all of the backups.
 */
static void
set_pool_state(struct backend_persistent *b, enum pool_state state)
{
	struct backend_pool_header *hdrp = &b->pool->primary_header;
	hdrp->state = state;
	util_checksum(hdrp, sizeof (*hdrp), &hdrp->checksum, 1);
	b->persist(&hdrp->checksum, sizeof (hdrp->checksum));
	write_backup_pool_headers(b);
}

/*
 * set_chunk_flag -- (internal) persistently set a chunk flag
 */
static bool
set_chunk_flag(struct backend_persistent *b, struct backend_chunk_header *c,
	enum chunk_flag flag)
{
	if (c->flags & flag) {
		return false;
	}

	c->flags |= flag;
	b->persist(c, sizeof (*c));

	return true;
}

/*
 * clear_chunk_flag -- (internal) persistently clear a chunk flag
 */
static bool
clear_chunk_flag(struct backend_persistent *b, struct backend_chunk_header *c,
	enum chunk_flag flag)
{
	if ((c->flags & flag) == 0) {
		return false;
	}

	c->flags &= ~(flag);
	b->persist(c, sizeof (*c));

	return true;
}

/*
 * get_chunk_by_offset --
 *	(internal) find a chunk header based on the data offset
 */
static struct backend_chunk_header *
get_chunk_by_offset(struct backend_persistent *backend, uint64_t data_offset,
	uint16_t *zone_idx, uint16_t *chunk_idx)
{

	ASSERT(data_offset < backend->pool_size);

	/* There's gotta be a better way to do this... */
	data_offset -= sizeof (struct backend_pool_header) +
		sizeof (struct backend_info_slot) * MAX_INFO_SLOT;
	uint64_t max_zone_size = sizeof (struct backend_pool_header) +
		sizeof (struct backend_chunk_header) * MAX_CHUNK +
		sizeof (struct backend_chunk) * MAX_CHUNK;

	*zone_idx = data_offset / max_zone_size;
	uint64_t zone_offset = ((data_offset) - (*zone_idx * max_zone_size) -
		sizeof (struct backend_pool_header) -
		sizeof (struct backend_chunk_header) * MAX_CHUNK);
	ASSERT(zone_offset % CHUNKSIZE == 0);
	*chunk_idx = zone_offset / CHUNKSIZE;

	struct backend_zone *z = &backend->pool->zone[*zone_idx];
	struct backend_chunk_header *c = &z->chunk_header[*chunk_idx];

	return c;
}

/*
 * Recover slot functions are all flushed using a single persist call, and so
 * they have to be implemented in a way that is resistent to store reordering.
 */

/*
 * recover_slot_unknown -- (internal) clear already recovered slot
 *
 * This function will be called on slots that have been already processed but
 * the clearing function was interrupted at the last moment.
 */
static void
recover_slot_unknown(struct backend_persistent *b,
	struct backend_info_slot *slot)
{
	/*
	 * The slot was already being discarded, just get rid of any
	 * potential left-overs and all will be OK.
	 */
	b->pmemset(slot, 0, sizeof (*slot));
}

/*
 * recover_slot_alloc -- (internal) Revert not completed allocation
 */
static void
recover_slot_alloc(struct backend_persistent *b,
	struct backend_info_slot *slot)
{
	struct backend_info_slot_alloc *alloc_slot =
		(struct backend_info_slot_alloc *)slot;

	uint16_t zone_idx;
	uint16_t chunk_idx;
	uint64_t *ptr = (uint64_t *)(alloc_slot->destination_addr
		+ (uint64_t)b->pool);
	if (*ptr != 0) {
		/* XXX allocs smaller than chunksize */
		struct backend_chunk_header *chunk = get_chunk_by_offset(b,
			*ptr, &zone_idx, &chunk_idx);
		clear_chunk_flag(b, chunk, CHUNK_FLAG_USED);
		*ptr = NULL_OFFSET;
		b->persist(ptr, sizeof (*ptr));
	}

	b->pmemset(alloc_slot, 0, sizeof (*alloc_slot));
}

/*
 * recover_slot_realloc -- (internal) Revert not completed reallocation
 */
static void
recover_slot_realloc(struct backend_persistent *b,
	struct backend_info_slot *slot)
{
	struct backend_info_slot_realloc *realloc_slot =
		(struct backend_info_slot_realloc *)slot;

	uint16_t zone_idx;
	uint16_t chunk_idx;
	uint64_t *ptr = (uint64_t *)(realloc_slot->destination_addr +
		(uint64_t)b->pool);
	if (*ptr != 0 && realloc_slot->old_alloc != 0 &&
		*ptr != realloc_slot->old_alloc) {
		struct backend_chunk_header *chunk = get_chunk_by_offset(b,
			*ptr, &zone_idx, &chunk_idx);
		clear_chunk_flag(b, chunk, CHUNK_FLAG_USED);
		*ptr = realloc_slot->old_alloc;
		b->persist(ptr, sizeof (*ptr));
	}

	b->pmemset(realloc_slot, 0, sizeof (*realloc_slot));
}

/*
 * recover_slot_free -- (internal) Revert not completed free
 */
static void
recover_slot_free(struct backend_persistent *b,
	struct backend_info_slot *slot)
{
	struct backend_info_slot_free *free_slot =
		(struct backend_info_slot_free *)slot;

	uint16_t zone_idx;
	uint16_t chunk_idx;
	uint64_t *ptr = (uint64_t *)(free_slot->free_addr + (uint64_t)b->pool);
	if (*ptr != 0) {
		struct backend_chunk_header *chunk = get_chunk_by_offset(b,
			*ptr, &zone_idx, &chunk_idx);
		set_chunk_flag(b, chunk, CHUNK_FLAG_USED);
	}

	b->pmemset(free_slot, 0, sizeof (*free_slot));
}

static void (*recover_slot[MAX_INFO_SLOT_TYPE])(struct backend_persistent *b,
	struct backend_info_slot *slot) = {
	recover_slot_unknown,
	recover_slot_alloc,
	recover_slot_realloc,
	recover_slot_free
};

/*
 * recover_info_slot -- (internal) Choose recovery function based on a slot type
 */
static void
recover_info_slot(struct backend_persistent *b,
	struct backend_info_slot *slot)
{
	ASSERT(slot->type < MAX_INFO_SLOT_TYPE);
	ASSERT(recover_slot[slot->type] != NULL);

	/*
	 * Call the appropriate recover function, but only if the slot isn't
	 * empty.
	 */
	static const char empty_info_slot[INFO_SLOT_DATA_SIZE] = {0};
	if (slot->type != INFO_SLOT_TYPE_UNKNOWN ||
		memcmp(empty_info_slot, slot->data, INFO_SLOT_DATA_SIZE)) {
		recover_slot[slot->type](b, slot);
	}
}

/*
 * can_open_pool -- (internal) Check if the pool can be opened by this build
 */
static bool
can_open_pool(struct backend_persistent *b)
{
	struct backend_pool_header h = b->pool->primary_header;
	if (h.size != b->pool_size) {
		LOG(3, "Trying to open valid pool with mismatched size");
		return false;
	}

	if (h.major != PERSISTENT_BACKEND_MAJOR) {
		LOG(3, "Trying to open pool created with incompatible backend "
			"version");
		return false;
	}

	if (h.chunk_size != CHUNKSIZE) {
		LOG(3, "Trying to open pool with chunksize different than %u. "
			"This is a compile-time constant.", CHUNKSIZE);
		return false;
	}

	if (h.chunks_per_zone != MAX_CHUNK) {
		LOG(3, "Trying to open pool with chunks per zone different "
		"than %lu. This is a compile-time constant.", MAX_CHUNK);
		return false;
	}

	return true;
}

/*
 * get_max_zones -- (internal) calculate the maximum number of zones
 */
static int
get_max_zones(size_t rawsize)
{
	int max_zone = 0;
	uint64_t zone_max_size = sizeof (struct backend_zone) +
		(MAX_CHUNK * CHUNKSIZE);

	while (rawsize > ZONE_MIN_SIZE) {
		max_zone++;
		rawsize -= rawsize < zone_max_size ? rawsize : zone_max_size;
	}

	return max_zone;
}

/*
 * open_pmem_storage -- (internal) Open the actual persistent pool memory region
 */
static bool
open_pmem_storage(struct backend_persistent *b)
{
	ASSERT(b->pool != NULL);
	ASSERT(b->pool_size > ZONE_MIN_SIZE);

	b->max_zone = get_max_zones(b->pool_size);

	bool pool_valid = verify_header(&b->pool->primary_header) ||
		recover_primary_header(b);

	if (pool_valid) {
		/*
		 * The pool is valid but may be incompatible with this
		 * implementation.
		 */
		if (!can_open_pool(b))
			return false;
	} else {
		write_pool_layout(b);
	}

	switch (get_pool_state(b)) {
	case POOL_STATE_CLOSED:
#ifdef DEBUG
		for (int i = 0; i < MAX_INFO_SLOT; ++i) {
			ASSERT(b->pool->info_slot[i].type ==
				INFO_SLOT_TYPE_UNKNOWN);
		}
#endif
		/* all is good */
		set_pool_state(b, POOL_STATE_OPEN);
		return true;
	case POOL_STATE_OPEN:
		/* need to iterate through info slots */
		for (int i = 0; i < MAX_INFO_SLOT; ++i) {
			recover_info_slot(b, &b->pool->info_slot[i]);
		}
		/* copy primary header into all backups, just in case */
		write_backup_pool_headers(b);
		return true;
	default:
		ASSERT(false); /* code unreachable */
	}

	return false;
}

/*
 * close_pmem_storage -- (internal) Close persistent memory pool region
 */
static void
close_pmem_storage(struct backend_persistent *b)
{
	/*
	 * Closing a pool with threads still using it is forbidden,
	 * check this only for debug build.
	 */
#ifdef DEBUG
	for (int i = 0; i < MAX_INFO_SLOT; ++i) {
		ASSERT(b->pool->info_slot[i].type == INFO_SLOT_TYPE_UNKNOWN);
	}
#endif
	ASSERT(get_pool_state(b) == POOL_STATE_OPEN);

	set_pool_state(b, POOL_STATE_CLOSED);
}

/*
 * memcpy_pmem -- (internal) non-temporal version of memcpy
 */
static void *
memcpy_pmem(void *dest, void *src, size_t len)
{
	pmem_memcpy_persist(dest, src, len);
	return dest;
}

/*
 * memset_pmem -- (internal) non-temporal version of memeset
 */
static void *
memset_pmem(void *dest, int c, size_t len)
{
	pmem_memset_persist(dest, c, len);
	return dest;
}

/*
 * memcpy_nopmem -- (internal) regular memcpy followed by an msync
 */
static void *
memcpy_nopmem(void *dest, void *src, size_t len)
{
	memcpy(dest, src, len);
	pmem_msync(dest, len);
	return dest;
}

/*
 * memset_nopmem -- (internal) regular memset followed by an msync
 */
static void *
memset_nopmem(void *dest, int c, size_t len)
{
	memset(dest, c, len);
	pmem_msync(dest, len);
	return dest;
}

/*
 * persistent_backend_open -- opens a persistent backend
 */
struct backend *
backend_persistent_open(void *ptr, size_t size)
{
	struct backend_persistent *backend = Malloc(sizeof (*backend));
	if (backend == NULL) {
		goto error_backend_malloc;
	}

	backend_init(&(backend->super), BACKEND_PERSISTENT,
		&persistent_bucket_ops,
		&persistent_arena_ops,
		&persistent_pool_ops);

	backend->is_pmem = pmem_is_pmem(ptr, size);

	/*
	 * Casting is necessery to silence the compiler about msync returning
	 * an int instead of nothing - the return value is not used anyway.
	 */
	if (backend->is_pmem) {
		backend->persist = (persist_func)pmem_persist;
		backend->pmemcpy = memcpy_pmem;
		backend->pmemset = memset_pmem;
	} else {
		backend->persist = (persist_func)pmem_msync;
		backend->pmemcpy = memcpy_nopmem;
		backend->pmemset = memset_nopmem;
	}

	backend->pool = ptr;
	backend->pool_size = size;
	backend->zones_exhausted = 0;

	if (!open_pmem_storage(backend)) {
		goto error_pool_open;
	}

	return (struct backend *)backend;

error_pool_open:
	Free(backend);
error_backend_malloc:
	return NULL;
}

/*
 * persistent_backend_close -- closes a persistent backend
 */
void
backend_persistent_close(struct backend *backend)
{
	ASSERT(backend->type == BACKEND_PERSISTENT);

	struct backend_persistent *persistent_backend =
		(struct backend_persistent *)backend;

	close_pmem_storage(persistent_backend);
	Free(persistent_backend);
}

/*
 * check_zone -- (internal) calculate zone size index
 */
static uint32_t
get_zone_size_idx(int zone_idx, int max_zone, size_t pool_size)
{
	if (zone_idx < max_zone - 1)
		return MAX_CHUNK;

	size_t zone_raw_size = pool_size -
		zone_idx * sizeof (struct backend_zone);

	zone_raw_size -= sizeof (struct backend_pool_header) +
		sizeof (struct backend_chunk_header) * MAX_CHUNK;

	return zone_raw_size / CHUNKSIZE;
}

/*
 * check_zone -- (internal) check zone consistency
 */
static bool
check_zone(struct backend_pool *pool, int id, uint32_t size_idx)
{
	struct backend_zone *zone = &pool->zone[id];

	/*
	 * Once a single chunk header is invalid, there is really no point in
	 * traversing the rest of the zone because even if traversing one by one
	 * it is impossible to distinguish garbage chunk headers from the
	 * 'correct' ones.
	 */
	int i;
	for (i = 0; i < size_idx; ) {
		struct backend_chunk_header *c = &zone->chunk_header[i];
		if (c->magic != CHUNK_HEADER_MAGIC) {
			if (i == 0) {
				/* This zone was never used */
				return true;
			} else {
				LOG(3, "Zone %d Chunk %d:"
					"Invalid header magic field", id, i);
				return false;
			}
		}
		if (c->type > MAX_CHUNK_TYPE || c->type == CHUNK_TYPE_UNKNOWN) {
			LOG(3, "Zone %d Chunk %d: Invalid type", id, i);
			return false;
		}
		if (c->size_idx > size_idx) {
			LOG(3, "Zone %d Chunk %d: size bigger than the zone",
				id, i);
			return false;
		}
		if (c->size_idx == 0) {
			LOG(3, "Zone %d Chunk %d: nil size", id, i);
			return false;
		}

		i += c->size_idx;
	}

	if (i != size_idx) {
		LOG(3, "Zone %d: Misaligned chunk headers", id);
		return false;
	}

	return true;
}

/*
 * check_slot_unknown -- (internal) check unknown slot consistency
 */
static bool
check_slot_unknown(struct backend_info_slot *slot, int id, size_t pool_size)
{
	/*
	 * This can be garbage filled after recovery interruption and thats
	 * perfectly OK.
	 */
	return true;
}

/*
 * check_slot_alloc -- (internal) check allocation slot consistency
 */
static bool
check_slot_alloc(struct backend_info_slot *slot, int id, size_t pool_size)
{
	struct backend_info_slot_alloc *alloc_slot =
		(struct backend_info_slot_alloc *)slot;
	if (alloc_slot->reserved != 0 || alloc_slot->reserved_e[0] != 0 ||
		alloc_slot->reserved_e[1] != 0) {
		LOG(1, "Info slot %d: reserved region not zeroed", id);
		return false;
	}

	if (alloc_slot->destination_addr > pool_size) {
		LOG(1, "Info slot %d: destination address out of"
			"pool memory region", id);
		return false;
	}

	return true;
}

/*
 * check_slot_realloc -- (internal) check reallocation slot consistency
 */
static bool
check_slot_realloc(struct backend_info_slot *slot, int id, size_t pool_size)
{
	struct backend_info_slot_realloc *realloc_slot =
		(struct backend_info_slot_realloc *)slot;

	if (realloc_slot->reserved != 0 || realloc_slot->reserved_e != 0) {
		LOG(1, "Info slot %d: reserved region not zeroed", id);
		return false;
	}

	if (realloc_slot->destination_addr > pool_size) {
		LOG(1, "Info slot %d: realloc destination address out of"
			"pool memory region", id);
		return false;
	}

	if (realloc_slot->old_alloc > pool_size) {
		LOG(1, "Info slot %d: realloc old address out of"
			"pool memory region", id);
		return false;
	}

	return true;
}

/*
 * check_slot_free -- (internal) check free slot consistency
 */
static bool
check_slot_free(struct backend_info_slot *slot, int id, size_t pool_size)
{
	struct backend_info_slot_free *free_slot =
		(struct backend_info_slot_free *)slot;

	if (free_slot->reserved != 0 || free_slot->reserved_e[0] != 0 ||
		free_slot->reserved_e[1] != 0) {
		LOG(1, "Info slot %d: reserved region not zeroed", id);
		return false;
	}

	if (free_slot->free_addr > pool_size) {
		LOG(1, "Info slot %d: free address out of"
			"pool memory region", id);
		return false;
	}

	return true;
}

static bool (*check_slot[MAX_INFO_SLOT_TYPE])
	(struct backend_info_slot *slot, int id, size_t pool_size) = {
	check_slot_unknown,
	check_slot_alloc,
	check_slot_realloc,
	check_slot_free
};

/*
 * check_info_slot -- (internal) check info slot consistency
 */
static bool
check_info_slot(struct backend_pool *pool, int id, size_t pool_size)
{
	struct backend_info_slot *slot = &pool->info_slot[id];
	if (slot->type >= MAX_INFO_SLOT_TYPE)
		return false;

	return check_slot[slot->type](slot, id, pool_size);
}

/*
 * backend_persistent_consistency_check -- check pool consistency
 */
bool
backend_persistent_consistency_check(void *ptr, size_t size)
{
	struct backend_pool *pool = ptr;
	if (pool == NULL) {
		LOG(3, "Invalid pool memory region");
		return false;
	}

	/*
	 * A single valid header, either primary or one of the backups is enough
	 * to make the pool consistent.
	 */
	bool valid_header = verify_header(&pool->primary_header);

	if (!valid_header) {
		LOG(3, "No valid primary header");
	}

	bool ok = true;

	for (int i = 0; i < MAX_INFO_SLOT; ++i) {
		ok &= check_info_slot(pool, i, size);
	}

	int max_zone = get_max_zones(size);

	for (int i = 0; i < max_zone; ++i) {
		if (!verify_header(&pool->zone[i].backup_header)) {
			LOG(3, "No valid backup %d headers", i);
		} else {
			valid_header = true;
		}
		ok &= check_zone(pool, i, get_zone_size_idx(i, max_zone, size));
	}

	return ok && valid_header;
}

/*
 * persistent_set_alloc_ptr -- persistent implementation of set_alloc_ptr
 */
void
persistent_set_alloc_ptr(struct arena *arena, uint64_t *ptr,
	uint64_t value)
{
	struct backend_persistent *backend =
		(struct backend_persistent *)arena->pool->backend;

	*ptr = value;
	backend->persist(ptr, sizeof (*ptr));
}

/*
 * write_chunk_header -- (internal) write valid chunk header to persistance
 */
static void
write_chunk_header(struct backend_persistent *b,
	struct backend_chunk_header *c, int size)
{
	struct backend_chunk_header hdr = {
		.magic = 0,
		.flags = 0,
		.size_idx = size,
		.type = CHUNK_TYPE_BASE,
		.type_specific = 0
	};
	*c = hdr;
	b->persist(c, sizeof (*c));
	c->magic = CHUNK_HEADER_MAGIC;
	b->persist(&c->magic, sizeof (c->magic));
}

/*
 * set_chunk_size -- (internal) resize a chunk
 */
static bool
set_chunk_size(struct backend_persistent *b, struct backend_chunk_header *c,
	int new_size)
{
	ASSERT(new_size > 0);

	c->size_idx = new_size;
	b->persist(c, sizeof (*c));

	return true;
}

/*
 * add_chunk -- (internal) add chunk to the volatile objects container
 */
static void
add_chunk(struct pmalloc_pool *pool, int zone_idx, int chunk_idx,
	struct backend_chunk_header *c, uint64_t data_offset)
{
	struct bucket_object obj = {
		.size_idx = c->size_idx,
		.unique_id = UID_PACK(chunk_idx, zone_idx),
		.real_size = CHUNKSIZE * c->size_idx,
		.data_offset = data_offset
	};

	int class_id = get_bucket_class_id_by_size(pool, obj.real_size);
	ASSERT(pool->buckets[class_id] != NULL);
	if (!bucket_add_object(pool->buckets[class_id], &obj)) {
		/* There's no point in continuing, volatile OOM */
		LOG(3, "Filling bucket with objects failed!");
		return;
	}
}

/*
 * persistent_fill_buckets -- persistent implementation of fill_buckets
 */
void
persistent_fill_buckets(struct pmalloc_pool *pool)
{
	struct backend_persistent *backend =
		(struct backend_persistent *)pool->backend;

	/* Fill in buckets one zone at a time. */
	int idx = backend->zones_exhausted++;

	struct backend_zone *z = &backend->pool->zone[idx];
	uint32_t zone_size_idx =
		get_zone_size_idx(idx, backend->max_zone, backend->pool_size);

	for (int i = 0; i < zone_size_idx; ) {
		struct backend_chunk_header *c = &z->chunk_header[i];
		if (c->magic != CHUNK_HEADER_MAGIC) {
			ASSERT(i == 0);
			write_chunk_header(backend, c, zone_size_idx);
		}

		if ((c->flags & CHUNK_FLAG_USED) == 0) {
			add_chunk(pool, idx, i, c, (uint64_t)&z->chunk_data[i] -
				(uint64_t)backend->pool);
		}

		i += c->size_idx;
	}
}

/*
 * persistent_bucket_classes --
 *	persistent implementation of create_bucket_classes
 */
void
persistent_bucket_classes(struct pmalloc_pool *pool)
{
	struct bucket_class default_class = {
		.unit_size = CHUNKSIZE
	};

	if (bucket_register_class(pool, default_class) == -1)
		ASSERT(false);
}

/*
 * persistent_init_bucket_obj -- persistent implementation of init_bucket_obj
 */
void
persistent_init_bucket_obj(struct bucket *bucket, struct bucket_object *obj)
{
	struct backend_persistent *backend =
		(struct backend_persistent *)bucket->pool->backend;

	uint16_t chunk_idx = UID_CHUNK_IDX(obj->unique_id);
	uint16_t zone_idx = UID_ZONE_IDX(obj->unique_id);

	ASSERT(zone_idx < backend->max_zone);
	ASSERT(chunk_idx < MAX_CHUNK);

	struct backend_zone *z = &backend->pool->zone[zone_idx];

	struct backend_chunk_header *c = &z->chunk_header[chunk_idx];
	if (obj->size_idx < c->size_idx) {
		uint32_t nsize = c->size_idx - obj->size_idx;
		uint32_t nc_idx = chunk_idx + obj->size_idx;
		struct backend_chunk_header *nc = &z->chunk_header[nc_idx];
		write_chunk_header(backend, nc, nsize);
		if (set_chunk_size(backend, c, obj->size_idx)) {
			add_chunk(bucket->pool, zone_idx, nc_idx, nc,
				(uint64_t)&z->chunk_data[nc_idx] -
				(uint64_t)backend->pool);
		}
	}

	obj->size_idx = c->size_idx;
	obj->real_size = c->size_idx * CHUNKSIZE;
	obj->data_offset = (uint64_t)&z->chunk_data[chunk_idx] -
		(uint64_t)backend->pool;
}

/*
 * persistent_set_bucket_obj_state --
 *	persistent implementation of set_bucket_obj_state
 */
bool
persistent_set_bucket_obj_state(struct bucket *bucket,
	struct bucket_object *obj, enum bucket_obj_state state)
{
	ASSERT(state < MAX_BUCKET_OBJ_STATE);
	uint16_t chunk_idx = UID_CHUNK_IDX(obj->unique_id);
	uint16_t zone_idx = UID_ZONE_IDX(obj->unique_id);
	ASSERT(chunk_idx < MAX_CHUNK);

	struct backend_persistent *backend =
		(struct backend_persistent *)bucket->pool->backend;

	ASSERT(zone_idx < backend->max_zone);

	struct backend_zone *z = &backend->pool->zone[zone_idx];
	struct backend_chunk_header *c = &z->chunk_header[chunk_idx];
	if (state == BUCKET_OBJ_STATE_ALLOCATED) {
		/* XXX proper 'initial content' handling */
		backend->pmemset(&z->chunk_data[chunk_idx], 0, obj->real_size);
		return set_chunk_flag(backend, c, CHUNK_FLAG_USED);
	} else if (state == BUCKET_OBJ_STATE_FREE) {
		return clear_chunk_flag(backend, c, CHUNK_FLAG_USED);
	}

	return false;
}

/*
 * persistent_locate_bucket_obj --
 *	persistent implementation of locate_bucket_obj
 */
bool
persistent_locate_bucket_obj(struct pmalloc_pool *pool,
	struct bucket_object *obj, uint64_t data_offset)
{
	struct backend_persistent *backend =
		(struct backend_persistent *)pool->backend;

	ASSERT(data_offset < backend->pool_size);

	uint16_t zone_idx;
	uint16_t chunk_idx;

	struct backend_chunk_header *c = get_chunk_by_offset(backend,
		data_offset, &zone_idx, &chunk_idx);

	if (c->magic != CHUNK_HEADER_MAGIC)
		return false;

	if ((c->flags & CHUNK_FLAG_USED) == 0)
		return false;

	obj->size_idx = c->size_idx;
	obj->unique_id = UID_PACK(chunk_idx, zone_idx);
	obj->real_size = CHUNKSIZE * c->size_idx;
	obj->data_offset = data_offset;

	return true;
}

/*
 * persistent_get_direct -- persistent implementation of get_direct
 */
void *
persistent_get_direct(struct pmalloc_pool *pool, uint64_t ptr)
{
	struct backend_persistent *backend =
		(struct backend_persistent *)pool->backend;

	ASSERT(ptr < backend->pool_size);
	return (void *)((uint64_t)backend->pool + ptr);
}

/*
 * persistent_copy_content -- persistent implementation of copy_content
 */
void persistent_copy_content(struct pmalloc_pool *pool,
	struct bucket_object *dest, struct bucket_object *src)
{
	ASSERT(dest->size_idx >= src->size_idx);

	struct backend_persistent *backend =
		(struct backend_persistent *)pool->backend;

	void *ddest = (void *)((uint64_t)backend->pool + dest->data_offset);
	void *dsrc = (void *)((uint64_t)backend->pool + src->data_offset);
	backend->pmemcpy(ddest, dsrc, src->real_size);
}

/*
 * set_slot_unknown -- (internal) set unknown info slot for arena
 */
static void
set_slot_unknown(struct backend_info_slot *slot, uint64_t ptr, uint64_t value)
{
	/* unreachable */
	ASSERT(false);
}

/*
 * set_slot_alloc -- (internal) set alloc info slot for arena
 */
static void
set_slot_alloc(struct backend_info_slot *slot, uint64_t ptr, uint64_t value)
{
	struct backend_info_slot_alloc *alloc_slot =
		(struct backend_info_slot_alloc *)slot;

	ASSERT(alloc_slot->destination_addr == 0);
	alloc_slot->destination_addr = ptr;
}

/*
 * set_slot_realloc -- (internal) set realloc info slot for arena
 */
static void
set_slot_realloc(struct backend_info_slot *slot, uint64_t ptr, uint64_t value)
{
	struct backend_info_slot_realloc *realloc_slot =
		(struct backend_info_slot_realloc *)slot;

	ASSERT(realloc_slot->destination_addr == 0);
	ASSERT(realloc_slot->old_alloc == 0);

	/*
	 * A 0 value in any of those two values indicates an incomplete write,
	 * so ordering doesn't really matter.
	 */
	realloc_slot->destination_addr = ptr;
	realloc_slot->old_alloc = value;
}

/*
 * set_slot_free -- (internal) set free info slot for arena
 */
static void
set_slot_free(struct backend_info_slot *slot, uint64_t ptr, uint64_t value)
{
	struct backend_info_slot_free *free_slot =
		(struct backend_info_slot_free *)slot;

	ASSERT(free_slot->free_addr == 0);

	free_slot->free_addr = ptr;
}

static void (*set_slot[MAX_INFO_SLOT_TYPE])
	(struct backend_info_slot *slot, uint64_t ptr, uint64_t value) = {
	set_slot_unknown,
	set_slot_alloc,
	set_slot_realloc,
	set_slot_free
};

/*
 * persistent_set_guard -- persistent implementation of set_guard
 */
void
persistent_set_guard(struct arena *arena, enum guard_type type,
	uint64_t *ptr)
{
	struct backend_persistent *backend =
		(struct backend_persistent *)arena->pool->backend;

	struct backend_info_slot *s = &backend->pool->info_slot[arena->id];

	/* info slot type matches the guard types */
	enum info_slot_type stype = (enum info_slot_type)type;

	ASSERT(s->type == INFO_SLOT_TYPE_UNKNOWN);
	ASSERT(stype < MAX_INFO_SLOT_TYPE);
	s->type = stype;
	uint64_t rptr = (uint64_t)ptr - (uint64_t)backend->pool;
	set_slot[stype](s, rptr, *ptr);

	backend->persist(s, sizeof (*s));
}

/*
 * persistent_clear_guard -- persistent implementation of clear_guard
 */
void
persistent_clear_guard(struct arena *arena)
{
	struct backend_persistent *backend =
		(struct backend_persistent *)arena->pool->backend;
	struct backend_info_slot *s = &backend->pool->info_slot[arena->id];
	backend->pmemset(s, 0, sizeof (*s));
}
