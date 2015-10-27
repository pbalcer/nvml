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
 * pmalloc.c -- persistent malloc implementation
 */

#include <errno.h>

#include "libpmemobj.h"
#include "util.h"
#include "pmalloc.h"
#include "lane.h"
#include "redo.h"
#include "list.h"
#include "obj.h"
#include "out.h"
#include "heap.h"
#include "bucket.h"
#include "heap_layout.h"
#include "out.h"
#include "valgrind_internal.h"

enum alloc_op_redo {
	ALLOC_OP_REDO_PTR_OFFSET,
	ALLOC_OP_REDO_HEADER,

	MAX_ALLOC_OP_REDO
};

/*
 * alloc_write_header -- (internal) creates allocation header
 */
static void
alloc_write_header(PMEMobjpool *pop, struct allocation_header *alloc,
	uint32_t chunk_id, uint32_t zone_id, uint64_t size)
{
	VALGRIND_ADD_TO_TX(alloc, sizeof (*alloc));
	alloc->chunk_id = chunk_id;
	alloc->size = size;
	alloc->zone_id = zone_id;
	VALGRIND_REMOVE_FROM_TX(alloc, sizeof (*alloc));
	pop->persist(pop, alloc, sizeof (*alloc));
}

/*
 * alloc_get_header -- (internal) calculates the address of allocation header
 */
static struct allocation_header *
alloc_get_header(PMEMobjpool *pop, uint64_t off)
{
	struct allocation_header *alloc = (struct allocation_header *)
		((uint64_t)pmalloc_header(pop, off) - sizeof (*alloc));

	return alloc;
}

/*
 * pop_offset -- (internal) calculates offset of ptr in the pool
 */
static uint64_t
pop_offset(PMEMobjpool *pop, void *ptr)
{
	return (uint64_t)ptr - (uint64_t)pop;
}

/*
 * persist_alloc -- (internal) performs a persistent allocation of the
 *	memory block previously reserved by volatile bucket
 */
static int
persist_alloc(PMEMobjpool *pop, struct lane_section *lane,
	struct memory_block m, uint64_t real_size, uint64_t *off,
	void (*constructor)(PMEMobjpool *pop, void *ptr, void *arg),
	void *arg)
{
	int err;

#ifdef DEBUG
	if (heap_block_is_allocated(pop, m)) {
		ERR("heap corruption");
		ASSERT(0);
	}
#endif /* DEBUG */

	uint64_t op_result = 0;

	void *block_data = heap_get_block_data(pop, m);

	ASSERT((uint64_t)block_data % _POBJ_CL_ALIGNMENT == 0);

	/* mark everything (including headers) as accessible */
	VALGRIND_DO_MAKE_MEM_UNDEFINED(pop, block_data, real_size);
	/* mark space as allocated */
	VALGRIND_DO_MEMPOOL_ALLOC(pop, block_data, real_size);

	void *block_hdr = heap_get_block_header(pop, m);
	alloc_write_header(pop, block_hdr, m.chunk_id, m.zone_id, real_size);

	if (constructor != NULL)
		constructor(pop, block_data, arg);

	if ((err = heap_lock_if_run(pop, m)) != 0) {
		VALGRIND_DO_MEMPOOL_FREE(pop, block_data);
		return err;
	}

	void *hdr = heap_prep_meta_header(pop, m, HEAP_OP_ALLOC, &op_result);

	struct allocator_lane_section *sec =
		(struct allocator_lane_section *)lane->layout;

	redo_log_store(pop, sec->redo, ALLOC_OP_REDO_PTR_OFFSET,
		pop_offset(pop, off), pop_offset(pop, block_data));
	redo_log_store_last(pop, sec->redo, ALLOC_OP_REDO_HEADER,
		pop_offset(pop, hdr), op_result);

	redo_log_process(pop, sec->redo, MAX_ALLOC_OP_REDO);

	if (heap_unlock_if_run(pop, m) != 0) {
		ERR("Failed to release run lock");
		ASSERT(0);
	}

	return err;
}

/*
 * pmalloc -- allocates a new block of memory
 *
 * The pool offset is written persistently into the off variable.
 *
 * If successful function returns zero. Otherwise an error number is returned.
 */
int
pmalloc(PMEMobjpool *pop, uint64_t *off, size_t size)
{
	return pmalloc_construct(pop, off, size, NULL, NULL);
}

/*
 * pmalloc_construct -- allocates a new block of memory with a constructor
 *
 * The block offset is written persistently into the off variable, but only
 * after the constructor function has been called.
 *
 * If successful function returns zero. Otherwise an error number is returned.
 */
