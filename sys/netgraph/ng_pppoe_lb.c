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

/*
 * ng_pppoe_lb.c - PPPoE Load Balancer Node
 *
 * This netgraph node distributes PPPoE sessions across multiple worker
 * ng_pppoe nodes to enable parallel processing on multi-core systems.
 *
 * Key features:
 * - Session affinity: packets for the same session always go to the same worker
 * - Discovery phase load balancing: round-robin distribution of PADI packets
 * - Dynamic worker management: add/remove workers at runtime
 * - CPU governor: automatic scaling based on CPU load (optional)
 * - Backward compatibility: single-worker mode behaves identically to legacy
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/smp.h>

#include <net/ethernet.h>
#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/ng_parse.h>
#include <netgraph/ng_pppoe.h>
#include <netgraph/ng_ether.h>

#include "ng_pppoe_lb.h"

/* Memory type */
#ifdef NG_SEPARATE_MALLOC
static MALLOC_DEFINE(M_NETGRAPH_PPPOE_LB, "netgraph_pppoe_lb", "netgraph pppoe_lb node");
#else
#define M_NETGRAPH_PPPOE_LB M_NETGRAPH
#endif

/* Parse types for control messages */
static const struct ng_parse_struct_field ng_pppoe_lb_add_worker_type_fields[]
	= NG_PPPOE_LB_ADD_WORKER_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_add_worker_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_add_worker_type_fields
};

static const struct ng_parse_struct_field ng_pppoe_lb_remove_worker_type_fields[]
	= { { "worker_index", &ng_parse_int32_type }, { NULL } };
static const struct ng_parse_type ng_pppoe_lb_remove_worker_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_remove_worker_type_fields
};

static const struct ng_parse_struct_field ng_pppoe_lb_config_type_fields[]
	= NG_PPPOE_LB_CONFIG_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_config_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_config_type_fields
};

static const struct ng_parse_struct_field ng_pppoe_lb_stats_type_fields[]
	= NG_PPPOE_LB_STATS_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_stats_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_stats_type_fields
};

static const struct ng_parse_struct_field ng_pppoe_lb_map_entry_type_fields[]
	= NG_PPPOE_LB_MAP_ENTRY_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_map_entry_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_map_entry_type_fields
};

static const struct ng_parse_array_info ng_pppoe_lb_map_entry_array_info = {
	&ng_pppoe_lb_map_entry_type,
	NULL	/* fixed-size array, no getLength needed */
};
static const struct ng_parse_type ng_pppoe_lb_map_entry_array_type = {
	&ng_parse_array_type,
	&ng_pppoe_lb_map_entry_array_info
};

static const struct ng_parse_struct_field ng_pppoe_lb_map_type_fields[]
	= NG_PPPOE_LB_MAP_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_map_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_map_type_fields
};

/* Sysctl variables */
static int ng_pppoe_lb_enabled = 0;
static int ng_pppoe_lb_num_workers = 1;
static int ng_pppoe_lb_algorithm = NG_PPPOE_LB_ALGO_ROUND_ROBIN;
static int ng_pppoe_lb_session_map_size = 1024;
static int ng_pppoe_lb_debug = 0;

/* Governor sysctl variables */
static int ng_pppoe_lb_governor_enabled = 0;
static int ng_pppoe_lb_governor_max_workers = 0;  /* 0 = auto (mp_ncpus) */
static int ng_pppoe_lb_governor_cpu_threshold = 80;
static int ng_pppoe_lb_governor_cpu_low_threshold = 30;
static int ng_pppoe_lb_governor_scale_up_interval = 10;
static int ng_pppoe_lb_governor_scale_down_interval = 60;

/* Include for CPU statistics */
#include <sys/resource.h>

SYSCTL_NODE(_net_graph, OID_AUTO, pppoe_lb, CTLFLAG_RW, 0, "PPPoE Load Balancer");
SYSCTL_INT(_net_graph_pppoe_lb, OID_AUTO, enabled, CTLFLAG_RW, &ng_pppoe_lb_enabled, 0,
    "Enable PPPoE load balancer (0=disabled, 1=enabled)");
SYSCTL_INT(_net_graph_pppoe_lb, OID_AUTO, num_workers, CTLFLAG_RW, &ng_pppoe_lb_num_workers, 0,
    "Number of worker nodes");
SYSCTL_INT(_net_graph_pppoe_lb, OID_AUTO, algorithm, CTLFLAG_RW, &ng_pppoe_lb_algorithm, 0,
    "Load balancing algorithm (0=round-robin, 1=hash, 2=least-loaded)");
