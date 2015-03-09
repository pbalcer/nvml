/*
 * Copyright (c) 2014-2015, Intel Corporation
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
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY LOG OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * obj.c -- transactional object store implementation
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <uuid/uuid.h>
#include <time.h>
#include <endian.h>
#include <stdbool.h>
#include <stddef.h>

#include "libpmem.h"
#include "libpmemobj.h"

#include "util.h"
#include "out.h"
#include "obj.h"

#include "pmalloc.h"

/*
 * pmemobj_map_common -- (internal) map a transactional memory pool
 *
 * This routine does all the work, but takes a rdonly flag so internal
 * calls can map a read-only pool if required.
 *
 * If empty flag is set, the file is assumed to be a new memory pool, and
 * new pool header is created.  Otherwise, a valid header must exist.
 */
static PMEMobjpool *
pmemobj_map_common(int fd, const char *layout, size_t poolsize, int rdonly,
		int empty)
{
	LOG(3, "fd %d layout %s poolsize %zu rdonly %d empty %d",
			fd, layout, poolsize, rdonly, empty);

	void *addr;
	if ((addr = util_map(fd, poolsize, rdonly)) == NULL) {
		(void) close(fd);
		return NULL;	/* util_map() set errno, called LOG */
	}

	(void) close(fd);

	/* check if the mapped region is located in persistent memory */
	int is_pmem = pmem_is_pmem(addr, poolsize);

	/* opaque info lives at the beginning of mapped memory pool */
	struct pmemobjpool *pop = addr;
	struct pool_hdr hdr;

	if (!empty) {
		memcpy(&hdr, &pop->hdr, sizeof (hdr));

		if (!util_convert_hdr(&hdr)) {
			errno = EINVAL;
			goto err;
		}

		/*
		 * valid header found
		 */
		if (strncmp(hdr.signature, OBJ_HDR_SIG, POOL_HDR_SIG_LEN)) {
			LOG(1, "wrong pool type: \"%s\"", hdr.signature);
			errno = EINVAL;
			goto err;
		}

		if (hdr.major != OBJ_FORMAT_MAJOR) {
			LOG(1, "obj pool version %d (library expects %d)",
				hdr.major, OBJ_FORMAT_MAJOR);
			errno = EINVAL;
			goto err;
		}

		if (layout &&
		    strncmp(pop->layout, layout, PMEMOBJ_LAYOUT_MAX)) {
			LOG(1, "wrong layout (\"%s\"), "
				"pool created with layout \"%s\"",
				layout, pop->layout);
			errno = EINVAL;
			goto err;
		}

		/* XXX check rest of required metadata */

		int retval = util_feature_check(&hdr, OBJ_FORMAT_INCOMPAT,
							OBJ_FORMAT_RO_COMPAT,
							OBJ_FORMAT_COMPAT);
		if (retval < 0)
		    goto err;
		else if (retval == 0)
		    rdonly = 1;
	} else {
		LOG(3, "creating new transactional memory pool");

		ASSERTeq(rdonly, 0);

		struct pool_hdr *hdrp = &pop->hdr;
		/* check if the pool header is all zeros */
		if (!util_is_zeroed(hdrp, sizeof (*hdrp))) {
			LOG(1, "Non-empty file detected");
			errno = EINVAL;
			goto err;
		}

		/* create the required metadata first */
		if (layout) {
			if (strlen(layout) >= PMEMOBJ_LAYOUT_MAX) {
				LOG(1, "Layout too long");
				errno = EINVAL;
				goto err;
			}
			strncpy(pop->layout, layout, PMEMOBJ_LAYOUT_MAX - 1);
			pmem_msync(pop->layout, sizeof (pop->layout));
		}

		/* XXX create remaining metadata */

		strncpy(hdrp->signature, OBJ_HDR_SIG, POOL_HDR_SIG_LEN);
		hdrp->major = htole32(OBJ_FORMAT_MAJOR);
		hdrp->compat_features = htole32(OBJ_FORMAT_COMPAT);
		hdrp->incompat_features = htole32(OBJ_FORMAT_INCOMPAT);
		hdrp->ro_compat_features = htole32(OBJ_FORMAT_RO_COMPAT);
		uuid_generate(hdrp->uuid);
		hdrp->crtime = htole64((uint64_t)time(NULL));
		util_checksum(hdrp, sizeof (*hdrp), &hdrp->checksum, 1);

		/* store pool's header */
		pmem_msync(hdrp, sizeof (*hdrp));

		pop->root_offset = 0;
		pmem_msync(&pop->root_offset, sizeof (pop->root_offset));
	}

	/*
	 * Use some of the memory pool area for run-time info.  This
	 * run-time state is never loaded from the file, it is always
	 * created here, so no need to worry about byte-order.
	 */
	pop->addr = addr;
	pop->size = poolsize;
	pop->rdonly = rdonly;
	pop->is_pmem = is_pmem;

	/* XXX the rest of run-time info */

	pop->pmp = pool_open(&(pop->heap),
		poolsize - offsetof(struct pmemobjpool, heap), 0);

	if (pop->pmp == NULL) {
		goto err;
	}

	/*
	 * If possible, turn off all permissions on the pool header page.
	 *
	 * The prototype PMFS doesn't allow this when large pages are in
	 * use. It is not considered an error if this fails.
	 */
	util_range_none(addr, sizeof (struct pool_hdr));

	LOG(3, "pop %p", pop);
	return pop;

err:
	LOG(4, "error clean up");
	int oerrno = errno;
	util_unmap(addr, poolsize);
	errno = oerrno;
	return NULL;
}

