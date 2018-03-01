/*
 * Copyright 2015-2017, Intel Corporation
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

#include "redo.h"
#include "out.h"
#include "util.h"
#include "valgrind_internal.h"

/*
 * Finish flag at the least significant bit
 */
#define REDO_FINISH_FLAG		((uint64_t)1<<0)
#define REDO_OPERATION(op)		(((uint64_t)(op)) << 1)
#define REDO_OPERATION_MASK		((((uint64_t)1 << 2) - 1) << 1)
#define REDO_OPERATION_FROM_FLAG(flag)	((flag) >> 1 & (((1ULL) << 2) - 1))
#define REDO_FLAG_MASK			(~(REDO_FINISH_FLAG |\
					REDO_OPERATION_MASK))

struct redo_ctx {
	void *base;

	struct pmem_ops p_ops;

	redo_check_offset_fn check_offset;
	void *check_offset_ctx;

	unsigned redo_capacity;
};

/*
 * redo_log_config_new -- allocates redo context
 */
struct redo_ctx *
redo_log_config_new(void *base,
		const struct pmem_ops *p_ops,
		redo_check_offset_fn check_offset,
		void *check_offset_ctx,
		unsigned redo_capacity)
{
	struct redo_ctx *cfg = Malloc(sizeof(*cfg));
	if (!cfg) {
		ERR("!can't create redo log config");
		return NULL;
	}

	cfg->base = base;
	cfg->p_ops = *p_ops;
	cfg->check_offset = check_offset;
	cfg->check_offset_ctx = check_offset_ctx;
	cfg->redo_capacity = redo_capacity;

	return cfg;
}

/*
 * redo_log_config_delete -- frees redo context
 */
void
redo_log_config_delete(struct redo_ctx *ctx)
{
	Free(ctx);
}

/*
 * redo_log_nflags -- (internal) get number of finish flags set
 */
size_t
redo_log_nflags(void *base, const struct redo_log *redo, size_t *nentries)
{
	size_t ret = 0;

	size_t i = 0;
	const struct redo_log *r = redo;
	const struct redo_log_entry *e = &r->entries[0];
	size_t entries = 0;
	*nentries = 0;

	while (1) {
		entries += 1;
		if (redo_log_is_last(e)) {
			if (ret == 0)
				*nentries = entries;
			ret++;
		}

		if (i == r->capacity) {
			if (r->next == 0)
				break;

			r = (struct redo_log *)((char *)base + r->next);
			i = 0;
		}
		e = &r->entries[i++];
	}

	LOG(15, "redo %p nentries %p nflags %zu", redo, nentries, ret);

	return ret;
}

void
redo_log_init(const struct redo_ctx *ctx, struct redo_log *redo)
{
	redo->checksum = 0;
	redo->next = 0;
}

int
redo_log_reserve(const struct redo_ctx *ctx, struct redo_log *redo,
	size_t nentries, redo_extend_fn extend)
{
	do {
		nentries -= MIN(nentries, redo->capacity);
		if (nentries != 0 && redo->next == 0) {
			if (extend(ctx->base, &redo->next) != 0)
				return -1;
		}
		redo = (struct redo_log *)((char *)ctx->base + redo->next);
	} while (nentries != 0);

	return 0;
}

void
redo_log_store(const struct redo_ctx *ctx, struct redo_log *dest,
	struct redo_log *src, size_t nentries)
{
	src->capacity = ctx->redo_capacity;
	src->entries[nentries - 1].offset |= REDO_FINISH_FLAG;
	util_checksum(src,
		sizeof(struct redo_log) +
		sizeof(struct redo_log_entry) * nentries,
			&src->checksum, 1, 0);

	src->unused = 0;

	struct redo_log *redo = dest;
	size_t offset = ctx->redo_capacity;
	size_t next_entries = nentries > ctx->redo_capacity ?
		nentries - ctx->redo_capacity : 0;
	while (next_entries > 0) {
		ASSERTne(redo->next, 0);

		redo = (struct redo_log *)((char *)ctx->base + redo->next);
		size_t ncopy = MIN(next_entries, redo->capacity);
		next_entries -= ncopy;

		pmemops_memcpy_persist(&ctx->p_ops, redo->entries,
			src->entries + offset,
			sizeof(struct redo_log_entry) * ncopy);
		offset += ncopy;
	}

	src->next = dest->next;
	pmemops_memcpy_persist(&ctx->p_ops, dest, src,
		sizeof(struct redo_log) +
		sizeof(struct redo_log_entry) * nentries);
}

void
redo_log_entry_create(const void *base,
	struct redo_log_entry *entry, uint64_t *ptr, uint64_t value,
	enum redo_operation_type type)
{
	entry->offset = ((uint64_t)(ptr) - (uint64_t)base);
	entry->offset |= REDO_OPERATION(type);
	entry->value = value;
}