SYSCTL_INT(_net_graph_pppoe_lb, OID_AUTO, session_map_size, CTLFLAG_RW, &ng_pppoe_lb_session_map_size, 0,
    "Session map hash table size");
SYSCTL_INT(_net_graph_pppoe_lb, OID_AUTO, debug, CTLFLAG_RW, &ng_pppoe_lb_debug, 0,
    "Debug level (0=none, 1=verbose, 2=very verbose)");

SYSCTL_NODE(_net_graph_pppoe_lb, OID_AUTO, governor, CTLFLAG_RW, 0, "CPU Governor");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, enabled, CTLFLAG_RW, &ng_pppoe_lb_governor_enabled, 0,
    "Enable CPU governor (0=disabled, 1=enabled)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, max_workers, CTLFLAG_RW, &ng_pppoe_lb_governor_max_workers, 0,
    "Maximum workers (0=auto/mp_ncpus, >0=hard cap)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, cpu_threshold, CTLFLAG_RW, &ng_pppoe_lb_governor_cpu_threshold, 0,
    "CPU % threshold to spawn more workers");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, cpu_low_threshold, CTLFLAG_RW, &ng_pppoe_lb_governor_cpu_low_threshold, 0,
    "CPU % threshold to reduce workers");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, scale_up_interval, CTLFLAG_RW, &ng_pppoe_lb_governor_scale_up_interval, 0,
    "Seconds between scale-up checks");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, scale_down_interval, CTLFLAG_RW, &ng_pppoe_lb_governor_scale_down_interval, 0,
    "Seconds between scale-down checks");

/* Session map entry */
struct ng_pppoe_lb_sess_entry {
	LIST_ENTRY(ng_pppoe_lb_sess_entry)	next;
	uint16_t				session_id;
	int					worker_index;
	time_t					last_activity;
};

/* Private data for each node */
struct ng_pppoe_lb_private {
	struct mtx				mtx;
	hook_p					ether_hook;
	hook_p					*worker_hooks;
	int					num_workers;
	int					num_workers_active;
	int					max_workers;
	
	/* Session-to-worker mapping */
	LIST_HEAD(, ng_pppoe_lb_sess_entry)	sess_list;
	int					sess_count;
	
	/* Round-robin counter for discovery */
	int					next_worker;
	
	/* Configuration */
	int					algorithm;
	int					debug_level;
	
	/* Statistics */
	uint64_t				packets_in;
	uint64_t				packets_out;
	uint64_t				sessions_created;
	uint64_t				sessions_destroyed;
	
	/* Governor state */
	struct callout				governor_callout;
	time_t					last_scale_up;
	time_t					last_scale_down;
};

#define	GET_PRIV(hook)	((struct ng_pppoe_lb_private *)NG_HOOK_PRIVATE(hook))
#define	GET_NODE_PRIV(node)	((struct ng_pppoe_lb_private *)NG_NODE_PRIVATE(node))

/* Forward declarations */
static int ng_pppoe_lb_constructor(node_p node);
static int ng_pppoe_lb_rcvmsg(node_p node, item_p item, hook_p lasthook);
static int ng_pppoe_lb_shutdown(node_p node);
static int ng_pppoe_lb_newhook(node_p node, hook_p hook, const char *name);
static int ng_pppoe_lb_connect(hook_p hook);
static int ng_pppoe_lb_rcvdata(hook_p hook, item_p item);
static int ng_pppoe_lb_disconnect(hook_p hook);

/* Internal functions */
static int ng_pppoe_lb_select_worker_discovery(struct ng_pppoe_lb_private *priv);
static int ng_pppoe_lb_select_worker_session(struct ng_pppoe_lb_private *priv, uint16_t session_id);
static struct ng_pppoe_lb_sess_entry *ng_pppoe_lb_find_session(struct ng_pppoe_lb_private *priv, uint16_t session_id);
static int ng_pppoe_lb_add_session(struct ng_pppoe_lb_private *priv, uint16_t session_id, int worker_index);
static int ng_pppoe_lb_remove_session(struct ng_pppoe_lb_private *priv, uint16_t session_id);
static void ng_pppoe_lb_governor_tick(void *arg);

