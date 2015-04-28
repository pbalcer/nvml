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
 * ctree.c -- crit-bit tree implementation
 */

#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include "util.h"
#include "out.h"
#include "ctree.h"

#define	BIT_IS_SET(n, i) (!!((n) & (1L << (i))))
#define	NODE_IS_ACCESSOR(node) (BIT_IS_SET((uintptr_t)(node), 0))
#define	NODE_ACCESSOR_GET(node) ((void *)(node) - 1)
#define	NODE_ACCESSOR_SET(d, node) ((d) = ((void *)(node) + 1))

struct node {
	void *slots[2]; /* slots for either accessor or leaf nodes */
	int diff;	/* most siginificant difference */
};

struct ctree {
	void *root;
	pthread_mutex_t lock;
};

/*
 * find_crit_bit -- (internal) finds the most siginificant different bit
 */
static int
find_crit_bit(uint64_t lhs, uint64_t rhs)
{
	return 64 - __builtin_clzll(lhs ^ rhs) - 1;
}

/*
 * ctree_new -- allocates and initializes crit-bit tree instance
 */
struct ctree *
ctree_new()
{
	struct ctree *t = Malloc(sizeof (*t));
	if (t == NULL)
		goto error_ctree_malloc;

	if (pthread_mutex_init(&t->lock, NULL) != 0)
		goto error_lock_init;

	t->root = NULL;

	return t;

error_lock_init:
	Free(t);
error_ctree_malloc:
	return NULL;
}

/*
 * ctree_delete -- cleanups and frees crit-bit tree instance
 */
void
ctree_delete(struct ctree *t)
{
	while (t->root)
		ctree_remove(t, 0, 0);

	if ((errno = pthread_mutex_destroy(&t->lock)) != 0)
		LOG(1, "!pthread_mutex_destroy");

	Free(t);
}

/*
 * ctree_inesrt -- inserts a new key into the tree
 */
int
ctree_insert(struct ctree *t, uint64_t key)
{
	void **dst = &t->root;
	struct node *a = NULL;
	int err;

	if ((err = pthread_mutex_lock(&t->lock)) != 0)
		return err;

	/* descend the path until a most closesly matching key is found */
	while (NODE_IS_ACCESSOR(*dst)) {
		a = NODE_ACCESSOR_GET(*dst);
		dst = &a->slots[BIT_IS_SET(key, a->diff)];
	}

	uint64_t *dstkeyp = *dst;
	uint64_t *kp = Malloc(sizeof (uint64_t)); /* allocate leaf node */
	if (kp == NULL) {
		err = ENOMEM;
		goto error_leaf_malloc;
	}

	*kp = key;
	if (dstkeyp == NULL) { /* root */
		*dst = kp;
		goto out;
	}

	uint64_t dstkey = *dstkeyp;
	struct node *n = Malloc(sizeof (*n)); /* accessor node */
	if (n == NULL) {
		err = ENOMEM;
		goto error_accessor_malloc;
	}

	if (dstkey == key) {
		err = EINVAL;
		goto error_duplicate;
	}

	n->diff = find_crit_bit(dstkey, key);

	/* insert the node at the direction based on the critical bit */
	int d = BIT_IS_SET(key, n->diff);
	n->slots[d] = kp;

	/* find the appropriate position in the tree to insert the node */
	dst = &t->root;
	while (NODE_IS_ACCESSOR(*dst)) {
		a = NODE_ACCESSOR_GET(*dst);

		/* the critical bits have to be sorted */
		if (a->diff < n->diff) break;
		dst = &a->slots[BIT_IS_SET(key, a->diff)];
	}

	/* insert the found destination in the other slot */
	n->slots[!d] = *dst;
	NODE_ACCESSOR_SET(*dst, n);

out:
	if ((errno = pthread_mutex_unlock(&t->lock)) != 0)
		LOG(1, "!pthread_mutex_unlock");

	return err;

error_accessor_malloc:
	Free(kp);
error_duplicate:
	Free(n);
error_leaf_malloc:
	if ((errno = pthread_mutex_unlock(&t->lock)) != 0)
		LOG(1, "!pthread_mutex_unlock");

	return err;
}

/*
 * ctree_find -- searches for a key in the tree
 */
uint64_t
ctree_find(struct ctree *t, uint64_t key)
{
	uint64_t *dst = t->root;
	struct node *a = NULL;
	while (NODE_IS_ACCESSOR(dst)) {
		a = NODE_ACCESSOR_GET(dst);
		dst = a->slots[BIT_IS_SET(key, a->diff)];
	}

	return dst ? *dst : 0;
}

/*
 * ctree_remove -- removes a key from the tree
 */
uint64_t
ctree_remove(struct ctree *t, uint64_t key, int eq)
{
	void **p = NULL; /* parent ref */
	void **dst = &t->root;
	int d = 0; /* last taken direction */
	struct node *a = NULL;

	if ((errno = pthread_mutex_lock(&t->lock)) != 0) {
		LOG(1, "!pthread_mutex_lock");
		return 0;
	}

	uint64_t k = 0;
	if (t->root == NULL)
		goto out;

	while (NODE_IS_ACCESSOR(*dst)) {
		a = NODE_ACCESSOR_GET(*dst);
		p = dst;
		dst = &a->slots[(d = BIT_IS_SET(key, a->diff))];
	}

	k = **(uint64_t **)dst;
	if ((k < key) || (eq && k != key)) {
		k = 0;
		goto out;
	}

	if (p) {
		*p = a->slots[!d];
	}

	Free(*dst);
	Free(a);

	if (!p) { /* root */
		*dst = NULL;
	}

out:
	if ((errno = pthread_mutex_unlock(&t->lock)) != 0)
		LOG(1, "!pthread_mutex_unlock");

	return k;
}
