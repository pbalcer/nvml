// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2020, Intel Corporation */

#include <errno.h>

#include "libpmem2.h"
#include "out.h"
#include "pmem2_utils.h"
#include "source.h"

/*
 * pmem2_device_dax_alignment -- checks the alignment of a given
 * dax device from given source
 */
int
pmem2_device_dax_alignment(const struct pmem2_source *src, size_t *alignment)
{
	ERR("Cannot read Device Dax alignment - ndctl is not available");

	return PMEM2_E_NOSUPP;
}

/*
 * pmem2_device_dax_size -- checks the size of a given dax device from
 * given source
 */
int
pmem2_device_dax_size(const struct pmem2_source *src, size_t *size)
{
	ERR("Cannot read Device Dax size - ndctl is not available");

	return PMEM2_E_NOSUPP;
}

/*
 * pmem2_source_numa_node -- gets the numa node on which a pmem file
 * is located from given source structure
 */
int
pmem2_source_numa_node(const struct pmem2_source *src, int *numa_node)
{
	ERR("Cannot get numa node from source - ndctl is not available");

	return PMEM2_E_NOSUPP;
}