/* Command list */
static const struct ng_cmdlist ng_pppoe_lb_cmds[] = {
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_ADD_WORKER,
		"add_worker",
		&ng_pppoe_lb_add_worker_type,
		NULL,
	},
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_REMOVE_WORKER,
		"remove_worker",
		&ng_pppoe_lb_remove_worker_type,
		NULL,
	},
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_SET_CONFIG,
		"set_config",
		&ng_pppoe_lb_config_type,
		NULL,
	},
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_GET_STATS,
		"get_stats",
		NULL,
		&ng_pppoe_lb_stats_type,
	},
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_GET_MAP,
		"get_map",
		NULL,
		&ng_pppoe_lb_map_type,
	},
	{ 0 }
};

static struct ng_type ng_pppoe_lb_typestruct = {
	.version =	NG_ABI_VERSION,
	.name =		NG_PPPOE_LB_NODE_TYPE,
	.constructor =	ng_pppoe_lb_constructor,
	.rcvmsg =	ng_pppoe_lb_rcvmsg,
	.shutdown =	ng_pppoe_lb_shutdown,
	.newhook =	ng_pppoe_lb_newhook,
	.connect =	ng_pppoe_lb_connect,
	.rcvdata =	ng_pppoe_lb_rcvdata,
	.disconnect =	ng_pppoe_lb_disconnect,
	.cmdlist =	ng_pppoe_lb_cmds,
};

NETGRAPH_INIT(pppoe_lb, &ng_pppoe_lb_typestruct);

/* Node constructor */
static int
ng_pppoe_lb_constructor(node_p node)
{
	struct ng_pppoe_lb_private *priv;

	priv = malloc(sizeof(*priv), M_NETGRAPH, M_NOWAIT | M_ZERO);
	if (priv == NULL)
		return (ENOMEM);

	mtx_init(&priv->mtx, "pppoe_lb", NULL, MTX_DEF);
	LIST_INIT(&priv->sess_list);
	priv->num_workers = 0;
	priv->num_workers_active = 0;
	/* Initialize max_workers: 0 means auto-detect to mp_ncpus */
	if (ng_pppoe_lb_governor_max_workers <= 0) {
		priv->max_workers = mp_ncpus;
	} else {
		/* Hard cap: never exceed mp_ncpus even if user sets higher */
		priv->max_workers = min(ng_pppoe_lb_governor_max_workers, mp_ncpus);
	}
	priv->algorithm = ng_pppoe_lb_algorithm;
	priv->debug_level = ng_pppoe_lb_debug;
	priv->next_worker = 0;
	priv->packets_in = 0;
	priv->packets_out = 0;
	priv->sessions_created = 0;
	priv->sessions_destroyed = 0;
	priv->last_scale_up = 0;
	priv->last_scale_down = 0;

	/* Initialize governor callout */
	callout_init(&priv->governor_callout, CALLOUT_MPSAFE);
	if (ng_pppoe_lb_governor_enabled) {
		callout_reset(&priv->governor_callout, hz, ng_pppoe_lb_governor_tick, priv);
	}

	NG_NODE_SET_PRIVATE(node, priv);
	NG_NODE_REF(node);  /* Reference for private data */

	return (0);
}