/*
 * pmemobj_create -- create a transactional memory pool
 */
PMEMobjpool *
pmemobj_create(const char *path, const char *layout, size_t poolsize,
		mode_t mode)
{
	LOG(3, "path %s layout %s poolsize %zu mode %d",
			path, layout, poolsize, mode);

	int fd;
	if (poolsize != 0) {
		/* create a new memory pool file */
		fd = util_pool_create(path, poolsize, PMEMOBJ_MIN_POOL, mode);
	} else {
		/* open an existing file */
		fd = util_pool_open(path, &poolsize, PMEMOBJ_MIN_POOL);
	}
	if (fd == -1)
		return NULL;	/* errno set by util_pool_create/open() */

	return pmemobj_map_common(fd, layout, poolsize, 0, 1);
}

/*
 * pmemobj_open -- open a transactional memory pool
 */
PMEMobjpool *
pmemobj_open(const char *path, const char *layout)
{
	LOG(3, "path %s layout %s", path, layout);

	size_t poolsize = 0;
	int fd;

	if ((fd = util_pool_open(path, &poolsize, PMEMOBJ_MIN_POOL)) == -1)
		return NULL;	/* errno set by util_pool_open() */

	return pmemobj_map_common(fd, layout, poolsize, 0, 0);
}

/*
 * pmemobj_close -- close a transactional memory pool
 */
void
pmemobj_close(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	pool_close(pop->pmp);

	util_unmap(pop->addr, pop->size);
}

/*
 * pmemobj_check -- transactional memory pool consistency check
 */
int
pmemobj_check(const char *path, const char *layout)
{
	LOG(3, "path %s layout %s", path, layout);

	size_t poolsize = 0;
	int fd;

	if ((fd = util_pool_open(path, &poolsize, PMEMOBJ_MIN_POOL)) == -1)
		return -1;	/* errno set by util_pool_open() */

	/* map the pool read-only */
	PMEMobjpool *pop = pmemobj_map_common(fd, layout, poolsize, 1, 0);

	if (pop == NULL)
		return -1;	/* errno set by pmemobj_map_common() */

	int consistent = 1;

	/* XXX validate metadata */

	pool_check(&(pop->heap),
		poolsize - offsetof(struct pmemobjpool, heap), 0);

	pmemobj_close(pop);

	if (consistent)
		LOG(4, "pool consistency check OK");

	return consistent;
}

struct transaction_context {
	PMEMobjpool *pool;
	struct pmemobj_tx *dtx;
	int running;
	int n_txop;
};

void *pmemobj_init_root(PMEMobjpool *p, size_t size)
{
	if (p->root_offset == 0) {
		pmalloc(p->pmp, &p->root_offset, size);
	}

	return pdirect(p->pmp, p->root_offset);
}

void
tx_abort(struct transaction_context *__ctx)
{
	/* XXX */
}

void
tx_commit(struct transaction_context *__ctx)
{
	__ctx->dtx->committed = 1;
	pmem_msync(__ctx->dtx, sizeof (*__ctx->dtx));

	for (int i = MAX_TXOPS; i >= 0; --i) {
		if (POBJ_IS_NULL(__ctx->dtx->txop[i]))
			continue;

		struct pmemobj_txop *txop = D(__ctx->dtx->txop[i]);

		LOG(0, "Commiting txop: %lu %lu %lu %lu",
			txop->type, txop->addr, txop->data, txop->len);
		if (txop->type == TXOP_TYPE_FREE) {
			pfree(__ctx->pool->pmp, (uint64_t *)(txop->addr +
				(uint64_t)&(__ctx->pool->heap)));
		} else if (txop->type == TXOP_TYPE_SET) {
			pfree(__ctx->pool->pmp, &txop->data);
		}		

		pfree(__ctx->pool->pmp, &__ctx->dtx->txop[i].pobj.offset);
	}
}

