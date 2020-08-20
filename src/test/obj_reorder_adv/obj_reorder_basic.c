// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2018, Intel Corporation */

/*
 * obj_reorder_basic.c -- a simple unit test for store reordering
 *
 * usage: obj_reorder_basic file w|c
 * w - write data
 * c - check data consistency
 *
 */

#include "libpmemobj/atomic_base.h"
#include "libpmemobj/tx_base.h"
#include "unittest.h"
#include "util.h"
#include "valgrind_internal.h"

#define LAYOUT_NAME "intro_1"

/*
 * write_consistent -- (internal) write data in a consistent manner
 */
static void
write_consistent(struct pmemobjpool *pop)
{
	pmemobj_alloc(pop, NULL, 3 * (1 << 20), 0, NULL, NULL);
}

/*
 * check_consistency -- (internal) check buf consistency
 */
static int
check_consistency(struct pmemobjpool *pop)
{
	printf("check!\n");
	return 0;
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_reorder_basic");

	util_init();

	if (argc != 3 || strchr("wc", argv[1][0]) == 0 || argv[1][1] != '\0')
		UT_FATAL("usage: %s w|c file", argv[0]);

	PMEMobjpool *pop = pmemobj_open(argv[2], LAYOUT_NAME);
	UT_ASSERT(pop != NULL);

	char opt = argv[1][0];
	switch (opt) {
		case 'w':
		{
			pmemobj_alloc(pop, NULL, 3 * (1 << 20), 0, NULL, NULL);
			VALGRIND_EMIT_LOG("PMREORDER_MARKER_WRITE.BEGIN");

			write_consistent(pop);
			break;
		}
		case 'c':
		{
			VALGRIND_EMIT_LOG("PMREORDER_MARKER_WRITE.BEGIN");

			int ret = check_consistency(pop);
			pmemobj_close(pop);
			END(ret);
		}
		default:
			UT_FATAL("Unrecognized option %c", opt);
	}
	VALGRIND_EMIT_LOG("PMREORDER_MARKER_WRITE.END");

	pmemobj_close(pop);
	DONE(NULL);
}