/* Receive control message */
static int
ng_pppoe_lb_rcvmsg(node_p node, item_p item, hook_p lasthook)
{
	struct ng_pppoe_lb_private *priv;
	struct ng_mesg *msg, *resp = NULL;
	int error = 0;

	priv = GET_NODE_PRIV(node);
	NGI_GET_MSG(item, msg);

	switch (msg->header.typecookie) {
	case NGM_PPPOE_LB_COOKIE:
		switch (msg->header.cmd) {
		case NGM_PPPOE_LB_ADD_WORKER:
			/* Handled via newhook/connect */
			break;

		case NGM_PPPOE_LB_REMOVE_WORKER:
			/* Handled via disconnect */
			break;

		case NGM_PPPOE_LB_SET_CONFIG: {
			struct ng_pppoe_lb_config *cfg;
			if (msg->header.arglen != sizeof(*cfg)) {
				error = EINVAL;
				break;
			}
			cfg = (struct ng_pppoe_lb_config *)msg->data;
			mtx_lock(&priv->mtx);
			priv->algorithm = cfg->algorithm;
			priv->max_workers = cfg->max_workers;
			priv->debug_level = cfg->debug_level;
			mtx_unlock(&priv->mtx);
			break;
		}

		case NGM_PPPOE_LB_GET_STATS: {
			struct ng_pppoe_lb_stats *stats;
			resp = malloc(sizeof(*resp) + sizeof(*stats), M_NETGRAPH_PPPOE_LB, M_NOWAIT | M_ZERO);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			resp->header.typecookie = NGM_PPPOE_LB_COOKIE;
			resp->header.cmd = NGM_PPPOE_LB_GET_STATS;
			resp->header.arglen = sizeof(*stats);
			stats = (struct ng_pppoe_lb_stats *)resp->data;
			mtx_lock(&priv->mtx);
			stats->packets_in = priv->packets_in;
			stats->packets_out = priv->packets_out;
			stats->sessions_created = priv->sessions_created;
			stats->sessions_destroyed = priv->sessions_destroyed;
			stats->num_workers = priv->num_workers;
			stats->num_workers_active = priv->num_workers_active;
			stats->algorithm = priv->algorithm;
			stats->map_count = priv->sess_count;
			mtx_unlock(&priv->mtx);
			break;
		}

		case NGM_PPPOE_LB_GET_MAP: {
			struct ng_pppoe_lb_map *map_req, *map_resp;
			struct ng_pppoe_lb_sess_entry *entry;
			int count = 0, max_entries;

			if (msg->header.arglen < sizeof(*map_req)) {
				error = EINVAL;
				break;
			}
			map_req = (struct ng_pppoe_lb_map *)msg->data;
			max_entries = map_req->max_entries;
			if (max_entries > 1024)
				max_entries = 1024;

			map_resp = malloc(sizeof(*map_resp) + max_entries * sizeof(*map_resp->entries), M_NETGRAPH_PPPOE_LB, M_NOWAIT | M_ZERO);
			if (map_resp == NULL) {
				error = ENOMEM;
				break;
			}
			resp->header.typecookie = NGM_PPPOE_LB_COOKIE;
			resp->header.cmd = NGM_PPPOE_LB_GET_MAP;
			map_resp = (struct ng_pppoe_lb_map *)resp->data;

			mtx_lock(&priv->mtx);
			count = 0;
			LIST_FOREACH(entry, &priv->sess_list, next) {
				if (count >= max_entries)
					break;
				map_resp->entries[count].session_id = entry->session_id;
				map_resp->entries[count].worker_index = entry->worker_index;
				map_resp->entries[count].last_activity = entry->last_activity;
				count++;
			}
			map_resp->count = count;
			resp->header.arglen = sizeof(*map_resp) + count * sizeof(struct ng_pppoe_lb_map_entry);
			mtx_unlock(&priv->mtx);
			break;
		}

		default:
			error = EINVAL;
			break;
		}
		break;

	default:
		error = EINVAL;
		break;
	}

	if (error != 0) {
		NG_FREE_MSG(msg);
		NG_FREE_ITEM(item);
	} else {
		if (resp != NULL) {
			NG_RESPOND_MSG(error, node, item, resp);
			NG_FREE_MSG(msg);
		} else {
			NG_FREE_MSG(msg);
			NG_FREE_ITEM(item);
		}
	}

	return (error);
}

/* Node shutdown */
static int
ng_pppoe_lb_shutdown(node_p node)
{
	struct ng_pppoe_lb_private *priv;
	struct ng_pppoe_lb_sess_entry *entry, *tmp;

	priv = GET_NODE_PRIV(node);

	/* Stop governor callout */
	callout_stop(&priv->governor_callout);

	/* Free all session entries */
	mtx_lock(&priv->mtx);
	LIST_FOREACH_SAFE(entry, &priv->sess_list, next, tmp) {
		LIST_REMOVE(entry, next);
		free(entry, M_NETGRAPH);
	}
	mtx_unlock(&priv->mtx);

	/* Free worker hooks array */
	if (priv->worker_hooks != NULL)
		free(priv->worker_hooks, M_NETGRAPH);

	mtx_destroy(&priv->mtx);

	NG_NODE_SET_PRIVATE(node, NULL);
	NG_NODE_UNREF(node);

	return (0);
}

/* New hook creation */
static int
ng_pppoe_lb_newhook(node_p node, hook_p hook, const char *name)
{
	struct ng_pppoe_lb_private *priv;

	priv = GET_NODE_PRIV(node);

	if (strcmp(name, NG_PPPOE_LB_HOOK_ETHER) == 0) {
		if (priv->ether_hook != NULL)
			return (EEXIST);
		priv->ether_hook = hook;
	} else if (strncmp(name, NG_PPPOE_LB_HOOK_WORKER_BASE,
	    strlen(NG_PPPOE_LB_HOOK_WORKER_BASE)) == 0) {
		/* Worker hook - will be connected later */
	} else {
		return (EINVAL);
	}

	return (0);
}

