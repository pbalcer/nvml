// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2019, Intel Corporation */

/*
 * obj_defrag.c -- unit test for pmemobj_defrag
 */

#include "unittest.h"
#include <stdio.h>
#include <limits.h>

#define OBJECT_SIZE 2048
#define NPOOLS 1025
#define POOL_SIZE ((1 << 20) * 16)

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_defrag");

	const char *dir = argv[1];
	PMEMobjpool **pops = malloc(sizeof(PMEMobjpool *) * NPOOLS);

	for (int i = 0; i < NPOOLS; ++i) {
		char path[1024];
		snprintf(path, 1024, "%s%s.%d", dir, "/pool", i);
		pops[i] = pmemobj_create(path, POBJ_LAYOUT_NAME(basic),
			POOL_SIZE, S_IWUSR | S_IRUSR);
		if (pops[i] == NULL)
			UT_FATAL("!pmemobj_create: %s", path);
	}

	for (int i = NPOOLS - 1; i >= 0; --i) {
		while (1) {
			struct pobj_action act;
			PMEMoid oid = pmemobj_reserve(pops[i], &act, OBJECT_SIZE, 0);
			if (OID_IS_NULL(oid))
				break;

			UT_ASSERTeq(oid.off, act.heap.offset);
			UT_ASSERT(act.heap.usable_size > OBJECT_SIZE);
			UT_ASSERT(oid.off > 0);
			UT_ASSERT(oid.off < POOL_SIZE);
		}
	}

	for (int i = 0; i < NPOOLS; ++i) {
		pmemobj_close(pops[i]);
	}

	free(pops);

	DONE(NULL);
}
