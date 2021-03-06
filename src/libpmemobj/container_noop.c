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
 * container_noop.c -- implementation of noop container
 *
 * The noop interface implementation is used for testing and as a base
 * for new implementations.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "container.h"
#include "container_noop.h"
#include "util.h"
#include "out.h"

bool
noop_add(struct container *c, uint64_t key, val_t value)
{
	return false;
}

val_t
noop_get_rm_eq(struct container *c, uint64_t key)
{
	return NULL_VAL;
}

val_t
noop_get_rm_ge(struct container *c, uint64_t key)
{
	return NULL_VAL;
}

struct container_operations container_noop_ops = {
	.add = noop_add,
	.get_rm_eq = noop_get_rm_eq,
	.get_rm_ge = noop_get_rm_ge
};

struct container *
container_noop_new()
{
	struct container_noop *container = Malloc(sizeof (*container));
	if (container == NULL) {
		goto error_container_malloc;
	}

	container_init(&container->super, CONTAINER_NOOP, &container_noop_ops);

	return (struct container *)container;
error_container_malloc:
	return NULL;
}

void
container_noop_delete(struct container *container)
{
	Free(container);
}