int
pmalloc_construct(PMEMobjpool *pop, uint64_t *off, size_t size,
	void (*constructor)(PMEMobjpool *pop, void *ptr, void *arg),
	void *arg)
{
	int err = 0;

	struct lane_section *lane;
	if ((err = lane_hold(pop, &lane, LANE_SECTION_ALLOCATOR)) != 0)
		return err;

	struct bucket *b = heap_get_best_bucket(pop, size);

	struct memory_block m = {0, 0, 0, 0};

	m.size_idx = b->calc_units(b, size);

	err = heap_get_bestfit_block(pop, b, &m);

	if (err == ENOMEM && b->type == BUCKET_HUGE)
		goto out; /* there's only one huge bucket */

	if (err == ENOMEM) {
		/*
		 * There's no more available memory in the common heap and in
		 * this lane cache, fallback to the auxiliary (shared) bucket.
		 */
		b = heap_get_auxiliary_bucket(pop, size);
		err = heap_get_bestfit_block(pop, b, &m);
	}

	if (err == ENOMEM) {
		/*
		 * The auxiliary bucket cannot satisfy our request, borrow
		 * memory from other caches.
		 */
		heap_drain_to_auxiliary(pop, b, m.size_idx);
		err = heap_get_bestfit_block(pop, b, &m);
	}

	if (err == ENOMEM) {
		/* we are completely out of memory */
		goto out;
	}

	/*
	 * Now that the memory is reserved we can go ahead with making the
	 * allocation persistent.
	 */
	uint64_t real_size = b->unit_size * m.size_idx;
	err = persist_alloc(pop, lane, m, real_size, off,
		constructor, arg);

out:
	if (lane_release(pop) != 0) {
		ERR("Failed to release the lane");
		ASSERT(0);
	}

	return err;
}

/*
 * prealloc -- resizes in-place a previously allocated memory block
 *
 * The block offset is written persistently into the off variable.
 *
 * If successful function returns zero. Otherwise an error number is returned.
 */
int
prealloc(PMEMobjpool *pop, uint64_t *off, size_t size)
{
	return prealloc_construct(pop, off, size, NULL, NULL);
}

/*
 * prealloc_construct -- resizes an existing memory block with a constructor
 *
 * The block offset is written persistently into the off variable, but only
 * after the constructor function has been called.
 *
 * If successful function returns zero. Otherwise an error number is returned.
 */
int
prealloc_construct(PMEMobjpool *pop, uint64_t *off, size_t size,
	void (*constructor)(PMEMobjpool *pop, void *ptr, void *arg), void *arg)
{
	if (size <= pmalloc_usable_size(pop, *off))
		return 0;

	int err = 0;

	struct lane_section *lane;
	if ((err = lane_hold(pop, &lane, LANE_SECTION_ALLOCATOR)) != 0)
		return err;

	struct memory_block cn = heap_get_block_from_offset(pop, *off);

	struct bucket *b = heap_get_chunk_bucket(pop, cn.zone_id, cn.chunk_id);
	struct allocation_header *alloc = heap_get_block_header(pop, cn);
	cn.size_idx = b->calc_units(b, alloc->size);

	uint32_t add_size_idx = b->calc_units(b, size - alloc->size);
	uint32_t new_size_idx = b->calc_units(b, size);
	uint64_t real_size = new_size_idx * b->unit_size;

	if ((err = heap_lock_if_run(pop, cn)) != 0)
		goto out_lane;

	struct memory_block next = {0};
	if ((err = heap_get_adjacent_free_block(pop, &next, cn, 0)) != 0)
		goto out;

	if (next.size_idx < add_size_idx) {
		err = ENOMEM;
		goto out;
	}

	if ((err = heap_get_exact_block(pop, b, &next,
		add_size_idx)) != 0)
		goto out;

	struct memory_block *blocks[2] = {&cn, &next};
	uint64_t op_result;
	void *hdr;
	struct memory_block m =
		heap_coalesce(pop, blocks, 2, HEAP_OP_ALLOC, &hdr, &op_result);

	void *block_data = heap_get_block_data(pop, m);

	/* mark new part as accessible and undefined */
	VALGRIND_DO_MAKE_MEM_UNDEFINED(pop, (char *)block_data + alloc->size,
			real_size - alloc->size);

	/* resize allocated space */
	VALGRIND_DO_MEMPOOL_CHANGE(pop, block_data, block_data,
		real_size);

	if (constructor != NULL)
		constructor(pop, block_data, arg);

	struct allocator_lane_section *sec =
		(struct allocator_lane_section *)lane->layout;

	redo_log_store(pop, sec->redo, ALLOC_OP_REDO_PTR_OFFSET,
		pop_offset(pop, &alloc->size), real_size);
	redo_log_store_last(pop, sec->redo, ALLOC_OP_REDO_HEADER,
		pop_offset(pop, hdr), op_result);

	redo_log_process(pop, sec->redo, MAX_ALLOC_OP_REDO);

out:
	if (heap_unlock_if_run(pop, cn) != 0) {
		ERR("Failed to release run lock");
		ASSERT(0);
	}

out_lane:
	if (lane_release(pop) != 0) {
		ERR("Failed to release the lane");
		ASSERT(0);
	}

	return err;
}

