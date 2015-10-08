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
vector_next_entry(PMEMobjpool *pop, struct vector *v, uint64_t *n)
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
vector_reinit(PMEMobjpool *pop, struct vector *v)
{
	v->next = 0;
	v->size = 0;
	pmemobj_persist(pop, &v->next, sizeof (uint64_t) * 2);
}

void
vector_init(PMEMobjpool *pop, struct vector *v)
{
	v->pool_uuid_lo = pop->uuid_lo;
	v->next = 0;
	v->size = 0;
	memset(v->entries, 0, sizeof (*v->entries));

	pmem_msync(v, sizeof (*v));
}

struct vector_new_args {
	uint64_t pos;
	void *arg;
	void (*constructor)(PMEMobjpool *, void *, void *);
};

void
vector_new_constructor(PMEMobjpool *pop, void *ptr, void *arg)
{
	struct vector_new_args *vec_args = arg;
	struct vector_entry *ventry = ptr;

	ventry->pos = vec_args->pos;
	pmemobj_persist(pop, &ventry->pos, sizeof (ventry->pos));

	vec_args->constructor(pop, (char*)ptr + sizeof (struct oob_header), vec_args->arg);
}

struct vector_lane_section {
	uint64_t remove;
	uint64_t alloc;
	uint64_t alloc_dest;
	uint64_t move_what;
	uint64_t move_where;
	struct redo_log redo[4];
};

int
vector_pushback_new(PMEMobjpool *pop, struct vector *v, PMEMoid *oid, size_t size, void (*constructor)(PMEMobjpool *pop, void *ptr, void *arg), void *arg)
{
	pmemobj_rwlock_rdlock(pop, &v->lock);

	uint64_t n;
	uint64_t *dest = vector_next_entry(pop, v, &n);
	int err = 0;
	struct lane_section *lane;
	if ((err = lane_hold(pop, &lane, LANE_SECTION_VECTOR)) != 0)
		return err;

	v->size++;

	struct vector_lane_section *sec =
		(struct vector_lane_section *)lane->layout;

	struct vector_new_args vec_args = {
		.pos = n,
		.arg = arg,
		.constructor = constructor
	};

	if (OBJ_PTR_IS_VALID(pop, oid)) {
		sec->alloc_dest = (uint64_t)oid - (uint64_t)pop;
		pmemobj_persist(pop, &sec->alloc_dest, sizeof (sec->alloc_dest));
	}

	int ret = pmalloc_construct(pop, &sec->alloc, size +
		sizeof (struct oob_header),
		vector_new_constructor, &vec_args, 0);

	if (ret != 0)
		goto out;

	*dest = sec->alloc;
	pmemobj_persist(pop, dest, sizeof (*dest));

	if (oid != NULL) {
		oid->off = sec->alloc + sizeof (struct oob_header);
		oid->pool_uuid_lo = v->pool_uuid_lo;
		if (OBJ_PTR_IS_VALID(pop, oid))
			pmemobj_persist(pop, oid, sizeof (*oid));
	}
out:
	pmemobj_rwlock_unlock(pop, &v->lock);

	if (lane_release(pop) != 0) {
		ERR("Failed to release the lane");
		ASSERT(0);
	}

	return ret;
}

int
vector_is_empty(struct vector *v)
{
	return v->size == 0;
}

int
vector_fix(PMEMobjpool *pop, struct vector *v)
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

		ventry = pmemobj_direct(oid);

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
vector_remove(PMEMobjpool *pop, struct vector *v, PMEMoid *oid)
{
	pmemobj_rwlock_rdlock(pop, &v->lock);

	PMEMoid real = *oid;
	real.off -= sizeof (struct oob_header);

	struct vector_entry *entry = pmemobj_direct(real);

	int err = 0;
	struct lane_section *lane;
	if ((err = lane_hold(pop, &lane, LANE_SECTION_VECTOR)) != 0)
		return err;

	struct vector_lane_section *sec =
		(struct vector_lane_section *)lane->layout;

	sec->remove = real.off;
	pmemobj_persist(pop, &sec->remove, sizeof (uint64_t));

	uint64_t tab;
	uint64_t tab_idx;
	vector_tab_from_idx(v, entry->pos, &tab, &tab_idx);

	entry->pos = 0;
	pmemobj_persist(pop, &entry->pos, sizeof (uint64_t));

	uint64_t *src = &vector_get_tab(pop, v->entries[tab])[tab_idx];
	*src = 0;
	pmemobj_persist(pop, src, sizeof (*src));

	pfree(pop, &sec->remove, 0);

	oid->off = 0;
	if (OBJ_PTR_IS_VALID(pop, oid))
		pmemobj_persist(pop, oid, sizeof (*oid));

	v->size--;

	pmemobj_rwlock_unlock(pop, &v->lock);

	if (lane_release(pop) != 0) {
		ERR("Failed to release the lane");
		ASSERT(0);
	}

	return 0;
}

