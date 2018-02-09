#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <libpmemobj.h>

static int
ulong_cmp(const void *v1, const void *v2)
{
	unsigned long lv1 = *(const unsigned long *)v1;
	unsigned long lv2 = *(const unsigned long *)v2;
	if (lv1 > lv2)
		return 1;
	if (lv1 < lv2)
		return -1;
	return 0;
}

int main(int argc, char *argv[])
{
	struct timespec start, end;
	if (argc < 2) {
		printf("args: path\n");
		exit(1);
	}

	long j = 1;
	pmemobj_ctl_set(NULL, "prefault.at_create", &j);
	pmemobj_ctl_set(NULL, "prefault.at_open", &j);

	PMEMobjpool *pop = pmemobj_create(argv[1], "bla", (1ULL<<30) * 7, 0655);
//	PMEMobjpool *pop = pmemobj_open(argv[1], "bla");
	if (!pop) {
		perror("create");
		abort();
	}

	PMEMoid root = pmemobj_root(pop, 150<<20);
	if (OID_IS_NULL(root)) {
		perror("root");
		abort();
	}


	/* warmup tx */
	TX_BEGIN(pop) {
		pmemobj_tx_alloc(1, 1);
	} TX_ONABORT {
		perror("tx abort");
		abort();
	} TX_END

#define SAMPLES 30
	printf("nops,ops");
	for (int j = 0; j < SAMPLES; ++j)
		printf(",smpl%d", j);
	printf(",median\n");

	size_t ops = 1000;
	size_t nops;

	for (nops = 1; nops < 30; ++nops) {
		printf("%4lu, %7lu", nops, ops);
		unsigned long samples[SAMPLES];
		for (int j = 0; j < SAMPLES; ++j) {
			if (clock_gettime(CLOCK_MONOTONIC, &start))
				abort();

			for (size_t o = 0; o < ops; ++o) {
			TX_BEGIN(pop) {
					for (size_t i = 0; i < nops; ++i) {
						pmemobj_tx_alloc(1, 0);
					}
				} TX_ONABORT {
					perror("tx abort");
					abort();
				} TX_END
			}

			if (clock_gettime(CLOCK_MONOTONIC, &end))
				abort();

			unsigned long nsecs = (end.tv_sec - start.tv_sec) * (1 << 30) +
						end.tv_nsec - start.tv_nsec;

			printf(",%5lu", nsecs / ops);
			samples[j] = nsecs / ops;
		}
		qsort(samples, SAMPLES, sizeof(samples[0]), ulong_cmp);

		printf(",%5lu\n", samples[SAMPLES/2]);
	}

	pmemobj_close(pop);

	return 0;
}
