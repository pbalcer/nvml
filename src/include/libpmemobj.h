/*
 * Copyright (c) 2014, Intel Corporation
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

/*
 * libpmemobj.h -- definitions of libpmemobj entry points
 *
 * This library provides support for programming with persistent memory (pmem).
 *
 * libpmemobj provides a pmem-resident transactional object store.
 *
 * See libpmemobj(3) for details.
 */

#ifndef	LIBPMEMOBJ_H
#define	LIBPMEMOBJ_H 1

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/*
 * opaque type internal to libpmemobj
 */
typedef struct pmemobjpool PMEMobjpool;

/*
 * PMEMOBJ_MAJOR_VERSION and PMEMOBJ_MINOR_VERSION provide the current version
 * of the libpmemobj API as provided by this header file.  Applications can
 * verify that the version available at run-time is compatible with the version
 * used at compile-time by passing these defines to pmemobj_check_version().
 */
#define	PMEMOBJ_MAJOR_VERSION 1
#define	PMEMOBJ_MINOR_VERSION 0
const char *pmemobj_check_version(
		unsigned major_required,
		unsigned minor_required);

#define	PMEMOBJ_MIN_POOL ((size_t)(1 << 21))	/* min pool size: 2MB */
#define	PMEMOBJ_LAYOUT_MAX ((size_t)1024)

PMEMobjpool *pmemobj_open(const char *path, const char *layout);
PMEMobjpool *pmemobj_create(const char *path, const char *layout,
		size_t poolsize, mode_t mode);
void pmemobj_close(PMEMobjpool *pop);
int pmemobj_check(const char *path, const char *layout);

/*
 * Passing NULL to pmemobj_set_funcs() tells libpmemobj to continue to use the
 * default for that function.  The replacement functions must not make calls
 * back into libpmemobj.
 */
void pmemobj_set_funcs(
		void *(*malloc_func)(size_t size),
		void (*free_func)(void *ptr));

#define POBJ_ID_MAGIC 0x12345678

#define POBJ(type)\
union {\
type *__type;\
struct pobj_id pobj;\
}

struct pobj_id {
	uint64_t offset;
};

struct transaction_context;

enum tx_state {
	TX_STATE_UNKNOWN,
	TX_STATE_FAILED,
	TX_STATE_SUCCESS,
	TX_STATE_ABORTED,

	MAX_TX_STATE
};

typedef enum tx_state (*tx_func)(struct transaction_context *ctx, void *root);

void *pmemobj_init_root(PMEMobjpool *p, size_t size);
enum tx_state pmemobj_tx_exec(PMEMobjpool *p, tx_func tx);
int pmemobj_set(struct transaction_context *ctx, void *dst, void *src,
	size_t size);
void *pmemobj_direct(struct transaction_context *ctx, struct pobj_id pobj);

int pmemobj_alloc(struct transaction_context *ctx, struct pobj_id *obj,
	size_t size);
int pmemobj_free(struct transaction_context *ctx, struct pobj_id *obj);

#define TX_EXEC(name, rname)\
enum tx_state name(struct transaction_context *__ctx, void *rname)\

#define POBJ_IS_NULL(obj) (obj.pobj.offset == 0)

#define POBJ_NEW(dest) ({\
if (pmemobj_alloc(__ctx, &dest.pobj, sizeof (*dest.__type)) != 1)\
return TX_STATE_ABORTED;\
})

#define POBJ_SET(dest, value) ({\
typeof (dest) __v = value;\
if (pmemobj_set(__ctx, &dest, &__v, sizeof (__v)) != 1)\
return TX_STATE_ABORTED;\
})

#define POBJ_DELETE(dest) ({\
if (pmemobj_free(__ctx, &dest.pobj) != 1)\
return TX_STATE_ABORTED;\
})

#define D(obj) ((typeof (obj.__type))pmemobj_direct(__ctx, obj.pobj))

#ifdef __cplusplus
}
#endif
#endif	/* libpmemobj.h */