int
vector_move(PMEMobjpool *pop, struct vector *ov, struct vector *nv, PMEMoid oid)
{
	pmemobj_rwlock_rdlock(pop, &ov->lock);
	pmemobj_rwlock_rdlock(pop, &nv->lock);

	PMEMoid real = oid;
	real.off -= sizeof (struct oob_header);

	struct vector_entry *entry = pmemobj_direct(real);

	int err = 0;
	struct lane_section *lane;
	if ((err = lane_hold(pop, &lane, LANE_SECTION_VECTOR)) != 0)
		return err;
	struct vector_lane_section *sec =
		(struct vector_lane_section *)lane->layout;

	sec->move_what = oid.off;
	sec->move_where = (uint64_t)nv - (uint64_t)pop;
	pmemobj_persist(pop, &sec->move_what,
		sizeof (sec->move_what) + sizeof (sec->move_where));

	uint64_t n;
	uint64_t *dest = vector_next_entry(pop, nv, &n);

	*dest = real.off;
	pmemobj_persist(pop, dest, sizeof (*dest));

	uint64_t tab;
	uint64_t tab_idx;
	vector_tab_from_idx(ov, entry->pos, &tab, &tab_idx);
	uint64_t *src = &vector_get_tab(pop, ov->entries[tab])[tab_idx];
	*src = 0;
	pmemobj_persist(pop, src, sizeof (*src));

	entry->pos = n;
	pmemobj_persist(pop, &entry->pos, sizeof (entry->pos));

	sec->move_what = 0;
	sec->move_where = 0;
	pmemobj_persist(pop, &sec->move_what,
		sizeof (sec->move_what) + sizeof (sec->move_where));

	nv->size++;
	ov->size--;

	pmemobj_rwlock_unlock(pop, &nv->lock);
	pmemobj_rwlock_unlock(pop, &ov->lock);

	if (lane_release(pop) != 0) {
		ERR("Failed to release the lane");
		ASSERT(0);
	}
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
		oid.off = vector_get_tab(pop, v->entries[tab])[tab_idx] + sizeof (struct oob_header);
		if (oid.off != 0)
			callback(oid);
	}

	pmemobj_rwlock_unlock(pop, &v->lock);

	return 0;
}

static PMEMoid
vector_get_(PMEMobjpool *pop, struct vector *v, uint64_t index, int dir)
{
	if (index >= v->next)
		return OID_NULL;

	uint64_t tab;
	uint64_t tab_idx;
	vector_tab_from_idx(v, index, &tab, &tab_idx);
	if (v->entries[tab] == 0)
		return OID_NULL;

	PMEMoid oid;
	oid.off = vector_get_tab(pop, v->entries[tab])[tab_idx];
	if (oid.off == 0) {
		if (dir == 0 || index + dir >= v->next || (index == 0 && dir == -1))
			return OID_NULL;

		return vector_get_(pop, v, index + dir, dir);
	}

	oid.off += sizeof (struct oob_header);

	oid.pool_uuid_lo = v->pool_uuid_lo;

	return oid;
}

PMEMoid
vector_get(PMEMobjpool *pop, struct vector *v, uint64_t index)
{
	return vector_get_(pop, v, index, 0);
}

PMEMoid vector_next(PMEMobjpool *pop, struct vector *v, PMEMoid oid)
{
	struct vector_entry *entry = (void *)((char*)pmemobj_direct(oid) - sizeof (struct oob_header));

	return vector_get_(pop, v, entry->pos + 1, 1);
}

PMEMoid vector_get_last(PMEMobjpool *pop, struct vector *v)
{
	return vector_get_(pop, v, v->next - 1, -1);
}

PMEMoid
vector_get_first(PMEMobjpool *pop, struct vector *v)
{
	return vector_get_(pop, v, 0, 1);
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
	/* XXX */
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
