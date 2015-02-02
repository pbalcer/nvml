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
 * obj_pmalloc_container.c -- unit test for pmalloc container interface
 */
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include "unittest.h"
#include "container.h"

#define	TEST_KEY 0b10101L
#define	TEST_KEY2 0b10111L
#define	TEST_KEY_SMALLER 0b10001
#define	TEST_VALUE 1234

void
container_test_create_delete()
{
	struct container *c = container_new(CONTAINER_NOOP);
	ASSERT(c != NULL);
	ASSERT(c->type == CONTAINER_NOOP);
	container_delete(c);
}

void
container_test_lft_insert_get_remove(enum container_type ctype)
{
	struct container *c = container_new(ctype);
	ASSERT(c != NULL);
	ASSERT(c->c_ops->add(c, TEST_KEY, TEST_VALUE));
	ASSERT(c->c_ops->add(c, TEST_KEY2, TEST_VALUE));
	ASSERT(c->c_ops->get_rm_ge(c, TEST_KEY_SMALLER) == TEST_VALUE);
	ASSERT(c->c_ops->get_rm_ge(c, TEST_KEY_SMALLER) == TEST_VALUE);
	container_delete(c);
}

#define	TAB_SIZE 1000

void
container_test_lft_many(enum container_type ctype)
{
	srand(0);
	struct container *c = container_new(ctype);
	ASSERT(c != NULL);
	int *elements = MALLOC(sizeof (int) * TAB_SIZE);
	for (int i = 0; i < TAB_SIZE; ++i) {
		elements[i] = rand();
	}

	for (int i = 0; i < TAB_SIZE; ++i) {
		c->c_ops->add(c, elements[i], elements[i]);
	}

	for (int i = 0; i < TAB_SIZE; ++i) {
		ASSERT(elements[i] == c->c_ops->get_rm_eq(c, elements[i]));
	}

	FREE(elements);
	container_delete(c);
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_pmalloc_container");
	container_test_create_delete();
	container_test_lft_insert_get_remove(CONTAINER_BINARY_SEARCH_TREE);
	container_test_lft_many(CONTAINER_BINARY_SEARCH_TREE);

	DONE(NULL);
}
