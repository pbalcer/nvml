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

/*
 * backend_persistent.h -- internal definitions for persistent backend
 */

typedef void (*persist_func)(void *addr, size_t len);
typedef void *(*pmemcpy_func)(void *dest, void *src, size_t len);
typedef void *(*pmemset_func)(void *dest, int c, size_t len);

#define	PERSISTENT_BACKEND_MAJOR 1
#define	PERSISTENT_BACKEND_MINOR 0

#define	MAX_INFO_SLOT 1024

/*
 * This implementation stores chunk indexes on a 16bit unsigned integer,
 * hence the limit, it's still plenty.
 */
#define	MAX_CHUNK 65535L
#define	CHUNKSIZE (1024 * 256)
#define	POOL_SIGNATURE_LEN 16
#define	POOL_SIGNATURE "MEMORY_POOL_HDR\0"
#define	CHUNK_HEADER_MAGIC 0xC3F0
#define	ZONE_MIN_SIZE (32 * (CHUNKSIZE))
#define	INFO_SLOT_DATA_SIZE 28

#define	MAX_CHUNK_LOCKS 1024

enum pool_flag {
	POOL_FLAG_CLEAR_RECYCLED	=	0x0001,
	POOL_FLAG_FILL_RECYCLED		=	0x0002,
	POOL_FLAG_RUNTIME_TREE		=	0x0004,
	POOL_FLAG_LAZY_LOAD		=	0x0008,
};

enum chunk_flag {
	CHUNK_FLAG_USED			=	0x0001,
	CHUNK_FLAG_ZEROED		=	0x0002
};

enum pool_state {
	POOL_STATE_UNKNOWN,
	POOL_STATE_OPEN,
	POOL_STATE_CLOSED,

	MAX_POOL_STATE
};

enum chunk_type {
	CHUNK_TYPE_UNKNOWN,
	CHUNK_TYPE_BASE,
	CHUNK_TYPE_RUN,
	CHUNK_TYPE_BITMAP,

	MAX_CHUNK_TYPE
};

enum info_slot_type {
	INFO_SLOT_TYPE_UNKNOWN,
	INFO_SLOT_TYPE_ALLOC,
	INFO_SLOT_TYPE_REALLOC,
	INFO_SLOT_TYPE_FREE,

	MAX_INFO_SLOT_TYPE
};

struct backend_pool_header {
	char signature[POOL_SIGNATURE_LEN];
	uint32_t flags; /* enum pool_flag */
	uint32_t state; /* enum pool_state */
	uint64_t major;
	uint64_t minor;
	uint64_t size;
	uint64_t chunk_size; /* size in bytes */
	uint64_t chunks_per_zone;
	char reserved[952];
	uint64_t checksum;
};

struct backend_info_slot {
	uint32_t type;  /* enum info_slot_type */
	char data[INFO_SLOT_DATA_SIZE]; /* 'union' of slot_* structures */
};

struct backend_info_slot_alloc {
	uint32_t type;
	uint32_t reserved;
	uint64_t destination_addr;
	uint64_t reserved_e[2];
};

struct backend_info_slot_realloc {
	uint32_t type;
	uint32_t reserved;
	uint64_t destination_addr;
	uint64_t old_alloc;
	uint64_t reserved_e;
};

struct backend_info_slot_free {
	uint32_t type;
	uint32_t reserved;
	uint64_t free_addr;
	uint64_t reserved_e[2];
};

struct backend_chunk_header {
	uint32_t magic; /* Must be CHUNK_HEADER_MAGIC */
	uint32_t type_specific;
	uint16_t type;
	uint16_t flags;
	uint32_t size_idx;
};

struct backend_chunk {
	char data[CHUNKSIZE];
};

struct backend_zone {
	struct backend_pool_header backup_header;
	struct backend_chunk_header chunk_header[MAX_CHUNK];
	struct backend_chunk chunk_data[MAX_CHUNK];
};

struct backend_pool {
	struct backend_pool_header primary_header;
	struct backend_info_slot info_slot[MAX_INFO_SLOT];
	struct backend_zone zone[];
};

struct backend_persistent {
	struct backend super;
	struct backend_pool *pool;
	size_t pool_size; /* size in bytes */
	int max_zone;
	int is_pmem;
	int zones_exhausted; /* number of zones already processed */
	persist_func persist;
	pmemcpy_func pmemcpy;
	pmemset_func pmemset;
	pthread_mutex_t chunk_locks[MAX_CHUNK_LOCKS];
};

struct backend *backend_persistent_open(void *ptr, size_t size);
void backend_persistent_close(struct backend *backend);
bool backend_persistent_consistency_check(void *ptr, size_t size);

void persistent_set_alloc_ptr(struct arena *arena, uint64_t *ptr,
	uint64_t value);
void persistent_fill_buckets(struct pmalloc_pool *pool);
void persistent_bucket_classes(struct pmalloc_pool *pool);
void persistent_init_bucket_obj(struct bucket *bucket,
	struct bucket_object *obj);
bool persistent_set_bucket_obj_state(struct bucket *bucket,
	struct bucket_object *obj, enum bucket_obj_state state);
bool persistent_locate_bucket_obj(struct pmalloc_pool *pool,
	struct bucket_object *obj, uint64_t data_offset);
void *persistent_get_direct(struct pmalloc_pool *pool, uint64_t ptr);
void persistent_copy_content(struct pmalloc_pool *pool,
	struct bucket_object *dest, struct bucket_object *src);
void persistent_set_guard(struct arena *arena, enum guard_type type,
	uint64_t *ptr);
void persistent_clear_guard(struct arena *arena);
