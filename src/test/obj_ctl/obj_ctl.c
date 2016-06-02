/*
 * Copyright 2016, Intel Corporation
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
 * obj_ctl.c -- tests for the libpmemobj control module
 */

#include "unittest.h"

static void
test_ctl_parser()
{
	int ret;
	ret = pmemobj_ctl(NULL, "a.b.c.d", NULL, NULL);
	UT_ASSERTne(ret, 0);
	ret = pmemobj_ctl(NULL, "", NULL, NULL);
	UT_ASSERTne(ret, 0);
	ret = pmemobj_ctl(NULL, "debug.", NULL, NULL);
	UT_ASSERTne(ret, 0);
	ret = pmemobj_ctl(NULL, ".", NULL, NULL);
	UT_ASSERTne(ret, 0);
	ret = pmemobj_ctl(NULL, "..", NULL, NULL);
	UT_ASSERTne(ret, 0);

	/* test methods set read to 0 and write to 1 if successful */
	int arg_read = 1;
	int arg_write = 0;

	/* correct name, wrong args */
	ret = pmemobj_ctl(NULL, "debug.test_rw", NULL, NULL);
	UT_ASSERTne(ret, 0);
	ret = pmemobj_ctl(NULL, "debug.test_wo", &arg_read, NULL);
	UT_ASSERTne(ret, 0);
	ret = pmemobj_ctl(NULL, "debug.test_wo", &arg_read, &arg_write);
	UT_ASSERTne(ret, 0);
	ret = pmemobj_ctl(NULL, "debug.test_ro", NULL, &arg_write);
	UT_ASSERTne(ret, 0);
	ret = pmemobj_ctl(NULL, "debug.test_ro", &arg_read, &arg_write);
	UT_ASSERTne(ret, 0);

	ret = pmemobj_ctl(NULL, "debug.test_rw", &arg_read, &arg_write);
	UT_ASSERTeq(ret, 0);
	UT_ASSERTeq(arg_read, 0);
	UT_ASSERTeq(arg_write, 1);

	arg_read = 1;
	arg_write = 0;

	ret = pmemobj_ctl(NULL, "debug.test_ro", &arg_read, NULL);
	UT_ASSERTeq(ret, 0);
	UT_ASSERTeq(arg_read, 0);
	UT_ASSERTeq(arg_write, 0);

	arg_read = 1;
	arg_write = 0;

	ret = pmemobj_ctl(NULL, "debug.test_wo", NULL, &arg_write);
	UT_ASSERTeq(ret, 0);
	UT_ASSERTeq(arg_read, 1);
	UT_ASSERTeq(arg_write, 1);
}

static void
test_heap_stats(PMEMobjpool *pop)
{
	int ret;

	size_t allocated = 1;
	size_t freed = 1;

	ret = pmemobj_ctl(pop, "stats.heap.allocated", &allocated, NULL);
	UT_ASSERTeq(ret, 0);
	UT_ASSERTeq(allocated, 0);

	pmemobj_ctl(pop, "stats.heap.freed", &freed, NULL);
	UT_ASSERTeq(ret, 0);
	UT_ASSERTeq(freed, 0);

	PMEMoid oid;
	pmemobj_alloc(pop, &oid, 64, 0, NULL, NULL);
	pmemobj_free(&oid);

	pmemobj_ctl(pop, "stats.heap.allocated", &allocated, NULL);
	UT_ASSERTeq(ret, 0);
	UT_ASSERTeq(allocated, 128);

	pmemobj_ctl(pop, "stats.heap.freed", &freed, NULL);
	UT_ASSERTeq(ret, 0);
	UT_ASSERTeq(freed, 128);

	size_t active_zones = 0;
	ret = pmemobj_ctl(pop, "stats.heap.active_zones", &active_zones, NULL);
	UT_ASSERTeq(ret, 0);
	UT_ASSERTeq(active_zones, 1);
}

int
main(int argc, char *argv[])
{
	test_ctl_parser();

	START(argc, argv, "obj_ctl");

	if (argc != 2)
		UT_FATAL("usage: %s file-name", argv[0]);

	const char *path = argv[1];

	PMEMobjpool *pop;
	if ((pop = pmemobj_create(path, "ctl", PMEMOBJ_MIN_POOL,
		S_IWUSR | S_IRUSR)) == NULL)
		UT_FATAL("!pmemobj_create: %s", path);

	test_heap_stats(pop);

	pmemobj_close(pop);

	DONE(NULL);
}