struct transaction_context *
pmemobj_tx_exec_init(PMEMobjpool *p)
{
	/* this HAS to be called __ctx */
	struct transaction_context *__ctx = Malloc(sizeof (*__ctx));
	if (__ctx == NULL)
		return NULL;

	__ctx->pool = p;

	if (POBJ_IS_NULL(p->tx)) {
		pmemobj_alloc(__ctx, &p->tx.pobj, sizeof (*p->tx.__type));
		__ctx->dtx = D(p->tx);
	} else {
		/* XXX nested transactions... */
		Free(__ctx);
		return NULL;
	}

	__ctx->n_txop = 0;

	__ctx->running = 1;

	return __ctx;
}

enum tx_state
pmemobj_tx_exec_finish(struct transaction_context *__ctx, enum tx_state s)
{
	__ctx->running = 0;

	if (s == TX_STATE_SUCCESS) {
		tx_commit(__ctx);
	} else if (s == TX_STATE_ABORTED) {
		tx_abort(__ctx);
	}

	POBJ_DELETE(__ctx->pool->tx);

	Free(__ctx);

	return s;
}

void *
pmemobj_root(struct transaction_context *__ctx)
{
	return pdirect(__ctx->pool->pmp, __ctx->pool->root_offset);
}

int
pmemobj_set(struct transaction_context *__ctx, void *dst, void *src,
	size_t size)
{
	uint64_t offset = (uint64_t)dst - (uint64_t)&(__ctx->pool->heap);
	if (__ctx->running) {
		uint64_t *n_off = 
			&__ctx->dtx->txop[__ctx->n_txop++].pobj.offset;
		pmalloc(__ctx->pool->pmp, n_off, sizeof (struct pmemobj_txop));

		struct pmemobj_txop *dtxop =
			pdirect(__ctx->pool->pmp, *n_off);
		dtxop->type = TXOP_TYPE_SET;
		dtxop->addr = offset;
		dtxop->len = size;
		pmem_msync(dtxop, sizeof (*dtxop));
		pmalloc(__ctx->pool->pmp, &dtxop->data, size);
		struct pmemobj_txop *dd =
			pdirect(__ctx->pool->pmp, dtxop->data);
		memcpy(dd, dst, size);
	}

	memcpy(dst, src, size);
	pmem_msync(dst, size);

	return 1;
}

void *
pmemobj_direct(struct transaction_context *__ctx, struct pobj_id pobj)
{
	return pdirect(__ctx->pool->pmp, pobj.offset);
}

int
pmemobj_alloc(struct transaction_context *__ctx, struct pobj_id *obj,
	size_t size)
{
	if (__ctx->running) {
		uint64_t *n_off = 
			&__ctx->dtx->txop[__ctx->n_txop++].pobj.offset;
		pmalloc(__ctx->pool->pmp, n_off, sizeof (struct pmemobj_txop));

		struct pmemobj_txop *dtxop =
			pdirect(__ctx->pool->pmp, *n_off);
		dtxop->type = TXOP_TYPE_ALLOC;
		dtxop->addr = (uint64_t)&obj->offset -
			(uint64_t)&(__ctx->pool->heap);
		pmem_msync(dtxop, sizeof (*dtxop));
	}

	pmalloc(__ctx->pool->pmp, &obj->offset, size);

	return 1;
}

int
pmemobj_free(struct transaction_context *__ctx, struct pobj_id *obj)
{
	if (__ctx->running) {
		uint64_t *n_off = 
			&__ctx->dtx->txop[__ctx->n_txop++].pobj.offset;
		pmalloc(__ctx->pool->pmp, n_off, sizeof (struct pmemobj_txop));

		struct pmemobj_txop *dtxop =
			pdirect(__ctx->pool->pmp, *n_off);
		dtxop->type = TXOP_TYPE_FREE;
		dtxop->addr = (uint64_t)&obj->offset -
			(uint64_t)&(__ctx->pool->heap);
		pmem_msync(dtxop, sizeof (*dtxop));
	} else {
		pfree(__ctx->pool->pmp, &obj->offset);
	}

	return 1;
}