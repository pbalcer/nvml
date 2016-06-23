/*
 * Copyright 2014-2016, Intel Corporation
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
 * ctl.c -- implementation of the interface for examining and modification of
 *	the library internal state
 */

#include <sys/param.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>

#include "libpmem.h"
#include "libpmemobj.h"

#include "util.h"
#include "out.h"
#include "lane.h"
#include "redo.h"
#include "memops.h"
#include "pmalloc.h"
#include "heap_layout.h"
#include "list.h"
#include "cuckoo.h"
#include "ctree.h"
#include "obj.h"
#include "sync.h"
#include "valgrind_internal.h"
#include "ctl.h"

#define CTL_STR(name) #name

#define CTL_NODE_END {NULL, NULL, NULL, NULL}

#define CTL_NODE(prefix, name)\
ctl_node_##prefix##_##name

/* Declaration of a new child node */
#define CTL_CHILD(prefix, name)\
{CTL_STR(name), NULL, NULL, (struct ctl_node *)CTL_NODE(prefix, name)}

#define CTL_READ_HANDLER(prefix, name)\
ctl_##prefix##_##name##_read

#define CTL_WRITE_HANDLER(prefix, name)\
ctl_##prefix##_##name##_write

/*
 * Declaration of a new read-only leaf. If used the corresponding read function
 * must be declared by CTL_READ_HANDLER or CTL_GEN_RO_STAT macros.
 */
#define CTL_LEAF_RO(prefix, name)\
{CTL_STR(name), CTL_READ_HANDLER(prefix, name), NULL, NULL}

/*
 * Declaration of a new write-only leaf. If used the corresponding write
 * function must be declared by CTL_WRITE_HANDLER macro.
 */
#define CTL_LEAF_WO(prefix, name)\
{CTL_STR(name), NULL, CTL_WRITE_HANDLER(prefix, name), NULL}

/*
 * Declaration of a new read-write leaf. If used both read and write function
 * must be declared by CTL_READ_HANDLER and CTL_WRITE_HANDLER macros.
 */
#define CTL_LEAF_RW(prefix, name)\
{CTL_STR(name), CTL_READ_HANDLER(prefix, name),\
	CTL_WRITE_HANDLER(prefix, name), NULL}

/*
 * If the CTL leaf node is simply a read-only statistics, this macro can be
 * used to declare the read function that returns the value by reference.
 */
#define CTL_GEN_RO_STAT(prefix, struct_prefix, stat, type)\
static int CTL_READ_HANDLER(prefix, stat)(PMEMobjpool *pop, void *arg)\
{ type *out = arg; *out = CTL_STAT_GET(struct_prefix.stat, type); return 0; }

#define CTL_GEN_WO_TRAP(prefix, struct_prefix, name)\
static int CTL_WRITE_HANDLER(prefix, name)(PMEMobjpool *pop, void *arg)\
{ pop->stats->debug.traps.struct_prefix.name = arg; return 0; }

typedef int (*node_callback)(PMEMobjpool *pop, void *arg);

/*
 * CTL Tree node structure, do not use directly. All the necessery functionality
 * is provided by the included macros.
 */
struct ctl_node {
	char *name;
	node_callback read_cb;
	node_callback write_cb;
	struct ctl_node *children;
};

CTL_GEN_RO_STAT(stats_heap, heap, allocated, size_t);
CTL_GEN_RO_STAT(stats_heap, heap, freed, size_t);
CTL_GEN_RO_STAT(stats_heap, heap, active_zones, size_t);

static const struct ctl_node CTL_NODE(stats, heap)[] = {
	CTL_LEAF_RO(stats_heap, allocated),
	CTL_LEAF_RO(stats_heap, freed),
	CTL_LEAF_RO(stats_heap, active_zones),
	CTL_NODE_END
};

static const struct ctl_node CTL_NODE(, stats)[] = {
	CTL_CHILD(stats, heap),
	CTL_NODE_END
};

static int CTL_READ_HANDLER(debug, test_rw)(PMEMobjpool *pop, void *arg)
{
	int *arg_rw = arg;
	*arg_rw = 0;

	return 0;
}

static int CTL_WRITE_HANDLER(debug, test_rw)(PMEMobjpool *pop, void *arg)
{
	int *arg_rw = arg;
	*arg_rw = 1;

	return 0;
}

