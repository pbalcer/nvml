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
 * blk_poison.c -- unit test for pmemblk poison handling
 */

#include "unittest.h"

#define PAGESIZE (1<<12)

#define TEST_BSIZE PAGESIZE
#define TEST_LBA 0

int
main(int argc, char *argv[])
{
	START(argc, argv, "blk_rw");

	if (argc != 2)
		UT_FATAL("usage: %s file", argv[0]);

	const char *path = argv[1];

	PMEMblkpool *handle = pmemblk_create(path, TEST_BSIZE, 0,
					S_IWUSR | S_IRUSR);

	unsigned char *buf = MALLOC(TEST_BSIZE);
	memset(buf, 'a', TEST_BSIZE);

	int ret;
	errno = 0;

	/* allow metadata writes */
	ret = pmemblk_write(handle, buf, TEST_LBA);
	UT_ASSERTeq(ret, 0);
	UT_ASSERTeq(errno, 0);

	ret = madvise((char *)handle + (1<<23), (1<<23) + (1<<16), MADV_HWPOISON);
	UT_ASSERTeq(ret, 0);
	UT_ASSERTeq(errno, 0);

	ret = pmemblk_write(handle, buf, TEST_LBA);
	UT_ASSERTeq(ret, -1);
	UT_ASSERTeq(errno, EFAULT);

	errno = 0;

	ret = pmemblk_read(handle, buf, TEST_LBA);
	UT_ASSERTeq(ret, -1);
	UT_ASSERTeq(errno, EFAULT);

	errno = 0;

	ret = pmemblk_set_zero(handle, TEST_LBA);
	UT_ASSERTeq(ret, -1);
	UT_ASSERTeq(errno, EFAULT);
#if 0
	/* XXX: locking issues (arenap->map_locks[map_lock_num] is held) */
	errno = 0;

	ret = pmemblk_set_error(handle, TEST_LBA);
	UT_ASSERTeq(ret, -1);
	UT_ASSERTeq(errno, EFAULT);
#endif
	FREE(buf);
	pmemblk_close(handle);

	DONE(NULL);
}
