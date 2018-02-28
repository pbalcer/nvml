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
 * redo.h -- redo log public interface
 */

#ifndef LIBPMEMOBJ_REDO_H
#define LIBPMEMOBJ_REDO_H 1

#include <stddef.h>
#include <stdint.h>

#include "pmemops.h"

struct redo_ctx;

/*
 * redo_log -- redo log entry
 */
struct redo_log_entry {
	uint64_t offset;	/* offset with finish flag */
	uint64_t value;
};

#define REDO_LOG(nentries) {\
	uint64_t checksum;\
	uint64_t next;\
	uint64_t capacity;\
	uint64_t unused;\
	struct redo_log_entry entries[nentries];\
}\

struct redo_log REDO_LOG();

enum redo_operation_type {
	REDO_OPERATION_SET,
	REDO_OPERATION_AND,
	REDO_OPERATION_OR,

	MAX_OPERATION_TYPE
};

typedef int (*redo_check_offset_fn)(void *ctx, uint64_t offset);
typedef int (*redo_extend_fn)(void *, uint64_t *);

struct redo_ctx *redo_log_config_new(void *base,
		const struct pmem_ops *p_ops,
		redo_check_offset_fn check_offset,
		void *check_offset_ctx,
		unsigned redo_capacity);

void redo_log_config_delete(struct redo_ctx *ctx);

int redo_log_reserve(const struct redo_ctx *ctx, struct redo_log *redo,
	size_t nentries, redo_extend_fn extend);
void redo_log_init(const struct redo_ctx *ctx, struct redo_log *redo);
void redo_log_store(const struct redo_ctx *ctx, struct redo_log *dest,
	struct redo_log *src, size_t nentries);
int redo_log_is_last(const struct redo_log_entry *entry);
size_t redo_log_nflags(void *base,
	const struct redo_log *redo, size_t nentries);
uint64_t redo_log_offset(const struct redo_log_entry *entry);
enum redo_operation_type redo_log_operation(const struct redo_log_entry *entry);

void redo_log_entry_create(const void *base,
	struct redo_log_entry *entry, uint64_t *ptr, uint64_t value,
	enum redo_operation_type type);

enum redo_apply_persistence {
	REDO_PERSISTENCE_FLUSH,
	REDO_PERSISTENCE_CLEAN,

	MAX_REDO_PERSISTENCE
};

void redo_log_entry_apply(void *base, const struct redo_log_entry *e,
	flush_fn flush);

void redo_log_process(const struct redo_ctx *ctx, struct redo_log *redo,
		size_t nentries);
void redo_log_recover(const struct redo_ctx *ctx, struct redo_log *redo,
		size_t nentries);
int redo_log_check(const struct redo_ctx *ctx, struct redo_log *redo,
		size_t nentries);

const struct pmem_ops *redo_get_pmem_ops(const struct redo_ctx *ctx);

#endif
