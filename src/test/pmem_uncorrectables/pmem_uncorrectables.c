/*
 * Copyright 2017, Intel Corporation
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
 * pmem_poison.c -- unit test for persistent memory poison handling
 */

#include "unittest.h"
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <sys/ucontext.h>
#include <linux/types.h>
#include <linux/fiemap.h>
#include <linux/fs.h>

#include <ndctl/libndctl.h>
#include <daxctl/libdaxctl.h>
#include <linux/ndctl.h>

struct block {
	uint64_t offset;
	uint64_t length;
};

struct sysfs_iter {
	FILE *f;
	char *format;
};

struct block_iter {
	struct fiemap *f;
	size_t iter_pos;
};

static int
block_iter_init(struct block_iter *iter, int fd)
{
	os_stat_t st;
	if (os_fstat(fd, &st) < 0)
		return -1;	

	/* first read the number of extents in a file*/
	iter->f = malloc(sizeof(struct fiemap));
	if (iter->f == NULL)
		return -1;

	iter->f->fm_start = 0;
	iter->f->fm_length = st.st_size;
	iter->f->fm_flags = 0;
	iter->f->fm_extent_count = 0;

	int ret = ioctl(fd, FS_IOC_FIEMAP, iter->f);
	if (ret != 0) {
		free(iter->f);
		return ret;
	}

	unsigned nexts = iter->f->fm_mapped_extents;

	/* then allocate appropriately sized array and ask for the extents */
	struct fiemap *nf = realloc(iter->f,
		sizeof(struct fiemap) + nexts * sizeof(struct fiemap_extent));
	if (nf == NULL) {
		free(iter->f);
		return -1;
	}
	iter->f = nf;

	iter->f->fm_extent_count = nexts;

	ret = ioctl(fd, FS_IOC_FIEMAP, iter->f);
	if (ret != 0)
		return ret;

	iter->iter_pos = 0;

	return ret;
}

static void
block_iter_fini(struct block_iter *iter)
{
	free(iter->f);
}

static int
block_iter_next(struct block_iter *iter, struct block *b)
{
	if (iter->iter_pos == iter->f->fm_extent_count)
		return -1;

	b->offset = iter->f->fm_extents[iter->iter_pos].fe_physical;
	b->length = iter->f->fm_extents[iter->iter_pos].fe_length;

	iter->iter_pos++;

	return 0;
}

static size_t
block_iter_length(struct block_iter *iter)
{
	return iter->f->fm_extent_count;
}

static int
sysfs_iter_init(struct sysfs_iter *iter,
	int fd, const char *path, const char *format)
{
	os_stat_t st;
	if (os_fstat(fd, &st) < 0)
		return -1;

	char *tp = "block";
	char devpath[PATH_MAX];
	snprintf(devpath, PATH_MAX, "/sys/dev/%s/%d:%d/%s",
		tp, major(st.st_dev), minor(st.st_dev), path);

	if ((iter->f = fopen(devpath, "ro")) == NULL)
		return -1;

	iter->format = strdup(format);

	return 0;
}

static void
sysfs_iter_fini(struct sysfs_iter *iter)
{
	free(iter->format);
	fclose(iter->f);
}

static int
sysfs_iter_next(struct sysfs_iter *iter, ...)
{
	va_list ap;
	va_start(ap, iter);
	int ret = vfscanf(iter->f, iter->format, ap);
	va_end(ap);

	return ret;
}

static int
sysfs_iter_vnext(struct sysfs_iter *iter, va_list ap)
{
	int ret = vfscanf(iter->f, iter->format, ap);

	return ret;
}

static int
sysfs_read_single(int fd, const char *path, const char *format, ...)
{
	struct sysfs_iter iter;
	sysfs_iter_init(&iter, fd, path, format);

	va_list ap;
	va_start(ap, format); /* va_end will be called in fini */

	int ret = sysfs_iter_vnext(&iter, ap);

	va_end(ap);

	sysfs_iter_fini(&iter);
	return ret;
}

static int
block_is_inside_space(struct block *b, struct block *space, size_t nblocks)
{
	for (int i = 0; i < nblocks; ++i) {
		struct block *cur = &space[i];

		if (cur->offset <= b->offset &&
			cur->offset + cur->length >= b->offset + b->length) {
			return 1;
		} else if (cur->offset >= b->offset &&
			cur->offset + cur->length <= b->offset + b->length) {
			return 1;
		} else if (cur->offset + cur->length >= b->offset &&
			cur->offset + cur->length <= b->offset + b->length) {
			return 1;
		} else if (cur->offset >= b->offset &&
			cur->offset + cur->length <= b->offset + b->length) {
			return 1;
		}
	}

	return 0;
}

static void
block_normalize(struct block *b, size_t start, size_t end)
{
	if (b->offset > start)
		b->offset = start;

	if (b->offset + b->length > end)
		b->length = end - b->offset;

	b->offset -= start;
}

typedef void badblock_cb(struct block *badblock, void *arg);

