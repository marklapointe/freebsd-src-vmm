/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2025 CloudBSD Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#ifndef _NETGRAPH_NG_PPPOE_LB_H_
#define	_NETGRAPH_NG_PPPOE_LB_H_

#ifndef _KERNEL
#include <stdint.h>
#else
#include <sys/types.h>
#include <netgraph/netgraph.h>
#endif

/* Node type name */
#define NG_PPPOE_LB_NODE_TYPE		"pppoe_lb"

/* Cookie for control messages */
#define NGM_PPPOE_LB_COOKIE		1089893073

/* Hook names */
#define NG_PPPOE_LB_HOOK_ETHER		"ether"
#define NG_PPPOE_LB_HOOK_WORKER_BASE	"worker"

/* Control messages */
enum {
	NGM_PPPOE_LB_ADD_WORKER = 1,
	NGM_PPPOE_LB_REMOVE_WORKER,
	NGM_PPPOE_LB_SET_CONFIG,
	NGM_PPPOE_LB_GET_STATS,
	NGM_PPPOE_LB_GET_MAP,
};

/* Load balancing algorithms */
#define NG_PPPOE_LB_ALGO_ROUND_ROBIN	0
#define NG_PPPOE_LB_ALGO_HASH		1
#define NG_PPPOE_LB_ALGO_LEAST_LOADED	2

/* Configuration structure for NGM_PPPOE_LB_SET_CONFIG */
struct ng_pppoe_lb_config {
	uint32_t	algorithm;	/* Load balancing algorithm */
	uint32_t	max_workers;	/* Maximum workers (0=unlimited) */
	uint32_t	debug_level;	/* Debug level (0=none, 1=verbose, 2=very verbose) */
};

/* Statistics structure for NGM_PPPOE_LB_GET_STATS */
struct ng_pppoe_lb_stats {
	uint64_t	packets_in;		/* Total packets received */
	uint64_t	packets_out;		/* Total packets distributed */
	uint64_t	sessions_created;	/* Total sessions created */
	uint64_t	sessions_destroyed;	/* Total sessions destroyed */
	uint32_t	num_workers;		/* Current number of workers */
	uint32_t	num_workers_active;	/* Number of active workers */
	uint32_t	algorithm;		/* Current algorithm */
	uint32_t	map_count;		/* Current session map entries */
};

/* Session map entry for NGM_PPPOE_LB_GET_MAP */
struct ng_pppoe_lb_map_entry {
	uint16_t	session_id;		/* PPPoE session ID */
	uint16_t	worker_index;		/* Worker node index */
	uint32_t	last_activity;		/* Last activity timestamp */
};

/* Message structure for NGM_PPPOE_LB_GET_MAP */
struct ng_pppoe_lb_map {
	uint32_t	count;			/* Number of entries */
	uint32_t	max_entries;		/* Maximum entries to return */
	struct ng_pppoe_lb_map_entry entries[];	/* Variable-length array */
};

/* Add worker message */
struct ng_pppoe_lb_add_worker {
	char		worker_name[64];	/* Worker node name */
};

/* Remove worker message */
struct ng_pppoe_lb_remove_worker {
	int		worker_index;		/* Worker index to remove */
};

/* Parse type macros for control messages */
#define NG_PPPOE_LB_ADD_WORKER_TYPE_INFO {		\
	  { "worker_name",	&ng_parse_string_type },	\
	  { NULL }						\
}

#define NG_PPPOE_LB_CONFIG_TYPE_INFO {		\
	  { "algorithm",	&ng_parse_uint32_type },	\
	  { "max_workers",	&ng_parse_uint32_type },	\
	  { "debug_level",	&ng_parse_uint32_type },	\
	  { NULL }						\
}

#define NG_PPPOE_LB_STATS_TYPE_INFO {		\
	  { "packets_in",	&ng_parse_uint64_type },	\
	  { "packets_out",	&ng_parse_uint64_type },	\
	  { "sessions_created",	&ng_parse_uint64_type },	\
	  { "sessions_destroyed",	&ng_parse_uint64_type },	\
	  { "num_workers",	&ng_parse_uint32_type },	\
	  { "num_workers_active",	&ng_parse_uint32_type },	\
	  { "algorithm",	&ng_parse_uint32_type },	\
	  { "map_count",	&ng_parse_uint32_type },	\
	  { NULL }						\
}

#define NG_PPPOE_LB_MAP_ENTRY_TYPE_INFO {	\
	  { "session_id",	&ng_parse_uint16_type },	\
	  { "worker_index",	&ng_parse_uint16_type },	\
	  { "last_activity",	&ng_parse_uint32_type },	\
	  { NULL }						\
}

#define NG_PPPOE_LB_MAP_TYPE_INFO {		\
	  { "count",	&ng_parse_uint32_type },	\
	  { "max_entries",	&ng_parse_uint32_type },	\
	  { "entries",	&ng_pppoe_lb_map_entry_array_type },	\
	  { NULL }						\
}

#endif /* !_NETGRAPH_NG_PPPOE_LB_H_ */
