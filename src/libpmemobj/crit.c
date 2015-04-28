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
 * crit.c -- crit-bit tree implementation
 */

#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include "util.h"
#include "out.h"
#include "crit.h"

struct node {
	void *childs[2];
	int diff;
};

struct crit {
	void *root;
	pthread_mutex_t *lock;
};

#define	KEY_BYTES 8
#define	BIT_IS_SET(n, i) (!!((n) & (1L << (i))))
#define	NODE_IS_ACCESSOR(node) (BIT_IS_SET((uintptr_t)(node), 0))
#define	NODE_ACCESSOR_GET(node) ((void *)(node) - 1)
#define	NODE_ACCESSOR_SET(d, node) ((d) = ((void *)(node) + 1))

static int
find_crit_bit(uint64_t lhs, uint64_t rhs)
{
	return 64 - __builtin_clzll(lhs ^ rhs) - 1;
}

struct crit *
crit_new()
{
	struct crit *t = Malloc(sizeof (*t));
	if (t == NULL)
		goto error_crit_malloc;

	t->lock = Malloc(sizeof (*t->lock));
	if (t->lock == NULL)
		goto error_lock_malloc;

	if (pthread_mutex_init(t->lock, NULL) != 0)
		goto error_lock_init;

	t->root = NULL;

	return t;

error_lock_init:
	Free(t->lock);
error_lock_malloc:
	Free(t);
error_crit_malloc:
	return NULL;
}

void
crit_delete(struct crit *t)
{
	while (t->root)
		crit_remove(t, 0, 0);

	if (pthread_mutex_destroy(t->lock) != 0)
		LOG(1, "!pthread_mutex_destroy");

	Free(t->lock);

	Free(t);
}

int
crit_insert(struct crit *t, uint64_t key)
{
	void **dst = &t->root;
	struct node *a = NULL;
	int err;

	if ((err = pthread_mutex_lock(t->lock) != 0)) {
		LOG(1, "!pthread_mutex_lock");
		return err;
	}

	while (NODE_IS_ACCESSOR(*dst)) {
		a = NODE_ACCESSOR_GET(*dst);
		dst = &a->childs[BIT_IS_SET(key, a->diff)];
	}

	uint64_t *dstkeyp = *dst;
	uint64_t *kp = Malloc(sizeof (uint64_t)); /* leaf node */
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

	n->diff = find_crit_bit(dstkey, key);
	if (n->diff == -1) {
		err = EINVAL;
		goto error_duplicate;
	}

	int d = BIT_IS_SET(key, n->diff);
	n->childs[d] = kp;

	dst = &t->root;
	while (NODE_IS_ACCESSOR(*dst)) {
		a = NODE_ACCESSOR_GET(*dst);
		if (a->diff < n->diff) break;
		dst = &a->childs[BIT_IS_SET(key, a->diff)];
	}

	n->childs[!d] = *dst;
	NODE_ACCESSOR_SET(*dst, n);

out:
	if ((err = pthread_mutex_unlock(t->lock)) != 0)
		LOG(1, "!pthread_mutex_unlock");

	return err;

error_accessor_malloc:
	Free(kp);
error_duplicate:
	Free(n);
error_leaf_malloc:
	if (pthread_mutex_unlock(t->lock) != 0)
		LOG(1, "!pthread_mutex_unlock");
	return err;
}

uint64_t
crit_find(struct crit *t, uint64_t key)
{
	uint64_t *dst = t->root;
	struct node *a = NULL;
	while (NODE_IS_ACCESSOR(dst)) {
		a = NODE_ACCESSOR_GET(dst);
		dst = a->childs[BIT_IS_SET(key, a->diff)];
	}

	return dst ? *dst : 0;
}

uint64_t
crit_remove(struct crit *t, uint64_t key, int eq)
{
	void **p = NULL; /* parent ref */
	void **dst = &t->root;
	int d = 0; /* last taken direction */
	struct node *a = NULL;

	if (pthread_mutex_lock(t->lock) != 0) {
		LOG(1, "!pthread_mutex_lock");
		return 0;
	}

	uint64_t k = 0;
	if (t->root == NULL)
		goto out;

	while (NODE_IS_ACCESSOR(*dst)) {
		a = NODE_ACCESSOR_GET(*dst);
		p = dst;
		dst = &a->childs[(d = BIT_IS_SET(key, a->diff))];
	}

	k = **(uint64_t **)dst;
	if (eq && k != key) {
		k = 0;
		goto out;
	}

	if (p) {
		*p = a->childs[!d];
	}

	Free(*dst);
	Free(a);

	if (!p) { /* root */
		*dst = NULL;
	}

out:
	if (pthread_mutex_unlock(t->lock) != 0)
		LOG(1, "!pthread_mutex_unlock");

	return k;
}
