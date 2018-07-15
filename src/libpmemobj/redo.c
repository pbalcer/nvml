/*
 * Copyright 2015-2018, Intel Corporation
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
 *     * Neither the name of the copyright holder nor the names of its
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
 * redo.c -- redo log implementation
 */

#include <inttypes.h>
#include <string.h>

#include "libpmemobj.h"
#include "redo.h"
#include "out.h"
#include "util.h"
#include "valgrind_internal.h"

/*
 * Operation flag at the three least significant bits
 */
#define REDO_OPERATION(op)		((uint64_t)(op))
#define REDO_OPERATION_MASK		((uint64_t)(0b111))
#define REDO_OPERATION_FROM_OFFSET(off)	((off) & REDO_OPERATION_MASK)
#define REDO_OFFSET_MASK		(~(REDO_OPERATION_MASK))

#define CACHELINE_ALIGN(size) ALIGN_UP(size, CACHELINE_SIZE)

typedef int (*redo_entry_cb)(struct redo_log_entry_base *e, void *arg,
	const struct pmem_ops *p_ops);

/*
 * redo_log_next_by_offset -- calculates the next pointer
 */
static struct redo_log *
redo_log_next_by_offset(size_t offset, void *base)
{
	return offset == 0 ? NULL :
		(struct redo_log *)((char *)base + offset);
}

/*
 * redo_log_next -- retrieves the pointer to the next redo log
 */
static struct redo_log *
redo_log_next(struct redo_log *redo, void *base)
{
	return redo_log_next_by_offset(redo->next, base);
}


/*
 * redo_log_operation -- returns the type of entry operation
 */
enum redo_operation_type
redo_log_entry_type(const struct redo_log_entry_base *entry)
{
	return REDO_OPERATION_FROM_OFFSET(entry->offset);
}

/*
 * redo_log_offset -- returns offset
 */
uint64_t
redo_log_entry_offset(const struct redo_log_entry_base *entry)
{
	return entry->offset & REDO_OFFSET_MASK;
}

/*
 * redo_log_nentries -- returns number of entries in the redo log
 */
size_t
redo_log_nentries(struct redo_log *redo)
{
	return redo->nentries;
}

/*
 * redo_log_foreach_entry -- iterates over every existing entry in the redo log
 */
static int
redo_log_foreach_entry(struct redo_log *redo,
	redo_entry_cb cb, void *arg, const struct pmem_ops *p_ops)
{
	struct redo_log_entry_base *e;
	int ret = 0;
	size_t nentries = 0;
	size_t offset = 0;
	void *base = p_ops->base;

	for (struct redo_log *r = redo; r != NULL; r = redo_log_next(r, base)) {
		for (size_t i = 0; i < r->capacity; ++i) {
			if (nentries == redo->nentries)
				return ret;

			nentries++;
			e = (struct redo_log_entry_base *)r->data + offset;

			if ((ret = cb(e, arg, p_ops)) != 0)
				return ret;
		}
	}

	return ret;
}

/*
 * redo_log_capacity -- (internal) returns the total capacity of the redo log
 */
size_t
redo_log_capacity(struct redo_log *redo, size_t redo_base_bytes, void *base)
{
	size_t capacity = redo_base_bytes;

	/* skip the first one, we count it in 'redo_base_bytes' */
	while ((redo = redo_log_next(redo, base)) != NULL) {
		capacity += redo->capacity;
	}

	return capacity;
}

/*
 * redo_log_rebuild_next_vec -- rebuilds the vector of next entries
 */
void
redo_log_rebuild_next_vec(struct redo_log *redo, struct redo_next *next,
	void *base)
{
	do {
		if (redo->next != 0)
			VEC_PUSH_BACK(next, redo->next);
	} while ((redo = redo_log_next(redo, base)) != NULL);
}

/*
 * redo_log_reserve -- (internal) reserves new capacity in the redo log
 */
int
redo_log_reserve(struct redo_log *redo,
	size_t redo_base_capacity, size_t *new_capacity, redo_extend_fn extend,
	struct redo_next *next, void *base)
{
	size_t capacity = redo_base_capacity;

	uint64_t offset;
	VEC_FOREACH(offset, next) {
		redo = redo_log_next_by_offset(offset, base);
		capacity += redo->capacity;
	}

