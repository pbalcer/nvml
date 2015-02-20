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
 * container.h -- internal definitions for containers
 */

enum container_type {
	CONTAINER_NOOP,
	CONTAINER_BINARY_SEARCH_TREE,
	/* CONTAINER_LOCK_FREE_BITWISE_TRIE, */

	MAX_CONTAINER_TYPE
};

/*
 * Type of the value stored in the container, the type is a 64bit unsigned
 * integer because the value stored is memory block unique id.
 */
typedef uint64_t val_t;

/*
 * Can't be 0 because thats a legal unique id of the first memory block in
 * the first zone.
 */
#define	NULL_VAL ~(0)

struct container;

struct container_operations {
	/*
	 * Adds a key-value pair to the container.
	 */
	bool (*add)(struct container *c, uint64_t key, val_t value);
	/*
	 * Gets and removes a key-value pair from the container that has
	 * an equal key.
	 */
	val_t (*get_rm_eq)(struct container *c, uint64_t key);
	/*
	 * As above, but key has to be equal or greater.
	 */
	val_t (*get_rm_ge)(struct container *c, uint64_t key);
};

struct container {
	enum container_type type;
	struct container_operations *c_ops;
};

struct container *container_new(enum container_type type);
void container_delete(struct container *container);
void container_init(struct container *container, enum container_type type,
	struct container_operations *c_ops);