enum redo_operation_type
redo_log_operation(const struct redo_log_entry *entry)
{
	return REDO_OPERATION_FROM_FLAG(entry->offset);
}

/*
 * redo_log_offset -- returns offset
 */
uint64_t
redo_log_offset(const struct redo_log_entry *entry)
{
	return entry->offset & REDO_FLAG_MASK;
}

/*
 * redo_log_is_last -- returns 1/0
 */
int
redo_log_is_last(const struct redo_log_entry *entry)
{
	return entry->offset & REDO_FINISH_FLAG;
}

void
redo_log_entry_apply(void *base, const struct redo_log_entry *e,
	flush_fn flush)
{
	enum redo_operation_type t = redo_log_operation(e);
	uint64_t offset = redo_log_offset(e);

	uint64_t *val = (uint64_t *)((uintptr_t)base + offset);
	VALGRIND_ADD_TO_TX(val, sizeof(*val));
	switch (t) {
		case REDO_OPERATION_AND:
			*val &= e->value;
		break;
		case REDO_OPERATION_OR:
			*val |= e->value;
		break;
		case REDO_OPERATION_SET:
			*val = e->value;
		break;
		default:
			ASSERT(0);
	}
	VALGRIND_REMOVE_FROM_TX(val, sizeof(*val));

	flush(base, val, sizeof(uint64_t));
}

/*
 * redo_log_process -- (internal) process redo log entries
 */
void
redo_log_process(const struct redo_ctx *ctx, struct redo_log *redo,
		size_t nentries)
{
	LOG(15, "redo %p nentries %zu", redo, nentries);

#ifdef DEBUG
	ASSERTeq(redo_log_check(ctx, redo, nentries), 0);
#endif
	const struct pmem_ops *p_ops = &ctx->p_ops;

	size_t i = 0;
	struct redo_log *r = redo;
	struct redo_log_entry *e = &r->entries[0];

	while (!redo_log_is_last(e)) {
		redo_log_entry_apply(ctx->base, e, ctx->p_ops.flush);

		if (i == r->capacity) {
			ASSERTne(r->next, 0);
			r = (struct redo_log *)((char *)ctx->base + r->next);
			i = 0;
		}
		e = &r->entries[i++];
	}

	redo_log_entry_apply(ctx->base, e, ctx->p_ops.flush);

	e->offset = 0;
	pmemops_persist(p_ops, &e->offset, sizeof(e->offset));
}

/*
 * redo_log_recover -- (internal) recovery of redo log
 *
 * The redo_log_recover shall be preceded by redo_log_check call.
 */
void
redo_log_recover(const struct redo_ctx *ctx, struct redo_log *redo,
		size_t nentries)
{
	LOG(15, "redo %p nentries %zu", redo, nentries);
	ASSERTne(ctx, NULL);

	size_t nflags = redo_log_nflags(ctx->base, redo, &nentries);
	ASSERT(nflags < 2);
	if (nentries == 0 || !util_checksum(redo, sizeof(struct redo_log) +
			sizeof(struct redo_log_entry) * nentries,
			&redo->checksum, 0, 0)) {
		return;
	}

	if (nflags == 1)
		redo_log_process(ctx, redo, nentries);
}

/*
 * redo_log_check -- (internal) check consistency of redo log entries
 */
int
redo_log_check(const struct redo_ctx *ctx, struct redo_log *redo,
		size_t nentries)
{
	LOG(15, "redo %p nentries %zu", redo, nentries);
	ASSERTne(ctx, NULL);
	return 0;

	size_t nflags = redo_log_nflags(ctx->base, redo, &nentries);

	if (nflags > 1) {
		LOG(15, "redo %p too many finish flags", redo);
		return -1;
	}

	if (nflags == 1) {
		void *cctx = ctx->check_offset_ctx;

		struct redo_log_entry *e;
		uint64_t offset;

		for (e = &redo->entries[0]; !redo_log_is_last(e); ++e) {
			offset = redo_log_offset(e);
			if (!ctx->check_offset(cctx, offset)) {
				LOG(15, "redo %p invalid offset %" PRIu64,
						e, e->offset);
				return -1;
			}
		}

		offset = redo_log_offset(e);
		if (!ctx->check_offset(cctx, offset)) {
			LOG(15, "redo %p invalid offset %" PRIu64,
			    redo, offset);
			return -1;
		}
	}

	return 0;
}

/*
 * redo_get_pmem_ops -- returns pmem_ops
 */
const struct pmem_ops *
redo_get_pmem_ops(const struct redo_ctx *ctx)
{
	return &ctx->p_ops;
}