	while (capacity < *new_capacity) {
		if (extend(base, &redo->next) != 0)
			return -1;
		VEC_PUSH_BACK(next, redo->next);
		redo = redo_log_next(redo, base);
		capacity += redo->capacity;
	}
	*new_capacity = capacity;

	return 0;
}

/*
 * redo_log_checksum -- (internal) calculates redo log checksum
 */
static int
redo_log_checksum(struct redo_log *redo, size_t redo_base_bytes, int insert)
{
	return util_checksum(redo, SIZEOF_REDO_LOG(redo_base_bytes),
		&redo->checksum, insert, 0);
}

/*
 * redo_log_store -- (internal) stores the transient src redo log in the
 *	persistent dest redo log
 *
 * The source and destination redo logs must be cacheline aligned.
 */
void
redo_log_store(struct redo_log *dest, struct redo_log *src, size_t nentries,
	size_t redo_base_capacity, struct redo_next *next,
	struct pmem_ops *p_ops)
{
	/*
	 * First, store all entries over the base capacity of the redo log in
	 * the next logs.
	 * Because the checksum is only in the first part, we don't have to
	 * worry about failsafety here.
	 */
	struct redo_log *redo = dest;
	size_t offset = redo_base_capacity;
	size_t dest_ncopy = MIN(nentries, redo_base_capacity);
	size_t next_entries = nentries - dest_ncopy;
	size_t nlog = 0;

	void *base = p_ops->base;

	while (next_entries > 0) {
		redo = redo_log_next_by_offset(VEC_ARR(next)[nlog++], base);
		ASSERTne(redo, NULL);

		size_t ncopy = MIN(next_entries, redo->capacity);
		next_entries -= ncopy;

		pmemops_memcpy(p_ops,
			redo->entries,
			src->entries + offset,
			CACHELINE_ALIGN(sizeof(struct redo_log_entry) * ncopy),
			PMEMOBJ_F_MEM_WC |
			PMEMOBJ_F_MEM_NODRAIN |
			PMEMOBJ_F_RELAXED);
		offset += ncopy;
	}

	if (nlog != 0)
		pmemops_drain(p_ops);

	/*
	 * Then, calculate the checksum and store the first part of the
	 * redo log.
	 */
	src->next = VEC_SIZE(next) == 0 ? 0 : VEC_FRONT(next);
	src->nentries = nentries; /* total number of entries */
	redo_log_checksum(src, dest_ncopy, 1);

	pmemops_memcpy(p_ops, dest, src,
		CACHELINE_ALIGN(SIZEOF_REDO_LOG(dest_ncopy)),
		PMEMOBJ_F_MEM_WC);
}


void
redo_log_entry_val_create(struct redo_log_entry_base *entry, uint64_t *ptr,
	uint64_t value, enum redo_operation_type type, const void *base)
{
	struct redo_log_entry_val *e = (struct redo_log_entry_val *)entry;
	e->base.offset = (uint64_t)(ptr) - (uint64_t)base;
	e->base.offset |= REDO_OPERATION(type);
	e->value = value;
}

void
redo_log_entry_buf_create(struct redo_log_entry_base *entry, uint64_t *ptr,
	uint64_t size, const void *src, enum redo_operation_type type,
	const struct pmem_ops *p_ops)
{
	struct redo_log_entry_buf *e = (struct redo_log_entry_buf *)entry;
	e->base.offset = (uint64_t)(ptr) - (uint64_t)p_ops->base;
	e->base.offset |= REDO_OPERATION(type);
	e->size = size;
	if (p_ops != NULL) {
		pmemops_memcpy(p_ops, e->data, src, size, 0);
	} else {
		memcpy(e->data, src, size);
	}
}

/*
 * redo_log_entry_apply -- applies modifications of a single redo log entry
 */