/*
 * pmalloc_usable_size -- returns the number of bytes in the memory block
 */
size_t
pmalloc_usable_size(PMEMobjpool *pop, uint64_t off)
{
	return alloc_get_header(pop, off)->size;
}

/*
 * pfree -- deallocates a memory block previously allocated by pmalloc
 *
 * A zero value is written persistently into the off variable.
 *
 * If successful function returns zero. Otherwise an error number is returned.
 */
int
pfree(PMEMobjpool *pop, uint64_t *off)
{
	int err = 0;

	struct lane_section *lane;
	if ((err = lane_hold(pop, &lane, LANE_SECTION_ALLOCATOR)) != 0)
		return err;

	struct memory_block m = heap_get_block_from_offset(pop, *off);

	struct bucket *b = heap_get_chunk_bucket(pop, m.zone_id, m.chunk_id);
	struct allocation_header *alloc = heap_get_block_header(pop, m);

	m.size_idx = b->calc_units(b, alloc->size);

#ifdef DEBUG
	if (!heap_block_is_allocated(pop, m)) {
		ERR("Double free or heap corruption");
		ASSERT(0);
	}
#endif /* DEBUG */

	if ((err = heap_lock_if_run(pop, m)) != 0)
		goto out;

	VALGRIND_DO_MEMPOOL_FREE(pop, heap_get_block_data(pop, m));

	uint64_t op_result;
	void *hdr;
	struct memory_block res = heap_free_block(pop, b, m, &hdr, &op_result);

	struct allocator_lane_section *sec =
		(struct allocator_lane_section *)lane->layout;

	redo_log_store(pop, sec->redo, ALLOC_OP_REDO_PTR_OFFSET,
		pop_offset(pop, off), 0);
	redo_log_store_last(pop, sec->redo, ALLOC_OP_REDO_HEADER,
		pop_offset(pop, hdr), op_result);

	redo_log_process(pop, sec->redo, MAX_ALLOC_OP_REDO);

	/*
	 * There's no point in rolling back redo log changes because the
	 * volatile errors don't break the persistent state.
	 */
	if (CNT_OP(b, insert, pop, res) != 0) {
		ERR("Failed to update the heap volatile state");
		ASSERT(0);
	}

	if (heap_unlock_if_run(pop, m) != 0) {
		ERR("Failed to release run lock");
		ASSERT(0);
	}

	if (b->type == BUCKET_RUN &&
		heap_degrade_run_if_empty(pop, b, res) != 0) {
		ERR("Failed to degrade run");
		ASSERT(0);
	}

out:
	if (lane_release(pop) != 0) {
		ERR("Failed to release the lane");
		ASSERT(0);
	}

	return err;
}

void *
pmalloc_header(PMEMobjpool *pop, uint64_t off)
{
	struct memory_block m = heap_get_block_from_offset(pop, off);

	return (char *)heap_get_block_header(pop, m) +
		sizeof (struct allocation_header);
}

/*
 * lane_allocator_construct -- create allocator lane section
 */
static int
lane_allocator_construct(PMEMobjpool *pop, struct lane_section *section)
{
	return 0;
}

/*
 * lane_allocator_destruct -- destroy allocator lane section
 */
static int
lane_allocator_destruct(PMEMobjpool *pop, struct lane_section *section)
{
	return 0;
}

/*
 * lane_allocator_recovery -- recovery of allocator lane section
 */
static int
lane_allocator_recovery(PMEMobjpool *pop, struct lane_section_layout *section)
{
	struct allocator_lane_section *sec =
		(struct allocator_lane_section *)section;

	redo_log_recover(pop, sec->redo, MAX_ALLOC_OP_REDO);

	return 0;
}

/*
 * lane_allocator_check -- consistency check of allocator lane section
 */
static int
lane_allocator_check(PMEMobjpool *pop, struct lane_section_layout *section)
{
	LOG(3, "allocator lane %p", section);

	struct allocator_lane_section *sec =
		(struct allocator_lane_section *)section;

	int ret;
	if ((ret = redo_log_check(pop, sec->redo, MAX_ALLOC_OP_REDO)) != 0)
		ERR("allocator lane: redo log check failed");

	return ret;
}

/*
 * lane_allocator_init -- initializes allocator section
 */
static int
lane_allocator_boot(PMEMobjpool *pop)
{
	return heap_boot(pop);
}

struct section_operations allocator_ops = {
	.construct = lane_allocator_construct,
	.destruct = lane_allocator_destruct,
	.recover = lane_allocator_recovery,
	.check = lane_allocator_check,
	.boot = lane_allocator_boot
};

SECTION_PARM(LANE_SECTION_ALLOCATOR, &allocator_ops);
