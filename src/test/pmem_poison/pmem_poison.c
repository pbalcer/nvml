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

static sigjmp_buf sigbus_jmpbuf;

static void
sigbus_handler(int signum, siginfo_t *info, void *ctx)
{
	if (info->si_code == BUS_MCEERR_AO || info->si_code == BUS_MCEERR_AR) {
		pmem_poison_produce(info->si_addr, info->si_addr_lsb);
		siglongjmp(sigbus_jmpbuf, 1);
	}
}

static uint64_t *addri;

static int
poison_handler(void *addr, size_t len)
{
	UT_ASSERTeq(len, (1 << 12));
	UT_ASSERTeq(addr, addri);

	return 0;
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "pmem_poison");

	if (argc != 2)
		UT_FATAL("usage: %s path", argv[0]);

	char *path = argv[1];

	size_t len;
	int is_pmem;
	void *addr = pmem_map_file(path, 0, 0, 0, &len, &is_pmem);
	UT_ASSERTne(addr, NULL);

	struct sigaction act;
	act.sa_sigaction = sigbus_handler;
	act.sa_flags = SA_SIGINFO;

	if (sigsetjmp(sigbus_jmpbuf, 0) != 0) {
		pmem_poison_consume(poison_handler);
	} else {
		int ret = sigaction(SIGBUS, &act, NULL);
		UT_ASSERTeq(ret, 0);

		madvise(addr, (1 << 12), MADV_HWPOISON);
		addri = addr;
		*addri = 5;

		/* unreachable */
		UT_ASSERT(0);
	}

	pmem_unmap(addr, len);

	DONE(NULL);
}