void
redo_log_entry_apply(const struct redo_log_entry_base *e,
	const struct pmem_ops *p_ops)
{
	enum redo_operation_type t = redo_log_entry_type(e);
	uint64_t offset = redo_log_entry_offset(e);

	size_t dst_size = sizeof(uint64_t);
	uint64_t *dst = (uint64_t *)((uintptr_t)p_ops->base + offset);

	struct redo_log_entry_buf *eb;
	struct redo_log_entry_val *ev;

	switch (t) {
		case REDO_OPERATION_AND:
			ev = (struct redo_log_entry_val *)e;

			VALGRIND_ADD_TO_TX(dst, dst_size);
			*dst &= ev->value;
			pmemops_xflush(p_ops, dst, sizeof(uint64_t),
				PMEMOBJ_F_RELAXED);
		break;
		case REDO_OPERATION_OR:
			ev = (struct redo_log_entry_val *)e;

			VALGRIND_ADD_TO_TX(dst, dst_size);
			*dst |= ev->value;
			pmemops_xflush(p_ops, dst, sizeof(uint64_t),
				PMEMOBJ_F_RELAXED);
		break;
		case REDO_OPERATION_SET:
			ev = (struct redo_log_entry_val *)e;

			VALGRIND_ADD_TO_TX(dst, dst_size);
			*dst = ev->value;
			pmemops_xflush(p_ops, dst, sizeof(uint64_t),
				PMEMOBJ_F_RELAXED);
		break;
		case REDO_OPERATION_BUF_SET:
			eb = (struct redo_log_entry_buf *)e;

			dst_size = eb->size;
			VALGRIND_ADD_TO_TX(dst, dst_size);
			pmemops_memset(p_ops, dst, *eb->data, eb->size,
				PMEMOBJ_F_RELAXED | PMEMOBJ_F_MEM_NODRAIN);
		break;
		case REDO_OPERATION_BUF_CPY:
			eb = (struct redo_log_entry_buf *)e;

			dst_size = eb->size;
			VALGRIND_ADD_TO_TX(dst, dst_size);
			pmemops_memcpy(p_ops, dst, eb->data, eb->size,
				PMEMOBJ_F_RELAXED | PMEMOBJ_F_MEM_NODRAIN);
		break;
		default:
			ASSERT(0);
	}
	VALGRIND_REMOVE_FROM_TX(dst, dst_size);
}

/*
 * redo_log_process_entry -- processes a single redo log entry
 */
static int
redo_log_process_entry(struct redo_log_entry_base *e, void *arg,
	const struct pmem_ops *p_ops)
{
	redo_log_entry_apply(e, p_ops);

	return 0;
}

/*
 * redo_log_clobber -- zeroes the metadata of the redo log
 */
void
redo_log_clobber(struct redo_log *dest, struct redo_next *next,
	const struct pmem_ops *p_ops)
{
	static struct redo_log empty;

	if (next != NULL)
		empty.next = VEC_SIZE(next) == 0 ? 0 : VEC_FRONT(next);
	else
		empty.next = dest->next;

	pmemops_memcpy(p_ops, dest, &empty, sizeof(empty),
		PMEMOBJ_F_MEM_WC);
}

/*
 * redo_log_process -- (internal) process redo log entries
 */
void
redo_log_process(struct redo_log *redo, const struct pmem_ops *p_ops)
{
	LOG(15, "redo %p", redo);

#ifdef DEBUG
	ASSERTeq(redo_log_check(redo, p_ops), 0);
#endif
	redo_log_foreach_entry(redo, redo_log_process_entry, NULL, p_ops);
}

/*
 * redo_log_recover -- (internal) recovery of redo log
 *
 * The redo_log_recover shall be preceded by redo_log_check call.
 */
void
redo_log_recover(struct redo_log *redo, const struct pmem_ops *p_ops)
{
	LOG(15, "redo %p", redo);

	size_t nentries = MIN(redo->nentries, redo->capacity);
	if (nentries != 0 && redo_log_checksum(redo, nentries, 0)) {
		redo_log_process(redo, p_ops);
		redo_log_clobber(redo, NULL, p_ops);
	}
}

/*
 * redo_log_check_entry -- checks consistency of a single redo log entry
 */
static int
redo_log_check_entry(struct redo_log_entry_base *e,
	void *arg, const struct pmem_ops *p_ops)
{
	uint64_t offset = redo_log_entry_offset(e);
	/*
	if (!ctx->check_offset(ctx->check_offset_ctx, offset)) {
		LOG(15, "redo %p invalid offset %" PRIu64,
				e, e->offset);
		return -1;
	}*/
	return offset == 0 ? -1 : 0;
}

/*
 * redo_log_check -- (internal) check consistency of redo log entries
 */
int
redo_log_check(struct redo_log *redo, const struct pmem_ops *p_ops)
{
	LOG(15, "redo %p", redo);

	if (redo->nentries != 0)
		return redo_log_foreach_entry(redo,
			redo_log_check_entry, NULL, p_ops);

	return 0;
}
