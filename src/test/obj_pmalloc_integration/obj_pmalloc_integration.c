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
 * obj_pmalloc_integration.c -- integration test for pmalloc
 */
#include <stdbool.h>
#include <assert.h>
#include "unittest.h"
#include "pmalloc.h"
#include "bucket.h"
#include "arena.h"
#include "backend.h"
#include "backend_persistent.h"
#include "pool.h"
#include "util.h"

#define	TEST_ALLOC_SIZE 1024
#define	TEST_POOL_SIZE 1024 * 1024 * 40 /* 40MB */
#define	TEST_VALUE 123

void
test_flow()
{
	void *backend_ptr = MALLOC(TEST_POOL_SIZE);
	struct pmalloc_pool *p = pool_open(backend_ptr,
		TEST_POOL_SIZE, 0);

	uint64_t test_ptr = 0;
	pmalloc(p, &test_ptr, TEST_ALLOC_SIZE);

	int *a = pdirect(p, test_ptr);
	ASSERTrange(a, backend_ptr, TEST_POOL_SIZE);

	*a = TEST_VALUE;

	prealloc(p, &test_ptr, TEST_ALLOC_SIZE*2);
	ASSERT(*a == TEST_VALUE);

	pfree(p, &test_ptr);
	ASSERT(test_ptr == NULL_OFFSET);

	pool_close(p);

	pool_check(backend_ptr, TEST_POOL_SIZE, 0);

	FREE(backend_ptr);
}

#define	TEST_REALLOC_SIZE (CHUNKSIZE - 1024)

void
test_realloc()
{
	void *backend_ptr = MALLOC(TEST_POOL_SIZE);
	struct pmalloc_pool *p = pool_open(backend_ptr,
		TEST_POOL_SIZE, 0);

	uint64_t test_ptr = 0;

	prealloc(p, &test_ptr, TEST_REALLOC_SIZE);
	int *a = pdirect(p, test_ptr);
	ASSERTrange(a, backend_ptr, TEST_POOL_SIZE);
	*a = TEST_VALUE;

	prealloc(p, &test_ptr, TEST_REALLOC_SIZE * 2);
	int *a_new = pdirect(p, test_ptr);

	ASSERT(*a_new == *a);
	ASSERT(a != a_new); /* XXX remove after extend implementation */

	prealloc(p, &test_ptr, 0);
	ASSERT(test_ptr == NULL_OFFSET);

	pool_close(p);

	pool_check(backend_ptr, TEST_POOL_SIZE, 0);

	FREE(backend_ptr);
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_pmalloc_integration");

	test_flow();
	test_realloc();

	DONE(NULL);
}
