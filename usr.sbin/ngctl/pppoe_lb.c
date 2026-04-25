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

#include <err.h>
#include <netgraph.h>
#include <netgraph/ng_pppoe_lb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ngctl.h"

static int PppoeLbShowCmd(int ac, char **av);
static int PppoeLbConfigCmd(int ac, char **av);
static int PppoeLbStatsCmd(int ac, char **av);
static int PppoeLbMapCmd(int ac, char **av);

const struct ngcmd pppoe_lb_show_cmd = {
	PppoeLbShowCmd,
	"pppoe_lb show <path>",
	"Show PPPoE load balancer information",
	"Displays statistics, worker count, algorithm, and session map for the specified pppoe_lb node.",
	{ "pppoe_lb info" }
};

const struct ngcmd pppoe_lb_config_cmd = {
	PppoeLbConfigCmd,
	"pppoe_lb config <path> [algorithm <0|1|2>] [max_workers <n>] [debug <level>]",
	"Configure PPPoE load balancer",
	"Sets the load balancing algorithm (0=round-robin, 1=hash, 2=least-loaded),\n"
	"maximum workers for governor, and debug level.",
	{ "pppoe_lb set" }
};

const struct ngcmd pppoe_lb_stats_cmd = {
	PppoeLbStatsCmd,
	"pppoe_lb stats <path>",
	"Show PPPoE load balancer statistics",
	"Displays packet counts, session counts, and worker statistics.",
	{ "pppoe_lb statistics" }
};

const struct ngcmd pppoe_lb_map_cmd = {
	PppoeLbMapCmd,
	"pppoe_lb map <path>",
	"Show PPPoE session-to-worker mapping",
	"Displays the current session ID to worker node assignments.",
	{ "pppoe_lb sessions" }
};

static int
PppoeLbShowCmd(int ac, char **av)
{
	char *path;
	struct ng_mesg *resp;
	u_char rbuf[sizeof(struct ng_mesg) + sizeof(struct ng_pppoe_lb_stats)];
	int ch;

	/* Get options */
	optreset = 1;
	optind = 1;
	while ((ch = getopt(ac, av, "")) != -1) {
		switch (ch) {
		default:
			return (CMDRTN_USAGE);
		}
	}
	ac -= optind;
	av += optind;

	/* Get arguments */
	switch (ac) {
	case 1:
		path = av[0];
		break;
	default:
		return (CMDRTN_USAGE);
	}

	/* Get statistics */
	if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_GET_STATS, NULL, 0) < 0) {
		warn("send stats msg");
		return (CMDRTN_ERROR);
	}

	if (NgRecvMsg(csock, resp, sizeof(rbuf), NULL) < 0) {
		warn("recv stats msg");
		return (CMDRTN_ERROR);
	}

	struct ng_pppoe_lb_stats *stats = (struct ng_pppoe_lb_stats *)resp->data;

	printf("PPPoE Load Balancer Statistics:\n");
	printf("  Workers:          %u (active: %u)\n",
	    stats->num_workers, stats->num_workers_active);
	printf("  Algorithm:        ");
	switch (stats->algorithm) {
	case NG_PPPOE_LB_ALGO_ROUND_ROBIN:
		printf("round-robin\n");
		break;
	case NG_PPPOE_LB_ALGO_HASH:
		printf("hash-based\n");
		break;
	case NG_PPPOE_LB_ALGO_LEAST_LOADED:
		printf("least-loaded\n");
		break;
	default:
		printf("unknown (%u)\n", stats->algorithm);
	}
	printf("  Packets in:       %lu\n", (u_long)stats->packets_in);
	printf("  Packets out:      %lu\n", (u_long)stats->packets_out);
	printf("  Sessions created: %lu\n", (u_long)stats->sessions_created);
	printf("  Sessions destroyed: %lu\n", (u_long)stats->sessions_destroyed);
	printf("  Map entries:      %u\n", stats->map_count);

	free(resp);
	return (CMDRTN_OK);
}

static int
PppoeLbConfigCmd(int ac, char **av)
{
	char *path;
	struct ng_pppoe_lb_config cfg;
	int ch;
	int set_algorithm = 0, set_max_workers = 0, set_debug = 0;

	/* Initialize with defaults */
	memset(&cfg, 0, sizeof(cfg));

	/* Get options */
	optreset = 1;
	optind = 1;
	while ((ch = getopt(ac, av, "")) != -1) {
		switch (ch) {
		default:
			return (CMDRTN_USAGE);
		}
	}
	ac -= optind;
	av += optind;

	/* Get arguments */
	if (ac < 1)
		return (CMDRTN_USAGE);

	path = av[0];
	ac--;
	av++;

	/* Parse optional parameters */
	while (ac >= 2) {
		if (strcmp(av[0], "algorithm") == 0) {
			cfg.algorithm = atoi(av[1]);
			set_algorithm = 1;
			ac -= 2;
			av += 2;
		} else if (strcmp(av[0], "max_workers") == 0) {
			cfg.max_workers = atoi(av[1]);
			set_max_workers = 1;
			ac -= 2;
			av += 2;
		} else if (strcmp(av[0], "debug") == 0) {
			cfg.debug_level = atoi(av[1]);
			set_debug = 1;
			ac -= 2;
			av += 2;
		} else {
			return (CMDRTN_USAGE);
		}
	}

	if (!set_algorithm && !set_max_workers && !set_debug) {
		warnx("No configuration parameters specified");
		return (CMDRTN_USAGE);
	}

	/* Send configuration message */
	if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_SET_CONFIG, &cfg, sizeof(cfg)) < 0) {
		warn("send config msg");
		return (CMDRTN_ERROR);
	}

	printf("Configuration updated successfully\n");
	return (CMDRTN_OK);
}