static int CTL_WRITE_HANDLER(debug, test_wo)(PMEMobjpool *pop, void *arg)
{
	int *arg_wo = arg;
	*arg_wo = 1;

	return 0;
}

static int CTL_READ_HANDLER(debug, test_ro)(PMEMobjpool *pop, void *arg)
{
	int *arg_ro = arg;
	*arg_ro = 0;

	return 0;
}

CTL_GEN_WO_TRAP(debug_traps_allocator, allocator, after_existing_block_free);
CTL_GEN_WO_TRAP(debug_traps_allocator, allocator, after_new_block_prep);
CTL_GEN_WO_TRAP(debug_traps_allocator, allocator, before_ops_process);
CTL_GEN_WO_TRAP(debug_traps_allocator, allocator, after_ops_process);
CTL_GEN_WO_TRAP(debug_traps_allocator, allocator, after_run_degrade);

static const struct ctl_node CTL_NODE(debug_traps, allocator)[] = {
	CTL_LEAF_WO(debug_traps_allocator, after_existing_block_free),
	CTL_LEAF_WO(debug_traps_allocator, after_new_block_prep),
	CTL_LEAF_WO(debug_traps_allocator, before_ops_process),
	CTL_LEAF_WO(debug_traps_allocator, after_ops_process),
	CTL_LEAF_WO(debug_traps_allocator, after_run_degrade),

	CTL_NODE_END
};

static const struct ctl_node CTL_NODE(debug, traps)[] = {
	CTL_CHILD(debug_traps, allocator)
};

static const struct ctl_node CTL_NODE(, debug)[] = {
	CTL_LEAF_RO(debug, test_ro),
	CTL_LEAF_WO(debug, test_wo),
	CTL_LEAF_RW(debug, test_rw),
	CTL_CHILD(debug, traps),
	CTL_NODE_END
};

/*
 * This is the top level node of the ctl tree structure. Each node can contain
 * children and leaf nodes.
 *
 * Child nodes simply create a new path in the tree whereas child nodes are the
 * ones providing the read/write functionality by the means of callbacks.
 *
 * Each tree node must be NULL-terminated, CTL_NODE_END macro is provided for
 * convience.
 */
static const struct ctl_node ctl_root[] = {
	/*
	 * Debug features only relevant for testing the library and the ctl
	 * interface itself.
	 */
	CTL_CHILD(, debug),

	CTL_CHILD(, stats),
	CTL_NODE_END
};

/*
 * pmemobj_ctl -- parses the name and calls the appropriate methods from the ctl
 *	tree.
 */
int
pmemobj_ctl(PMEMobjpool *pop, const char *name, void *read_arg, void *write_arg)
{
	char *parse_str = strdup(name);

	char *sptr;
	char *node_name = strtok_r(parse_str, ".", &sptr);

	struct ctl_node *nodes = (struct ctl_node *)ctl_root;
	struct ctl_node *n = NULL;

	/*
	 * Go through the string and separate tokens that correspond to nodes
	 * in the main ctl tree.
	 */
	while (node_name != NULL) {
		for (n = &nodes[0]; n->name != NULL; ++n) {
			if (strcmp(n->name, node_name) == 0)
				break;
		}
		if (n->name == NULL) {
			errno = EINVAL;
			return -1;
		}
		nodes = n->children;
		node_name = strtok_r(NULL, ".", &sptr);
	};

	/*
	 * Discard invalid calls, this includes the ones that are mostly correct
	 * but include an extraneous arguments.
	 */
	if (n == NULL || (read_arg != NULL && n->read_cb == NULL) ||
		(write_arg != NULL && n->write_cb == NULL) ||
		(write_arg == NULL && read_arg == NULL)) {
		errno = EINVAL;
		return -1;
	}

	int result = 0;

	if (read_arg)
		result = n->read_cb(pop, read_arg);

	if (write_arg && result == 0)
		result = n->write_cb(pop, write_arg);

	return result;
}

/*
 * ctl_stats_new -- allocates and initalizes statistics data structures
 */
struct ctl_stats *
ctl_stats_new(void)
{
	return Zalloc(sizeof(struct ctl_stats));
}

/*
 * ctl_stats_delete -- deletes statistics
 */
void
ctl_stats_delete(struct ctl_stats *stats)
{
	Free(stats);
}
