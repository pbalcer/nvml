/*
 * Copyright 2015-2016, Intel Corporation
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
 * container_ctree.c -- ctree bucket container implementation
 */

 #include <errno.h>
 #include <pthread.h>

 #include "libpmem.h"
 #include "libpmemobj.h"
 #include "util.h"
 #include "out.h"
 #include "sys_util.h"
 #include "redo.h"
 #include "heap_layout.h"
 #include "memops.h"
 #include "memblock.h"
 #include "bucket.h"
 #include "ctree.h"
 #include "lane.h"
 #include "pmalloc.h"
 #include "heap.h"
 #include "list.h"
 #include "obj.h"
 #include "valgrind_internal.h"
 #include "container_ctree.h"

/*
 * The elements in the tree are sorted by the key and it's vital that the
 * order is by size, hence the order of the pack arguments.
 */
#define CHUNK_KEY_PACK(z, c, b, s)\
((uint64_t)(s) << 48 | (uint64_t)(b) << 32 | (uint64_t)(c) << 16 | (z))

#define CHUNK_KEY_GET_ZONE_ID(k)\
((uint16_t)((k & 0xFFFF)))

#define CHUNK_KEY_GET_CHUNK_ID(k)\
((uint16_t)((k & 0xFFFF0000) >> 16))

#define CHUNK_KEY_GET_BLOCK_OFF(k)\
((uint16_t)((k & 0xFFFF00000000) >> 32))

#define CHUNK_KEY_GET_SIZE_IDX(k)\
((uint16_t)((k & 0xFFFF000000000000) >> 48))

struct block_container_ctree {
	struct block_container super;
	struct ctree *tree;
};

/*
 * bucket_tree_insert_block -- (internal) inserts a new memory block
 *	into the container
 */
static int
bucket_tree_insert_block(struct block_container *bc, struct memory_block m)
{
	/*
	 * Even though the memory block representation of an object uses
	 * relatively large types in practise the entire memory block structure
	 * needs to fit in a single 64 bit value - the type of the key in the
	 * container tree.
	 * Given those limitations a reasonable idea might be to make the
	 * memory_block structure be the size of single uint64_t, which would
	 * work for now, but if someday someone decides there's a need for
	 * larger objects the current implementation would allow them to simply
	 * replace this container instead of making little changes all over
	 * the heap code.
	 */
	ASSERT(m.chunk_id < MAX_CHUNK);
	ASSERT(m.zone_id < UINT16_MAX);
	ASSERTne(m.size_idx, 0);

	struct block_container_ctree *c = (struct block_container_ctree *)bc;

	uint64_t key = CHUNK_KEY_PACK(m.zone_id, m.chunk_id, m.block_off,
				m.size_idx);

	return ctree_insert(c->tree, key, 0);
}

/*
 * bucket_tree_get_rm_block_bestfit -- (internal) removes and returns the
 *	best-fit memory block for size
 */
static int
bucket_tree_get_rm_block_bestfit(struct block_container *bc,
	struct memory_block *m)
{
	uint64_t key = CHUNK_KEY_PACK(m->zone_id, m->chunk_id, m->block_off,
			m->size_idx);

	struct block_container_ctree *c = (struct block_container_ctree *)bc;

	if ((key = ctree_remove(c->tree, key, 0)) == 0)
		return ENOMEM;

	m->chunk_id = CHUNK_KEY_GET_CHUNK_ID(key);
	m->zone_id = CHUNK_KEY_GET_ZONE_ID(key);
	m->block_off = CHUNK_KEY_GET_BLOCK_OFF(key);
	m->size_idx = CHUNK_KEY_GET_SIZE_IDX(key);

	return 0;
}

/*
 * bucket_tree_get_rm_block_exact -- (internal) removes exact match memory block
 */
static int
bucket_tree_get_rm_block_exact(struct block_container *bc,
	struct memory_block m)
{
	uint64_t key = CHUNK_KEY_PACK(m.zone_id, m.chunk_id, m.block_off,
			m.size_idx);

	struct block_container_ctree *c = (struct block_container_ctree *)bc;

	if ((key = ctree_remove(c->tree, key, 1)) == 0)
		return ENOMEM;

	return 0;
}

/*
 * bucket_tree_get_block_exact -- (internal) finds exact match memory block
 */
static int
bucket_tree_get_block_exact(struct block_container *bc, struct memory_block m)
{
	uint64_t key = CHUNK_KEY_PACK(m.zone_id, m.chunk_id, m.block_off,
			m.size_idx);

	struct block_container_ctree *c = (struct block_container_ctree *)bc;

	return ctree_find(c->tree, key) == key ? 0 : ENOMEM;
}

/*
 * bucket_tree_is_empty -- (internal) checks whether the bucket is empty
 */
static int
bucket_tree_is_empty(struct block_container *bc)
{
	struct block_container_ctree *c = (struct block_container_ctree *)bc;
	return ctree_is_empty(c->tree);
}

/*
 * bucket_tree_clear -- (internal) removes all nodes from the tree
 */
static void
bucket_tree_clear(struct block_container *bc)
{
	struct block_container_ctree *c = (struct block_container_ctree *)bc;

	ctree_clear(c->tree);
}

/*
 * Tree-based block container used to provide best-fit functionality to the
 * bucket. The time complexity for this particular container is O(k) where k is
 * the length of the key.
 *
 * The get methods also guarantee that the block with lowest possible address
 * that best matches the requirements is provided.
 */
const struct block_container_ops container_ctree_ops = {
	.insert = bucket_tree_insert_block,
	.get_rm_exact = bucket_tree_get_rm_block_exact,
	.get_rm_bestfit = bucket_tree_get_rm_block_bestfit,
	.get_exact = bucket_tree_get_block_exact,
	.is_empty = bucket_tree_is_empty,
	.clear = bucket_tree_clear
};

/*
 * bucket_tree_create -- creates a new tree-based container
 */
struct block_container *
bucket_tree_create(void)
{
	struct block_container_ctree *bc = Malloc(sizeof(*bc));
	if (bc == NULL)
		goto error_container_malloc;

	bc->super.type = CONTAINER_CTREE;

	bc->tree = ctree_new();
	if (bc->tree == NULL)
		goto error_ctree_new;

	return &bc->super;

error_ctree_new:
	Free(bc);

error_container_malloc:
	return NULL;
}

/*
 * bucket_tree_delete -- deletes a tree container
 */
void
bucket_tree_delete(struct block_container *bc)
{
	struct block_container_ctree *c = (struct block_container_ctree *)bc;
	ctree_delete(c->tree);
	Free(bc);
}