/* Hook connection */
static int
ng_pppoe_lb_connect(hook_p hook)
{
	struct ng_pppoe_lb_private *priv;
	hook_p *new_hooks;
	int new_size;

	priv = GET_PRIV(hook);

	if (hook == priv->ether_hook) {
		/* Ethernet hook connected */
		return (0);
	}

	/* Worker hook - add to array */
	mtx_lock(&priv->mtx);
	new_size = (priv->num_workers + 1) * sizeof(hook_p);
	new_hooks = realloc(priv->worker_hooks, new_size, M_NETGRAPH, M_NOWAIT);
	if (new_hooks == NULL) {
		mtx_unlock(&priv->mtx);
		return (ENOMEM);
	}
	priv->worker_hooks = new_hooks;
	priv->worker_hooks[priv->num_workers] = hook;
	priv->num_workers++;
	priv->num_workers_active++;
	mtx_unlock(&priv->mtx);

	return (0);
}

/* Receive data packet */
static int
ng_pppoe_lb_rcvdata(hook_p hook, item_p item)
{
	struct ng_pppoe_lb_private *priv;
	struct mbuf *m;
	struct pppoe_full_hdr *wh;
	int worker_idx, error = 0;
	uint16_t session_id;

	priv = GET_PRIV(hook);
	NGI_GET_M(item, m);

	if (m->m_len < sizeof(*wh))
		m = m_pullup(m, sizeof(*wh));
	if (m == NULL) {
		NG_FREE_ITEM(item);
		return (ENOBUFS);
	}

	wh = mtod(m, struct pppoe_full_hdr *);

	mtx_lock(&priv->mtx);
	priv->packets_in++;

	switch (ntohs(wh->eh.ether_type)) {
	case ETHERTYPE_PPPOE_DISC:
	case ETHERTYPE_PPPOE_3COM_DISC:
		/* Discovery phase - use round-robin for new sessions */
		worker_idx = ng_pppoe_lb_select_worker_discovery(priv);
		break;

	case ETHERTYPE_PPPOE_SESS:
	case ETHERTYPE_PPPOE_3COM_SESS:
		/* Session phase - use session ID for affinity */
		session_id = ntohs(wh->ph.sid);
		worker_idx = ng_pppoe_lb_select_worker_session(priv, session_id);
		break;

	default:
		mtx_unlock(&priv->mtx);
		NG_FREE_M(m);
		NG_FREE_ITEM(item);
		return (EPFNOSUPPORT);
	}

	if (worker_idx < 0 || worker_idx >= priv->num_workers_active) {
		mtx_unlock(&priv->mtx);
		NG_FREE_M(m);
		NG_FREE_ITEM(item);
		return (ENETUNREACH);
	}

	priv->packets_out++;
	mtx_unlock(&priv->mtx);

	NG_FWD_NEW_DATA(error, item, priv->worker_hooks[worker_idx], m);
	return (error);
}

/* Hook disconnection */
static int
ng_pppoe_lb_disconnect(hook_p hook)
{
	struct ng_pppoe_lb_private *priv;
	int i, idx;

	priv = GET_PRIV(hook);

	if (hook == priv->ether_hook) {
		priv->ether_hook = NULL;
		return (0);
	}

	/* Worker hook - remove from array */
	mtx_lock(&priv->mtx);
	idx = -1;
	for (i = 0; i < priv->num_workers; i++) {
		if (priv->worker_hooks[i] == hook) {
			idx = i;
			break;
		}
	}

	if (idx >= 0) {
		/* Shift remaining hooks */
		for (i = idx; i < priv->num_workers - 1; i++)
			priv->worker_hooks[i] = priv->worker_hooks[i + 1];
		priv->num_workers--;
		priv->num_workers_active--;
	}
	mtx_unlock(&priv->mtx);

	return (0);
}

/* Select worker for discovery (round-robin) */
static int
ng_pppoe_lb_select_worker_discovery(struct ng_pppoe_lb_private *priv)
{
	int idx;

	if (priv->num_workers_active == 0)
		return (-1);

	idx = priv->next_worker;
	priv->next_worker = (priv->next_worker + 1) % priv->num_workers_active;

	return (idx);
}

