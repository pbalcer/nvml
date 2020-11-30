// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2017-2020, Intel Corporation */

/*
 * pmem2_source_numa.c -- unit test for getting numa node from source
 */

#include <stdlib.h>
#include <ndctl/libndctl.h>

#include "libpmem2.h"
#include "unittest.h"
#include "ut_pmem2.h"

#define PMEM_LEN 4096

static size_t numa_nodes_size;
static size_t numa_nodes_it;
static int **numa_nodes;

int
main(int argc, char *argv[])
{
	START(argc, argv, "pmem2_source_numa");

	if (argc < 3)
		UT_FATAL("usage: %s (file numa_node)...", argv[0]);

	unsigned files = (unsigned)(argc - 1) / 2;
	numa_nodes = MALLOC(files * sizeof(numa_nodes[0]));
	numa_nodes_size = files;

	char **args = argv + 1;
	struct pmem2_config *cfg;
	PMEM2_CONFIG_NEW(&cfg);

	for (unsigned i = 0; i < files; i++) {
		unsigned arg_position = i * 2;
		int fd = OPEN(args[arg_position], O_CREAT | O_RDWR, 0666);
		POSIX_FALLOCATE(fd, 0, PMEM_LEN);
		struct pmem2_source *src;
		PMEM2_SOURCE_FROM_FD(&src, fd);

		*numa_nodes[i] = atoi(args[arg_position + 1]);
		int numa_node;
		int ret = pmem2_source_numa_node(src, &numa_node);
		UT_ASSERTeq(ret, 0);
		UT_ASSERTeq(numa_node, *numa_nodes[i]);

		PMEM2_SOURCE_DELETE(&src);
		CLOSE(fd);
	}

	PMEM2_CONFIG_DELETE(&cfg);
	FREE(numa_nodes);

	DONE(NULL);
}

FUNC_MOCK(ndctl_region_get_numa_node, int, const struct ndctl_region *pregion)
FUNC_MOCK_RUN_DEFAULT {
	if (numa_nodes_it < numa_nodes_size) {
		if (pregion != NULL) {
			return *numa_nodes[numa_nodes_it++];
		}
	}
	return -1;
}
FUNC_MOCK_END