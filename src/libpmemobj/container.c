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
 * container.c -- implementation of container interface
 *
 * Container is the structure on which the entire performance of the allocator
 * relies on - the faster the implementation is the better the overal speed of
 * frontend interface functions.
 * This isn't a fully blown universal collection API but rather a specifically
 * tailored to fit one purpose - to store and find chunks of a given size.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "container.h"
#include "container_noop.h"
#include "container_bst.h"
#include "out.h"
#include "util.h"

static struct container *(*container_new_by_type[MAX_CONTAINER_TYPE])() = {
	container_noop_new,
	container_bst_new
};

static void (*container_delete_by_type[MAX_CONTAINER_TYPE])() = {
	container_noop_delete,
	container_bst_delete
};

struct container *
container_new(enum container_type type)
{
	return container_new_by_type[type]();
}

void
container_delete(struct container *container)
{
	ASSERT(container->type < MAX_CONTAINER_TYPE);
	container_delete_by_type[container->type](container);
}

void
container_init(struct container *container, enum container_type type,
	struct container_operations *c_ops)
{
	container->type = type;
	container->c_ops = c_ops;
}