static int
badblocks_foreach(int fd, badblock_cb *cb, void *arg)
{
	int ret;

	size_t sector_size = 0;
	ret = sysfs_read_single(fd, "queue/hw_sector_size",
		"%lu", &sector_size);
	if (ret != 1)
		goto out;

	size_t nblocks = 0;
	struct block_iter b_iter;
	block_iter_init(&b_iter, fd);

	struct block *file_blocks = malloc(sizeof(struct block) *
		block_iter_length(&b_iter));
	if (file_blocks == NULL)
		goto out;

	struct block b;
	while (block_iter_next(&b_iter, &b) == 0) {
		printf("file block: %lu (%lu) %lu\n", b.offset,
			b.offset / sector_size, b.length);
		file_blocks[nblocks] = b;
		nblocks++;
	}

	block_iter_fini(&b_iter);

	UT_ASSERT(nblocks > 0);

	size_t file_start = file_blocks[0].offset;
	size_t file_end = file_blocks[nblocks - 1].offset +
		file_blocks[nblocks - 1].length;

	struct block bb;
	struct sysfs_iter iter;
	sysfs_iter_init(&iter, fd, "badblocks", "%lu %lu");

	while (sysfs_iter_next(&iter,
		&bb.offset, &bb.length) > 0) {

		bb.offset *= sector_size;
		bb.length *= sector_size;

		if (block_is_inside_space(&bb, file_blocks, nblocks)) {
			cb(&bb, arg);
/*			block_normalize(&bb, file_start, file_end);
			printf("badblock: %lu %lu\n", bb.offset, bb.length);

			void *buf = calloc(1, 4096);

			write(fd, buf, 4096);*/
		}
	}

	sysfs_iter_fini(&iter);

out:
	return ret;
}

static int
poison_handler(void *addr, size_t len)
{
//	UT_ASSERTeq(len, (1 << 12));

	printf("poison: %p %lu\n", addr, len);

	return 0;
}

static int
test_section(void *addr, size_t len)
{
	if (PMEM_POISON_HANDLE(addr, len) != 0) {
		pmem_poison_consume(poison_handler);
		return 0;
	}

	int sum = 0;
	for (int i = 0; i < len; ++i) {
		sum += *(char *)((char *)addr + 0);
	}
	printf("sum: %d\n", sum);

	PMEM_POISON_END();

	return -1;
}

static struct ndctl_region *
region_from_fd(struct ndctl_ctx *ctx)
{
	struct ndctl_bus *bus;
	struct ndctl_region *region;
	struct ndctl_namespace *ndns;
	struct ndctl_dax *dax;
	ndctl_bus_foreach(ctx, bus) {
		printf("%s %d %d\n", ndctl_bus_get_devname(bus),
		ndctl_bus_get_minor(bus),
		ndctl_bus_get_major(bus));
		ndctl_region_foreach(bus, region) {
			printf("%s %d\n", ndctl_region_get_devname(region),
				ndctl_region_get_id(region));

			ndctl_namespace_foreach(region, ndns) {
				if (strcmp("namespace5.0",
					ndctl_namespace_get_devname(ndns)) == 0)
					return region;

				printf("ns: %s %d\n",
					ndctl_namespace_get_devname(ndns),
					ndctl_namespace_get_id(ndns));
			}
			ndctl_dax_foreach(region, dax) {
				printf("devdax: %s\n",
				ndctl_dax_get_devname(dax));
				if (strcmp("dax6.0", ndctl_dax_get_devname(dax)) == 0)
					return region;
			}
		}
	}

	return NULL;
}

static struct ndctl_cmd *
badblock_clear_one(struct ndctl_ctx *ctx, struct ndctl_region *r, size_t addr, size_t len)
{
	size_t base = ndctl_region_get_resource(r);
	addr += base;

	struct ndctl_cmd *cmd_ars_cap =
		ndctl_bus_cmd_new_ars_cap(ndctl_region_get_bus(r), addr, len);

	int ret = ndctl_cmd_submit(cmd_ars_cap);
	UT_ASSERTeq(ret, 0);

	struct ndctl_cmd *cmd_ars_start =
		ndctl_bus_cmd_new_ars_start(cmd_ars_cap, ND_ARS_PERSISTENT);

	ret = ndctl_cmd_submit(cmd_ars_start);
	UT_ASSERTeq(ret, 0);

	struct ndctl_cmd *cmd_ars_status;
	do {
		cmd_ars_status = ndctl_bus_cmd_new_ars_status(cmd_ars_cap);
		ret = ndctl_cmd_submit(cmd_ars_status);
		UT_ASSERTeq(ret, 0);
	} while (ndctl_cmd_ars_in_progress(cmd_ars_status));

	struct ndctl_range range;
	ndctl_cmd_ars_cap_get_range(cmd_ars_cap, &range);

	struct ndctl_cmd *cmd_clear_error =
		ndctl_bus_cmd_new_clear_error(range.address, range.length, cmd_ars_cap);

	ret = ndctl_cmd_submit(cmd_clear_error);
	UT_ASSERTeq(ret, 0);

	size_t cleared = ndctl_cmd_clear_error_get_cleared(cmd_clear_error);
	UT_ASSERTeq(cleared, len);

	ndctl_cmd_unref(cmd_ars_cap);
	ndctl_cmd_unref(cmd_ars_start);
	ndctl_cmd_unref(cmd_ars_status);
	ndctl_cmd_unref(cmd_clear_error);

	return NULL;
}

static void
badblock_clear(struct block *badblock, void *arg)
{
/*	struct ndctl_ctx *ctx;
	int ret = ndctl_new(&ctx);
	if (ret != 0)
		return;

	struct ndctl_region *r = region_from_fd(ctx);

	badblock_clear_one(ctx, r, badblock->offset, badblock->length);

	ndctl_unref(ctx);*/

	printf("badblock: %lu %lu\n", badblock->offset, badblock->length);
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "pmem_poison");

	if (argc != 2)
		UT_FATAL("usage: %s path", argv[0]);

	char *path = argv[1];

	int fd = open(path, O_RDWR);
	badblocks_foreach(fd, badblock_clear, NULL);
	close(fd);

	size_t len;
	int is_pmem;
	void *addr = pmem_map_file(path, 0, 0, 0, &len, &is_pmem);
	UT_ASSERTne(addr, NULL);

	pmem_poison_register_handler();

	test_section(addr, len);

	pmem_unmap(addr, len);

	DONE(NULL);
}
