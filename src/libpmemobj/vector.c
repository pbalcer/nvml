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
 * vector.c -- implementation of vector module
 */


#include "libpmemobj.h"
#include "vector.h"
#include "lane.h"
#include "redo.h"
#include "libpmem.h"
#include "list.h"
#include "util.h"
#include "obj.h"
#include "pmalloc.h"
#include "out.h"
#include <stdio.h>

#define FIRST_TAB_BIT 3
#define FIRST_TAB_SIZE (1 << FIRST_TAB_BIT)

static int
find_highest_bit(uint64_t value)
{
	return 64 - __builtin_clzll(value) - 1;
}

static void
vector_tab_from_idx(struct vector *v, uint64_t idx, uint64_t *tab, uint64_t *tab_idx)
{
	uint64_t pos = idx + FIRST_TAB_SIZE;
	int hbit = find_highest_bit(pos);
	*tab = hbit - FIRST_TAB_BIT;
	*tab_idx = pos ^ (1 << hbit);
}

static uint64_t *
vector_get_tab(PMEMobjpool *pop, uint64_t tab_off)
{
	return (void *)((char *)pop + tab_off);
}

static uint64_t *
vector_next(PMEMobjpool *pop, struct vector *v, uint64_t *n)
{
	*n = __sync_fetch_and_add(&v->next, 1);
	pmemobj_persist(pop, &v->next, sizeof (v->next));
	uint64_t tab;
	uint64_t tab_idx;
	vector_tab_from_idx(v, *n, &tab, &tab_idx);

	while (v->entries[tab] == 0) {
		if (tab_idx == 0) {
			pmalloc(pop, &v->entries[tab], sizeof (uint64_t) * (1 << (tab + FIRST_TAB_BIT)), 0);
		}
		sched_yield();
	}

	return &vector_get_tab(pop, v->entries[tab])[tab_idx];
}

void
vector_init(PMEMobjpool *pop, struct vector *v)
{
	v->pool_uuid_lo = pop->uuid_lo;
	v->next = 0;
	memset(v->entries, 0, sizeof (*v->entries));

	pmem_msync(v, sizeof (*v));
}

struct vector_new_args {
	uint64_t pos;
	ptrdiff_t entry_offset;
	void *arg;
	void (*constructor)(PMEMobjpool *, void *, void *);
};

void
vector_new_constructor(PMEMobjpool *pop, void *ptr, void *arg)
{
	struct vector_new_args *vec_args = arg;
	struct vector_entry *ventry = (void *)((char *)ptr + vec_args->entry_offset);

	ventry->pos = vec_args->pos;
	pmemobj_persist(pop, &ventry->pos, sizeof (ventry->pos));

	vec_args->constructor(pop, ptr, vec_args->arg);
}


struct vector_lane_section {
	uint64_t where;
	uint64_t what;
	struct redo_log redo[4];
};


int
vector_pushback_new(PMEMobjpool *pop, struct vector *v, PMEMoid *oid, ptrdiff_t entry_offset, size_t size, void (*constructor)(PMEMobjpool *pop, void *ptr, void *arg), void *arg)
{
	pmemobj_rwlock_rdlock(pop, &v->lock);
	uint64_t n;
	uint64_t *dest = vector_next(pop, v, &n);
	int err = 0;
	struct lane_section *lane;
	if ((err = lane_hold(pop, &lane, LANE_SECTION_VECTOR)) != 0)
		return err;

	struct vector_lane_section *sec =
		(struct vector_lane_section *)lane->layout;

	struct vector_new_args vec_args = {
		.pos = n,
		.entry_offset = entry_offset,
		.arg = arg,
		.constructor = constructor
	};

	if (oid != NULL) {
		sec->what = n;
		sec->where = oid->off;
	}
	pmemobj_persist(pop, &sec->where, sizeof (uint64_t) * 2);
	int ret = pmalloc_construct(pop, dest, size, constructor, &vec_args, 0);
	pmemobj_rwlock_unlock(pop, &v->lock);

	if (lane_release(pop) != 0) {
		ERR("Failed to release the lane");
		ASSERT(0);
	}

	return ret;
}

int
vector_pushback(PMEMobjpool *pop, struct vector *v, PMEMoid oid, ptrdiff_t entry_offset)
{
	ASSERTeq(v->pool_uuid_lo, oid.pool_uuid_lo);
	pmemobj_rwlock_rdlock(pop, &v->lock);

	uint64_t n;
	uint64_t *dest = vector_next(pop, v, &n);
	*dest = oid.off;
	pmemobj_persist(pop, dest, sizeof (*dest));

	struct vector_entry *ventry = (void *)((char*)pmemobj_direct(oid) + entry_offset);
	ventry->pos = n;
	pmemobj_persist(pop, &ventry->pos, sizeof (ventry->pos));

	pmemobj_rwlock_unlock(pop, &v->lock);

	return 0;
}

