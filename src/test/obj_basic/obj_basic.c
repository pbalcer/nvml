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
 * obj_basic.c -- test for obj
 */
#include <stdbool.h>
#include <assert.h>
#include <pthread.h>
#include "unittest.h"

#define BASIC_LAYOUT "basic"
#define BASIC_SIZE ((1024 * 1024) * 100)

struct node {
	int value;
	POBJ(struct node) next;
};

TX_EXEC(basic_tx, root)
{
	struct node *n = root;
	OUT("node %p value: %d", n, n->value);
	POBJ_SET(n->value, 5);
	if (POBJ_IS_NULL(n->next)) {
		OUT("next NULL!");
		POBJ_NEW(n->next);
		struct node *next = D(n->next);
		POBJ_SET(next->value, 10);
	} else {
		struct node *next = D(n->next);
		OUT("next %p value: %d", next, next->value);
		POBJ_DELETE(n->next);
	}

	return TX_STATE_SUCCESS;
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_basic");
	if (argc < 2)
	FATAL("usage: %s file", argv[0]);

	PMEMobjpool *p = NULL;

	if (pmemobj_check(argv[1], BASIC_LAYOUT) == 1) {
		p = pmemobj_open(argv[1], BASIC_LAYOUT);
	} else {
		p = pmemobj_create(argv[1], BASIC_LAYOUT, BASIC_SIZE, S_IRWXU);
	}

	ASSERT(p != NULL);

	pmemobj_init_root(p, sizeof (struct node));

	//pmemobj_tx_exec(p, __tx);

	pmemobj_tx_exec(p, TX(root, {
		struct node *n = root;
		OUT("node %p value: %d", n, n->value);
		POBJ_SET(n->value, 5);
		if (POBJ_IS_NULL(n->next)) {
			OUT("next NULL!");
			POBJ_NEW(n->next);
			struct node *next = D(n->next);
			POBJ_SET(next->value, 10);
		} else {
			struct node *next = D(n->next);
			OUT("next %p value: %d", next, next->value);
			POBJ_DELETE(n->next);
		}

		return TX_STATE_SUCCESS;
	}));

	/* all done */
	pmemobj_close(p);
	DONE(NULL);
}
