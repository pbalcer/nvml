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

#include <stdlib.h>
#include <string.h>
#include <libpmemobj.h>
#include <stdio.h>

POBJ_LAYOUT_BEGIN(bench);
POBJ_LAYOUT_ROOT(bench, struct bench);
POBJ_LAYOUT_TOID(bench, struct obj);
POBJ_LAYOUT_END(bench);

#define ALLOCS 1000000

struct bench {
	TOID(struct obj) objs[ALLOCS];
};

struct obj {
	uint64_t len;
};

int
main(int argc, char *argv[])
{
	//uint64_t count = atoll(argv[2]);
	uint64_t size = atoll(argv[2]);
	size_t psize = size * ALLOCS * 3;
	PMEMobjpool *pop = pmemobj_create(argv[1], POBJ_LAYOUT_NAME(bench),
			psize, 0666);
	if (pop == NULL)
		return 0;

	TOID(struct bench) b = POBJ_ROOT(pop, struct bench);
	struct timespec tstart={0,0}, tend={0,0};
	clock_gettime(CLOCK_MONOTONIC, &tstart);
	for (int i = 0; i < ALLOCS; ++i) {
		POBJ_ZNEW(pop, &D_RW(b)->objs[i], struct obj);
	}
	clock_gettime(CLOCK_MONOTONIC, &tend);
	printf("insert %.5fs\n",
		((double)tend.tv_sec + 1.0e-9*tend.tv_nsec) -
		((double)tstart.tv_sec + 1.0e-9*tstart.tv_nsec));
	return 0;
}