int
vector_fix(PMEMobjpool *pop, struct vector *v, ptrdiff_t entry_offset)
{
	uint64_t tab;
	uint64_t tab_idx;
	PMEMoid oid;
	oid.pool_uuid_lo = v->pool_uuid_lo;
	struct vector_entry *ventry;

	uint64_t i = v->next;
	for (uint64_t i = v->next - 1; i < (v->next + OBJ_NLANES); ++i) {
		vector_tab_from_idx(v, v->next - 1, &tab, &tab_idx);
		oid.off = vector_get_tab(pop, v->entries[tab])[tab_idx];
		if (oid.off == 0)
			break;

		ventry = (void *)((char*)pmemobj_direct(oid) + entry_offset);

		if (ventry->pos == 0) { /* unfinished pushback */
			ventry->pos = i;
			pmemobj_persist(pop, &ventry->pos, sizeof (ventry->pos));
		}

		if (ventry->pos < i) { /* unfinished remove */
			uint64_t ntab;
			uint64_t ntab_idx;
			vector_tab_from_idx(v, ventry->pos, &ntab, &ntab_idx);
			uint64_t *dest = &vector_get_tab(pop, v->entries[tab])[tab_idx];
			*dest = oid.off;
			pmemobj_persist(pop, dest, sizeof (*dest));
		}

		if (ventry->pos > i) /* succesful remove */
			break;
	}

	v->next = i;
	pmemobj_persist(pop, &v->next, sizeof (v->next));

	return 0;
}

int
vector_remove(PMEMobjpool *pop, struct vector *v, PMEMoid oid, ptrdiff_t entry_offset)
{
	pmemobj_rwlock_wrlock(pop, &v->lock);

	struct vector_entry *ventry = (void *)((char*)pmemobj_direct(oid) + entry_offset);

	uint64_t tab;
	uint64_t tab_idx;
	vector_tab_from_idx(v, ventry->pos, &tab, &tab_idx);

	if (ventry->pos == v->next - 1) {
		ventry->pos = 0;
		vector_get_tab(pop, v->entries[tab])[tab_idx] = 0;
	} else {
		uint64_t ntab;
		uint64_t ntab_idx;
		vector_tab_from_idx(v, v->next - 1, &ntab, &ntab_idx);

		PMEMoid r;
		r.off = vector_get_tab(pop, v->entries[ntab])[ntab_idx];
		r.pool_uuid_lo = v->pool_uuid_lo;

		struct vector_entry *rventry = (void *)((char*)pmemobj_direct(oid) + entry_offset);

		rventry->pos = ventry->pos;
		pmemobj_persist(pop, &rventry->pos, sizeof (rventry->pos));

		vector_get_tab(pop, v->entries[tab])[tab_idx] = r.off;
	}

	v->next--;
	pmemobj_persist(pop, &v->next, sizeof (v->next));

	pmemobj_rwlock_unlock(pop, &v->lock);

	return 0;
}

int
vector_foreach(PMEMobjpool *pop, struct vector *v, void (*callback)(PMEMoid oid))
{
	pmemobj_rwlock_rdlock(pop, &v->lock);

	PMEMoid oid;
	oid.pool_uuid_lo = v->pool_uuid_lo;
	for (int i = 0; i < v->next; ++i) {
		uint64_t tab;
		uint64_t tab_idx;
		vector_tab_from_idx(v, i, &tab, &tab_idx);
		oid.off = vector_get_tab(pop, v->entries[tab])[tab_idx];
		if (oid.off != 0)
			callback(oid);
	}

	pmemobj_rwlock_unlock(pop, &v->lock);

	return 0;
}

/*
 * lane_vector_construct -- create vector lane section
 */
static int
lane_vector_construct(PMEMobjpool *pop, struct lane_section *section)
{
	return 0;
}

/*
 * lane_vector_destruct -- destroy vector lane section
 */
static int
lane_vector_destruct(PMEMobjpool *pop, struct lane_section *section)
{
	return 0;
}

/*
 * lane_vector_recovery -- recovery of vector lane section
 */
static int
lane_vector_recovery(PMEMobjpool *pop, struct lane_section_layout *section)
{
	return 0;
}

/*
 * lane_vector_check -- consistency check of vector lane section
 */
static int
lane_vector_check(PMEMobjpool *pop, struct lane_section_layout *section)
{
	LOG(3, "vector lane %p", section);

	return 0;
}

/*
 * lane_vector_init -- initializes vector section
 */
static int
lane_vector_boot(PMEMobjpool *pop)
{
	return heap_boot(pop);
}

struct section_operations vector_ops = {
	.construct = lane_vector_construct,
	.destruct = lane_vector_destruct,
	.recover = lane_vector_recovery,
	.check = lane_vector_check,
	.boot = lane_vector_boot
};

SECTION_PARM(LANE_SECTION_VECTOR, &vector_ops);
