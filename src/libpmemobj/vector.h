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
 * vector.h -- internal definitions for persistent vector module
 */


#define MAX_LISTS 32

struct vector {
	PMEMrwlock lock;
	uint64_t pool_uuid_lo;
	uint64_t next;
	uint64_t size;
	uint64_t entries[MAX_LISTS];
};

struct vector_entry {
	uint64_t pos;
};

#define VECTOR_ENTRY struct vector_entry

int vector_foreach(PMEMobjpool *pop, struct vector *v, void (*callback)(PMEMoid oid));
int vector_remove(PMEMobjpool *pop, struct vector *v, PMEMoid *oid);
int vector_fix(PMEMobjpool *pop, struct vector *v);
int vector_pushback_new(PMEMobjpool *pop, struct vector *v, PMEMoid *oid, size_t size, void (*constructor)(PMEMobjpool *pop, void *ptr, void *arg), void *arg);
int vector_move(PMEMobjpool *pop, struct vector *ov, struct vector *nv, PMEMoid oid);
int vector_is_empty(struct vector *v);
void vector_reinit(PMEMobjpool *pop, struct vector *v);
PMEMoid vector_get(PMEMobjpool *pop, struct vector *v, uint64_t index);
PMEMoid vector_next(PMEMobjpool *pop, struct vector *v, PMEMoid oid);
PMEMoid vector_get_last(PMEMobjpool *pop, struct vector *v);
PMEMoid vector_get_first(PMEMobjpool *pop, struct vector *v);
void vector_new_constructor(PMEMobjpool *pop, void *ptr, void *arg);
void vector_init(PMEMobjpool *pop, struct vector *v);