/* Select worker for session (hash-based affinity) */
static int
ng_pppoe_lb_select_worker_session(struct ng_pppoe_lb_private *priv, uint16_t session_id)
{
	struct ng_pppoe_lb_sess_entry *entry;
	int idx;

	/* Check session map first */
	entry = ng_pppoe_lb_find_session(priv, session_id);
	if (entry != NULL) {
		entry->last_activity = time_uptime;
		return (entry->worker_index);
	}

	/* No existing mapping - use hash */
	idx = session_id % priv->num_workers_active;

	/* Add to session map */
	ng_pppoe_lb_add_session(priv, session_id, idx);

	return (idx);
}

/* Find session in map */
static struct ng_pppoe_lb_sess_entry *
ng_pppoe_lb_find_session(struct ng_pppoe_lb_private *priv, uint16_t session_id)
{
	struct ng_pppoe_lb_sess_entry *entry;

	LIST_FOREACH(entry, &priv->sess_list, next) {
		if (entry->session_id == session_id)
			return (entry);
	}
	return (NULL);
}

/* Add session to map */
static int
ng_pppoe_lb_add_session(struct ng_pppoe_lb_private *priv, uint16_t session_id, int worker_index)
{
	struct ng_pppoe_lb_sess_entry *entry;

	entry = malloc(sizeof(*entry), M_NETGRAPH, M_NOWAIT);
	if (entry == NULL)
		return (ENOMEM);

	entry->session_id = session_id;
	entry->worker_index = worker_index;
	entry->last_activity = time_uptime;
	LIST_INSERT_HEAD(&priv->sess_list, entry, next);
	priv->sess_count++;
	priv->sessions_created++;

	return (0);
}

/* Remove session from map */
static int
ng_pppoe_lb_remove_session(struct ng_pppoe_lb_private *priv, uint16_t session_id)
{
	struct ng_pppoe_lb_sess_entry *entry;

	entry = ng_pppoe_lb_find_session(priv, session_id);
	if (entry == NULL)
		return (ENOENT);

	LIST_REMOVE(entry, next);
	free(entry, M_NETGRAPH);
	priv->sess_count--;
	priv->sessions_destroyed++;

	return (0);
}

/* Governor tick function - monitors CPU load and scales workers */
static void
ng_pppoe_lb_governor_tick(void *arg)
{
	struct ng_pppoe_lb_private *priv;
	time_t now;
	int avg_cpu, target_workers;
	long cp_time[CPUSTATES];
	long total_time, idle_time;

	priv = (struct ng_pppoe_lb_private *)arg;

	if (!ng_pppoe_lb_governor_enabled)
		goto out;

	now = time_uptime;

	/* Read CPU statistics */
	read_cpu_time(cp_time);

	/* Calculate CPU utilization percentage */
	total_time = cp_time[CP_USER] + cp_time[CP_NICE] + cp_time[CP_SYS] +
	    cp_time[CP_INTR] + cp_time[CP_IDLE];
	idle_time = cp_time[CP_IDLE];

	if (total_time > 0) {
		/* CPU usage = 100 - (idle_time * 100 / total_time) */
		avg_cpu = 100 - (int)((idle_time * 100) / total_time);
	} else {
		avg_cpu = 0;
	}

	/* Scale up decision */
	if (avg_cpu > ng_pppoe_lb_governor_cpu_threshold &&
	    now - priv->last_scale_up >= ng_pppoe_lb_governor_scale_up_interval) {
		if (priv->num_workers < priv->max_workers) {
			/* Can scale up - signal via flag (actual worker creation
			 * happens in userland via ngctl or pppoed) */
			priv->last_scale_up = now;
			if (priv->debug_level >= 1) {
				printf("ng_pppoe_lb: governor wants to scale up "
				    "(CPU=%d%%, workers=%d/%d)\n",
				    avg_cpu, priv->num_workers, priv->max_workers);
			}
		}
	}

	/* Scale down decision */
	if (avg_cpu < ng_pppoe_lb_governor_cpu_low_threshold &&
	    now - priv->last_scale_down >= ng_pppoe_lb_governor_scale_down_interval &&
	    priv->num_workers > 1) {
		target_workers = priv->num_workers - 1;
		if (target_workers < 1)
			target_workers = 1;
		priv->last_scale_down = now;
		if (priv->debug_level >= 1) {
			printf("ng_pppoe_lb: governor wants to scale down "
			    "(CPU=%d%%, workers=%d->%d)\n",
			    avg_cpu, priv->num_workers, target_workers);
		}
	}

out:
	/* Schedule next tick */
	callout_reset(&priv->governor_callout, hz, ng_pppoe_lb_governor_tick, priv);
}