static int
PppoeLbStatsCmd(int ac, char **av)
{
	char *path;
	struct ng_mesg *resp;
	u_char rbuf[sizeof(struct ng_mesg) + sizeof(struct ng_pppoe_lb_stats)];
	int ch;

	/* Get options */
	optreset = 1;
	optind = 1;
	while ((ch = getopt(ac, av, "")) != -1) {
		switch (ch) {
		default:
			return (CMDRTN_USAGE);
		}
	}
	ac -= optind;
	av += optind;

	/* Get arguments */
	switch (ac) {
	case 1:
		path = av[0];
		break;
	default:
		return (CMDRTN_USAGE);
	}

	/* Get statistics */
	if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_GET_STATS, NULL, 0) < 0) {
		warn("send stats msg");
		return (CMDRTN_ERROR);
	}

	if (NgRecvMsg(csock, resp, sizeof(rbuf), NULL) < 0) {
		warn("recv stats msg");
		return (CMDRTN_ERROR);
	}

	struct ng_pppoe_lb_stats *stats = (struct ng_pppoe_lb_stats *)resp->data;

	printf("PPPoE Load Balancer Statistics:\n");
	printf("  Workers:          %u (active: %u)\n",
	    stats->num_workers, stats->num_workers_active);
	printf("  Algorithm:        ");
	switch (stats->algorithm) {
	case NG_PPPOE_LB_ALGO_ROUND_ROBIN:
		printf("round-robin\n");
		break;
	case NG_PPPOE_LB_ALGO_HASH:
		printf("hash-based\n");
		break;
	case NG_PPPOE_LB_ALGO_LEAST_LOADED:
		printf("least-loaded\n");
		break;
	default:
		printf("unknown (%u)\n", stats->algorithm);
	}
	printf("  Packets in:       %lu\n", (u_long)stats->packets_in);
	printf("  Packets out:      %lu\n", (u_long)stats->packets_out);
	printf("  Sessions created: %lu\n", (u_long)stats->sessions_created);
	printf("  Sessions destroyed: %lu\n", (u_long)stats->sessions_destroyed);
	printf("  Map entries:      %u\n", stats->map_count);

	free(resp);
	return (CMDRTN_OK);
}

static int
PppoeLbMapCmd(int ac, char **av)
{
	char *path;
	struct ng_mesg *resp;
	struct ng_pppoe_lb_map *map;
	u_char rbuf[4096];  /* Buffer for up to ~256 session entries */
	int ch;
	u_int i;

	/* Get options */
	optreset = 1;
	optind = 1;
	while ((ch = getopt(ac, av, "")) != -1) {
		switch (ch) {
		default:
			return (CMDRTN_USAGE);
		}
	}
	ac -= optind;
	av += optind;

	/* Get arguments */
	switch (ac) {
	case 1:
		path = av[0];
		break;
	default:
		return (CMDRTN_USAGE);
	}

	/* Prepare map request */
	map = (struct ng_pppoe_lb_map *)rbuf;
	map->max_entries = (sizeof(rbuf) - sizeof(struct ng_pppoe_lb_map)) /
	    sizeof(struct ng_pppoe_lb_map_entry);

	/* Get session map */
	if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_GET_MAP, map, sizeof(struct ng_pppoe_lb_map)) < 0) {
		warn("send map msg");
		return (CMDRTN_ERROR);
	}

	if (NgRecvMsg(csock, resp, sizeof(rbuf), NULL) < 0) {
		warn("recv map msg");
		return (CMDRTN_ERROR);
	}

	map = (struct ng_pppoe_lb_map *)resp->data;

	printf("PPPoE Session-to-Worker Mapping:\n");
	printf("  %-10s %-10s %-15s\n", "Session ID", "Worker", "Last Activity");
	printf("  %-10s %-10s %-15s\n", "----------", "------", "-------------");

	if (map->count == 0) {
		printf("  (no active sessions)\n");
	} else {
		for (i = 0; i < map->count; i++) {
			struct ng_pppoe_lb_map_entry *entry = &map->entries[i];
			printf("  %-10u %-10u %-15u\n",
			    entry->session_id,
			    entry->worker_index,
			    entry->last_activity);
		}
	}

	printf("\n  Total sessions: %u\n", map->count);

	free(resp);
	return (CMDRTN_OK);
}
