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
 * container_bst.c -- implementation of binary search tree container
 *
 * This is the simplest working container implementation to be used as a
 * reference for further data structures work.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include "container.h"
#include "container_bst.h"
#include "util.h"
#include "out.h"

static struct bst_node *
bst_create_node(struct bst_node *p, val_t v, uint64_t k)
{
	struct bst_node *n = Malloc(sizeof (*n));
	if (n != NULL) {
		n->parent = p;
		n->value = v;
		n->key = k;
		n->left = NULL;
		n->right = NULL;
	}
	return n;
}

bool
bst_add(struct container *container, uint64_t key, val_t value)
{
	struct container_bst *c = (struct container_bst *)container;
	struct bst_node *p = NULL;
	struct bst_node **n = NULL;
	for (n = &c->root; *n != NULL;
		n = (*n)->key > key ? &(*n)->left : &(*n)->right) {
		p = *n;
	}
	*n = bst_create_node(p, value, key);

	return *n != NULL;
}

static struct bst_node *
bst_find_node(struct container *container, uint64_t key, bool greater)
{
	struct container_bst *c = (struct container_bst *)container;

	struct bst_node *n = NULL;
	struct bst_node *b = NULL;
	for (n = c->root; n != NULL && n->key != key;
		n = n->key > key ? n->left : n->right) {
		if (greater && n->key > key)
			b = n;
	}

	return n ? : b;
}

static void
bst_remove(struct container *container, struct bst_node *n)
{
	struct container_bst *c = (struct container_bst *)container;

	struct bst_node **nref = NULL;
	if (c->root == n)
		nref = &c->root;
	else
		nref = (n->parent->left == n ? &n->parent->left :
			&n->parent->right);

	if (n->left != NULL && n->right != NULL) {
		/* swap with minimum */
		struct bst_node **mref = &n->right;
		while ((*mref)->left) {
			mref = &(*mref)->left;
		}
		struct bst_node *m = (*mref);
		n->value = m->value;
		n->key = m->key;
		bst_remove(container, m);
	} else {
		*nref = n->left ? : n->right;
		if (*nref)
			(*nref)->parent = n->parent;
		Free(n);
	}
}

static val_t
best_get_rm(struct container *c, uint64_t key, bool greater)
{
	struct bst_node *n = bst_find_node(c, key, greater);
	if (n == NULL) {
		return NULL_VAL;
	}
	val_t ret = n->value;
	bst_remove(c, n);

	return ret;
}

val_t
bst_get_rm_eq(struct container *container, uint64_t key)
{
	struct container_bst *c = (struct container_bst *)container;

	if (pthread_mutex_lock(c->lock) != 0) {
		LOG(4, "Failed to acquire container mutex");
		return NULL_VAL;
	}

	val_t v = best_get_rm(container, key, false);

	if (pthread_mutex_unlock(c->lock) != 0) {
		LOG(4, "Failed to release container mutex");
	}

	return v;
}

val_t
bst_get_rm_ge(struct container *container, uint64_t key)
{
	struct container_bst *c = (struct container_bst *)container;

	if (pthread_mutex_lock(c->lock) != 0) {
		LOG(4, "Failed to acquire container mutex");
		return NULL_VAL;
	}

	val_t v = best_get_rm(container, key, true);

	if (pthread_mutex_unlock(c->lock) != 0) {
		LOG(4, "Failed to release container mutex");
	}

	return v;
}

struct container_operations container_bst_ops = {
	.add = bst_add,
	.get_rm_eq = bst_get_rm_eq,
	.get_rm_ge = bst_get_rm_ge
};

struct container *
container_bst_new()
{
	struct container_bst *container = Malloc(sizeof (*container));
	if (container == NULL) {
		goto error_container_malloc;
	}

	container_init(&container->super, CONTAINER_BINARY_SEARCH_TREE,
		&container_bst_ops);

	container->lock = Malloc(sizeof (*container->lock));
	if (container->lock == NULL) {
		goto error_lock_malloc;
	}

	if (pthread_mutex_init(container->lock, NULL) != 0) {
		goto error_lock_init;
	}

	container->root = NULL;

	return (struct container *)container;

error_lock_init:
	Free(container->lock);
error_lock_malloc:
	Free(container);
error_container_malloc:
	return NULL;
}

static void
bst_delete(struct bst_node *n)
{
	if (n == NULL)
		return;

	bst_delete(n->left);
	bst_delete(n->right);
	Free(n);
}

void
container_bst_delete(struct container *container)
{
	struct container_bst *c = (struct container_bst *)container;

	bst_delete(c->root);

	if (pthread_mutex_destroy(c->lock) != 0) {
		LOG(4, "Failed to destroy container lock");
	}
	Free(c->lock);
	Free(c);
}
