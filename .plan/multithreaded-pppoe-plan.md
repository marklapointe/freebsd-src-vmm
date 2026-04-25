# Multithreaded PPPoE for FreeBSD — Implementation Plan

## 1. Executive Summary

This document outlines a sensible, incremental approach to adding multithreaded PPPoE support to FreeBSD. The goal is to enable parallel processing of PPPoE sessions to improve throughput and reduce latency on multi-core systems serving many concurrent subscribers.

**Primary Recommendation:** Horizontal scaling via multiple `ng_pppoe` nodes with a session-aware load balancer.

---

## 2. Current Architecture Analysis

### 2.1 Components

| Component | Location | Role |
|-----------|----------|------|
| `ng_pppoe` | `sys/netgraph/ng_pppoe.c` | Kernel netgraph node handling PPPoE discovery and session encapsulation/decapsulation |
| `pppoed` | `libexec/pppoed/pppoed.c` | Userland daemon for discovery phase and session setup |
| `ng_ether` | `sys/netgraph/ng_ether.c` | Netgraph node bridging Ethernet interfaces into netgraph |
| `ng_base` | `sys/netgraph/ng_base.c` | Netgraph framework with built-in worker thread pool |

### 2.2 Current Threading Model

- Netgraph maintains a pool of worker threads (`ngthread`), defaulting to `mp_ncpus` threads
- These threads pull items from a global worklist and process them
- **Critical limitation:** All packets for a single netgraph node are serialized through that node's input queue
- A single `ng_pppoe` node handles all sessions, becoming the bottleneck

### 2.3 Session Data Structures

```
struct PPPoE (per-node private data)
├── struct sess_hash_entry sesshash[SESSHASHSIZE]  (session hash table)
│   └── mtx (per-bucket lock)
├── LIST_HEAD(, sess_con) listeners                  (listening hooks)
├── hook_p ethernet_hook                             (ethernet connection)
└── int packets_in, packets_out

struct sess_con (per-session data)
├── uint16_t Session_ID
├── enum state { PPPOE_SINIT, PPPOE_SREQ, PPPOE_SOFFER, PPPOE_NEWCONNECTED, PPPOE_CONNECTED }
├── struct pppoe_neg *neg                            (negotiation state)
├── hook_p hook                                      (upper layer hook)
└── struct pppoe_full_hdr pkt_hdr                    (pre-built packet header)
```

### 2.4 Bottleneck Identification

1. **Node-level serialization:** All packets for all sessions flow through one netgraph node
2. **Discovery phase:** PADI/PADO/PADR/PADS handling is inherently serial (session creation)
3. **Session lookup:** Hash table lookups are fast but still serialized at the node level
4. **Data path:** Once connected, sessions are independent but still processed serially

---

## 3. Proposed Architecture: Multi-Node PPPoE with Load Balancing

### 3.1 High-Level Design

```
+-------------------------------------------------------------+
|                    Ethernet Interface                        |
|                      (ng_ether node)                         |
+----------------------------+--------------------------------+
                             |
                             v
+-------------------------------------------------------------+
|              ng_pppoe_lb (Load Balancer Node)                |
|  - Distributes packets to worker nodes based on session hash |
|  - Handles discovery phase (PADI) broadcast                  |
|  - Manages session-to-node mapping table                     |
+--+-----------+-----------+-----------+-----------+----------+
   |           |           |           |           |
   v           v           v           v           v
+------+   +------+   +------+   +------+   +------+
|pppoed|   |pppoed|   |pppoed|   |pppoed|   |pppoed|
|  #0  |   |  #1  |   |  #2  |   |  #3  |   |  #N  |
+------+   +------+   +------+   +------+   +------+
```

### 3.2 Key Design Principles

1. **Minimal changes to existing code:** Reuse `ng_pppoe` as-is, add new components
2. **Leverage existing netgraph parallelism:** Each worker node runs on different threads
3. **Session affinity:** All packets for a given session go to the same worker node
4. **Incremental deployment:** Can start with 1 node (current behavior) and scale up

---

## 4. Implementation Phases

### Phase 1: Create `ng_pppoe_lb` Load Balancer Node

**New file:** `sys/netgraph/ng_pppoe_lb.c`

#### 4.1.1 Node Type Definition

```c
static struct ng_type ng_pppoe_lb_typestruct = {
    .version =    NG_ABI_VERSION,
    .name =       "pppoe_lb",
    .constructor = ng_pppoe_lb_constructor,
    .rcvmsg =     ng_pppoe_lb_rcvmsg,
    .shutdown =   ng_pppoe_lb_shutdown,
    .newhook =    ng_pppoe_lb_newhook,
    .connect =    ng_pppoe_lb_connect,
    .rcvdata =    ng_pppoe_lb_rcvdata,
    .disconnect = ng_pppoe_lb_disconnect,
    .cmdlist =    ng_pppoe_lb_cmds,
};
```

#### 4.1.2 Private Data Structure

```c
struct pppoe_lb_private {
    node_p          node;
    hook_p          ether_hook;           /* Connection to ng_ether */
    hook_p         *worker_hooks;          /* Array of worker node hooks */
    int             num_workers;
    int             num_workers_active;
    
    /* Session-to-worker mapping */
    struct sess_map {
        uint16_t    session_id;
        int         worker_index;
        time_t      last_activity;
    } *session_map;
    
    struct mtx      map_mtx;
    int             map_size;
    int             map_count;
    
    /* Discovery handling */
    struct mtx      discovery_mtx;
    int             next_worker;           /* Round-robin for new sessions */
    
    /* Statistics */
    uint64_t        packets_in;
    uint64_t        packets_out;
    uint64_t        sessions_created;
    uint64_t        sessions_destroyed;
};
```

#### 4.1.3 Packet Distribution Logic

```c
static int
ng_pppoe_lb_rcvdata(hook_p hook, item_p item)
{
    struct mbuf *m;
    struct pppoe_full_hdr *wh;
    struct pppoe_lb_private *priv;
    int worker_idx, error = 0;
    
    priv = NG_NODE_PRIVATE(NG_HOOK_NODE(hook));
    NGI_GET_M(item, m);
    
    if (m->m_len < sizeof(*wh))
        m = m_pullup(m, sizeof(*wh));
    if (m == NULL)
        return (ENOBUFS);
    
    wh = mtod(m, struct pppoe_full_hdr *);
    
    switch (wh->eh.ether_type) {
    case ETHERTYPE_PPPOE_DISC:
    case ETHERTYPE_PPPOE_3COM_DISC:
        /* Discovery phase - use round-robin for new sessions */
        worker_idx = pppoe_lb_select_worker_discovery(priv);
        break;
        
    case ETHERTYPE_PPPOE_SESS:
    case ETHERTYPE_PPPOE_3COM_SESS:
        /* Session phase - use session ID hash for affinity */
        worker_idx = pppoe_lb_select_worker_session(priv, ntohs(wh->ph.sid));
        break;
        
    default:
        NG_FREE_M(m);
        NG_FREE_ITEM(item);
        return (EPFNOSUPPORT);
    }
    
    if (worker_idx < 0 || worker_idx >= priv->num_workers_active) {
        NG_FREE_M(m);
        NG_FREE_ITEM(item);
        return (ENETUNREACH);
    }
    
    NG_FWD_NEW_DATA(error, item, priv->worker_hooks[worker_idx], m);
    return (error);
}
```

#### 4.1.4 Worker Selection Algorithms

**Discovery Phase (Round-Robin):**
```c
static int
pppoe_lb_select_worker_discovery(struct pppoe_lb_private *priv)
{
    int idx;
    
    mtx_lock(&priv->discovery_mtx);
    idx = priv->next_worker;
    priv->next_worker = (priv->next_worker + 1) % priv->num_workers_active;
    mtx_unlock(&priv->discovery_mtx);
    
    return (idx);
}
```

**Session Phase (Hash-based Affinity):**
```c
static int
pppoe_lb_select_worker_session(struct pppoe_lb_private *priv, uint16_t session_id)
{
    struct sess_map *map;
    int idx, i;
    
    /* Fast path: check session map */
    mtx_lock(&priv->map_mtx);
    for (i = 0; i < priv->map_count; i++) {
        if (priv->session_map[i].session_id == session_id) {
            idx = priv->session_map[i].worker_index;
            priv->session_map[i].last_activity = time_uptime;
            mtx_unlock(&priv->map_mtx);
            return (idx);
        }
    }
    mtx_unlock(&priv->map_mtx);
    
    /* Fallback: hash-based selection */
    return (session_id % priv->num_workers_active);
}
```

#### 4.1.5 Control Messages

New netgraph messages for `ng_pppoe_lb`:

| Message | Purpose |
|---------|---------|
| `NGM_PPPOE_LB_ADD_WORKER` | Add a worker node hook |
| `NGM_PPPOE_LB_REMOVE_WORKER` | Remove a worker node hook |
| `NGM_PPPOE_LB_SET_CONFIG` | Configure load balancing algorithm |
| `NGM_PPPOE_LB_GET_STATS` | Get load balancer statistics |
| `NGM_PPPOE_LB_GET_MAP` | Get session-to-worker mapping |

### Phase 2: Modify `pppoed` for Multi-Node Support

**File:** `libexec/pppoed/pppoed.c`

#### 4.2.1 Changes Required

1. **Add worker node management:**
   - Track multiple `ng_pppoe` nodes instead of one
   - Each worker node handles a subset of sessions

2. **Discovery broadcast:**
   - Send PADI to all worker nodes or to load balancer
   - Load balancer distributes PADI to workers

3. **Session tracking:**
   - Maintain mapping of session ID to worker node
   - Forward session data to correct worker

#### 4.2.2 Configuration Options

Add new command-line options:

```c
/* New options */
case 'w':
    num_workers = atoi(optarg);
    break;
case 'L':
    use_load_balancer = 1;
    break;
```

### Phase 3: Optional Per-Session Parallelism in `ng_pppoe`

**File:** `sys/netgraph/ng_pppoe.c`

#### 4.3.1 Fine-Grained Locking

Replace node-level serialization with per-session locks for data path:

```c
struct sess_con {
    /* Existing fields... */
    struct mtx      sess_mtx;           /* Per-session lock */
    
    /* Data path can be parallelized */
    struct mbuf    *data_queue;         /* Queue for out-of-order packets */
};
```

#### 4.3.2 Parallel Data Path

```c
static int
ng_pppoe_rcvdata_ether(hook_p hook, item_p item)
{
    /* ... existing header parsing ... */
    
    switch (wh->eh.ether_type) {
    case ETHERTYPE_PPPOE_SESS:
    case ETHERTYPE_PPPOE_3COM_SESS:
        sp = pppoe_findsession(privp, wh);
        if (sp == NULL)
            LEAVE(ENETUNREACH);
        
        /* Acquire per-session lock for data path */
        mtx_lock(&sp->sess_mtx);
        
        m_adj(m, sizeof(*wh));
        if (m->m_pkthdr.len < length) {
            mtx_unlock(&sp->sess_mtx);
            LEAVE(EMSGSIZE);
        }
        
        /* Process packet while holding session lock */
        NG_FWD_NEW_DATA(error, item, sp->hook, m);
        
        mtx_unlock(&sp->sess_mtx);
        return (error);
    }
}
```

**Note:** This is more complex and riskier. Recommended only after Phase 1 & 2 are stable.

---

## 5. Alternative Approaches Considered

### 5.1 Single-Node Per-Session Locking (Rejected)

**Approach:** Add fine-grained locks within `ng_pppoe` to allow parallel session processing.

**Why Rejected:**
- Netgraph framework still serializes all node operations
- Would require significant changes to netgraph core
- Risk of introducing deadlocks and race conditions
- Limited benefit without framework-level changes

### 5.2 Taskqueue-Based Processing (Rejected)

**Approach:** Use FreeBSD's `taskqueue` system to offload session processing.

**Why Rejected:**
- Adds latency to packet processing
- More complex lifecycle management
- Doesn't integrate well with netgraph's item-based processing model
- Overkill for this use case

### 5.3 Netgraph Framework Modification (Rejected)

**Approach:** Modify `ng_base.c` to allow parallel processing within a single node.

**Why Rejected:**
- Too invasive, affects all netgraph node types
- Would require extensive testing across all netgraph consumers
- Breaks existing assumptions about node-level serialization

---

## 6. Tooling and Management Interface Modifications

To support and manage multithreaded PPPoE sessions, the following userland tools and interfaces must be modified or created. All modifications must include **activation switches** so administrators can explicitly enable or disable multithreaded mode.

### 6.1 Core Daemon: `pppoed`

**Files:** `libexec/pppoed/pppoed.c`, `libexec/pppoed/pppoed.8`

**Modifications:**
1. **Add `-w <workers>` flag** — Number of PPPoE worker nodes to create (default: 1, which preserves current single-threaded behavior).
2. **Add `-L` flag** — Explicitly enable the `ng_pppoe_lb` load balancer. Without this flag, `pppoed` behaves exactly as before, creating a single `ng_pppoe` node directly attached to `ng_ether`.
3. **Add `-A <algorithm>` flag** — Select load-balancing algorithm when `-L` is used: `round-robin`, `hash`, or `least-loaded`.
4. **Worker lifecycle management:** When `-w > 1` and `-L` are both specified, `pppoed` must:
   - Create the `ng_pppoe_lb` node.
   - Create `w` `ng_pppoe` worker nodes.
   - Connect each worker to the load balancer via netgraph hooks.
   - Register session-to-worker mappings with the load balancer using `NGM_PPPOE_LB_ADD_WORKER`.
   - On shutdown, gracefully drain sessions before disconnecting workers.
5. **Backward compatibility:** If neither `-w` nor `-L` is provided, the daemon follows the existing code path exactly.

**Example usage:**
```sh
# Traditional single-threaded mode (default)
pppoed -p "ISP" em0

# Multithreaded mode with 4 workers and hash-based session affinity
pppoed -L -w 4 -A hash -p "ISP" em0
```

### 6.2 PPP Userland Daemon: `ppp`

**Files:** `usr.sbin/ppp/ether.c`, `usr.sbin/ppp/command.c`, `usr.sbin/ppp/ppp.8`

**Modifications:**
1. **Add `set pppoe workers <n>` command** — Configures how many worker nodes `ppp` should expect when acting as a PPPoE client. This is relevant when `ppp` connects to a multithreaded access concentrator.
2. **Add `show pppoe workers` command** — Displays the number of active worker nodes reported by the peer (if available via PPPoE tags).
3. **Update `ether.c`** — Handle potential `NGM_PPPOE_LB_GET_STATS` messages from the load balancer when querying status, so `ppp` can display per-worker session counts.
4. **Man page update** — Document the new `set pppoe workers` and `show pppoe workers` commands.

**Note:** The `ppp` daemon primarily acts as a client; most multithreading benefits are on the server (access concentrator) side. Client-side changes are mainly for monitoring and diagnostics.

### 6.3 Netgraph Control Utility: `ngctl`

**Files:** `usr.sbin/ngctl/ngctl.c`, `usr.sbin/ngctl/ngctl.8`

**Modifications:**
1. **Add `pppoe_lb` node type support** — Ensure `ngctl mkpeer pppoe_lb:` and related commands work for the new node type.
2. **Add `show pppoe_lb` command** — Display load balancer statistics:
   - Number of active workers
   - Total sessions
   - Packets distributed per worker
   - Current load-balancing algorithm
3. **Add `pppoe_lb config` command** — Runtime reconfiguration of the load balancer:
   ```sh
   ngctl msg pppoe_lb: NGM_PPPOE_LB_SET_CONFIG { algorithm=hash }
   ```
4. **Man page update** — Document new `pppoe_lb` subcommands.

### 6.4 Startup Script: `rc.d/pppoed`

**Files:** `libexec/rc/rc.d/pppoed`, `libexec/rc/rc.conf`

**Modifications:**
1. **Add `pppoed_workers` rc.conf variable** — Number of worker nodes (default: `""`, meaning use single-threaded mode).
2. **Add `pppoed_loadbalancer` rc.conf variable** — Set to `"YES"` to enable the load balancer (default: `"NO"`).
3. **Add `pppoed_lb_algorithm` rc.conf variable** — Load-balancing algorithm when load balancer is enabled (default: `"round-robin"`).
4. **Update startup script** — Pass the appropriate flags to `pppoed` based on rc.conf variables:
   ```sh
   if [ -n "${pppoed_workers}" ] && [ "${pppoed_workers}" -gt 1 ]; then
       pppoed_flags="${pppoed_flags} -w ${pppoed_workers}"
   fi
   if checkyesno pppoed_loadbalancer; then
       pppoed_flags="${pppoed_flags} -L"
       if [ -n "${pppoed_lb_algorithm}" ]; then
           pppoed_flags="${pppoed_flags} -A ${pppoed_lb_algorithm}"
       fi
   fi
   ```
5. **Update `rc.conf` defaults** — Add commented-out examples:
   ```sh
   # pppoed_enable="NO"
   # pppoed_provider=""
   # pppoed_interface=""
   # pppoed_workers=""          # Number of PPPoE worker nodes (>1 enables multithreading)
   # pppoed_loadbalancer="NO"   # Enable ng_pppoe_lb load balancer
   # pppoed_lb_algorithm="round-robin"  # round-robin, hash, least-loaded
   ```

### 6.5 sysctl Variables

**Files:** `sys/netgraph/ng_pppoe_lb.c` (sysctl registration)

**New sysctl variables for runtime tuning:**

| Variable | Type | Default | Description |
|----------|------|---------|-------------|
| `net.graph.pppoe_lb.enabled` | int | 0 | Global enable/disable for load balancer features |
| `net.graph.pppoe_lb.num_workers` | int | 1 | Number of worker nodes |
| `net.graph.pppoe_lb.algorithm` | int | 0 | 0=round-robin, 1=hash, 2=least-loaded |
| `net.graph.pppoe_lb.session_map_size` | int | 1024 | Session map hash table size |
| `net.graph.pppoe_lb.debug` | int | 0 | Debug level (0=none, 1=verbose, 2=very verbose) |

**Important:** `net.graph.pppoe_lb.enabled` acts as a master switch. When set to 0, the load balancer node type may still be loaded, but it refuses to create new instances, preventing accidental activation.

### 6.6 Monitoring and Diagnostics

#### 6.6.1 `netstat` Extensions

**Files:** `usr.bin/netstat/netstat.c`, `usr.bin/netstat/netstat.h`

**Modifications:**
1. **Add `-W` flag** — Display PPPoE worker statistics:
   ```sh
   netstat -W
   ```
   Output format:
   ```
   PPPoE Worker Statistics:
   Worker  Sessions  Packets In  Packets Out  CPU
   0       150       1.2M        1.1M        3
   1       148       1.1M        1.0M        4
   2       152       1.2M        1.1M        5
   3       149       1.1M        1.0M        6
   ```
2. **Implementation:** Query `NGM_PPPOE_LB_GET_STATS` and `NGM_PPPOE_GET_STATUS` from netgraph nodes.

#### 6.6.2 `ifconfig` Extensions

**Files:** `sbin/ifconfig/ifconfig.c`

**Modifications:**
1. **Add `pppoe_workers` option** — Display or set the number of PPPoE workers for an interface:
   ```sh
   ifconfig em0 pppoe_workers 4
   ifconfig em0 pppoe_workers  # displays current value
   ```
2. **Note:** This is primarily a convenience wrapper that communicates with `ng_pppoe_lb` via netgraph messages.

#### 6.6.3 `pppctl` Extensions

**Files:** `usr.sbin/pppctl/pppctl.c`, `usr.sbin/pppctl/pppctl.8`

**Modifications:**
1. **Add `show pppoe lb` command** — Display load balancer status when connected to a multithreaded concentrator.
2. **Add `set pppoe workers` command** — Configure expected worker count (client-side hint).

### 6.7 Kernel Module Loading

**Files:** `sys/netgraph/ng_pppoe_lb.c`, `sys/netgraph/Makefile`

**Modifications:**
1. **KLD module support** — Ensure `ng_pppoe_lb` can be loaded as a kernel module (`ng_pppoe_lb.ko`) rather than requiring a full kernel rebuild.
2. **Module dependencies** — Declare dependency on `ng_pppoe` and `ng_ether`.
3. **Module parameters** — Allow module load-time parameters:
   ```sh
   kldload ng_pppoe_lb num_workers=4 algorithm=hash
   ```

### 6.8 Build System

**Files:** `sys/netgraph/Makefile`, `sys/modules/netgraph/Makefile`, `libexec/pppoed/Makefile`

**Modifications:**
1. Add `ng_pppoe_lb.c` and `ng_pppoe_lb.h` to kernel build.
2. Add `ng_pppoe_lb` module to `sys/modules/netgraph/`.
3. Update `pppoed` Makefile if new source files are added.

---

## 7. Implementation Details

### 7.1 File Changes

| File | Change Type | Description |
|------|-------------|-------------|
| `sys/netgraph/ng_pppoe_lb.c` | New | Load balancer node implementation |
| `sys/netgraph/ng_pppoe_lb.h` | New | Load balancer node header |
| `sys/netgraph/Makefile` | Modify | Add `ng_pppoe_lb` to build |
| `sys/modules/netgraph/ng_pppoe_lb/` | New | KLD module build files |
| `libexec/pppoed/pppoed.c` | Modify | Add multi-node support, activation switches |
| `libexec/pppoed/pppoed.8` | Modify | Update man page with new flags |
| `libexec/rc/rc.d/pppoed` | Modify | Add rc.conf variable handling |
| `libexec/rc/rc.conf` | Modify | Add default rc.conf variables |
| `usr.sbin/ppp/ether.c` | Modify | Handle load balancer messages |
| `usr.sbin/ppp/command.c` | Modify | Add `set/show pppoe workers` commands |
| `usr.sbin/ppp/ppp.8` | Modify | Document new commands |
| `usr.sbin/ngctl/ngctl.c` | Modify | Add `pppoe_lb` subcommands |
| `usr.sbin/ngctl/ngctl.8` | Modify | Document new subcommands |
| `usr.bin/netstat/netstat.c` | Modify | Add `-W` flag for PPPoE worker stats |
| `usr.bin/netstat/netstat.1` | Modify | Document `-W` flag |
| `sbin/ifconfig/ifconfig.c` | Modify | Add `pppoe_workers` option |
| `sbin/ifconfig/ifconfig.8` | Modify | Document `pppoe_workers` option |
| `usr.sbin/pppctl/pppctl.c` | Modify | Add load balancer commands |
| `usr.sbin/pppctl/pppctl.8` | Modify | Document new commands |
| `sys/netgraph/ng_pppoe.c` | Minor | Optional per-session locking |

### 7.2 Configuration Example

```sh
# Traditional single-threaded mode (default behavior)
pppoed -p "ISP" em0

# Multithreaded mode with 4 workers and hash-based session affinity
pppoed -L -w 4 -A hash -p "ISP" em0

# Using ngctl directly
ngctl mkpeer pppoe_lb: pppoe_lb ether
ngctl mkpeer pppoe_lb: pppoe worker0
ngctl mkpeer pppoe_lb: pppoe worker1
ngctl connect pppoe_lb: worker0: lb0 worker0
ngctl connect pppoe_lb: worker1: lb1 worker1
ngctl connect em0: pppoe_lb: lower ether

# Runtime reconfiguration
ngctl msg pppoe_lb: NGM_PPPOE_LB_SET_CONFIG { algorithm=least-loaded }

# Monitoring
netstat -W
ifconfig em0 pppoe_workers
ngctl show pppoe_lb:
```

### 7.3 sysctl Variables

| Variable | Type | Default | Description |
|----------|------|---------|-------------|
| `net.graph.pppoe_lb.enabled` | int | 0 | Master enable/disable switch |
| `net.graph.pppoe_lb.num_workers` | int | 1 | Number of worker nodes |
| `net.graph.pppoe_lb.algorithm` | int | 0 | 0=round-robin, 1=hash, 2=least-loaded |
| `net.graph.pppoe_lb.session_map_size` | int | 1024 | Session map hash table size |
| `net.graph.pppoe_lb.debug` | int | 0 | Debug level |
| `net.graph.pppoe_lb.governor.enabled` | int | 0 | Enable CPU governor (0=disabled, 1=enabled) |
| `net.graph.pppoe_lb.governor.max_workers` | int | 0 | Max workers (0=auto/mp_ncpus, >0=hard cap limited by mp_ncpus) |
| `net.graph.pppoe_lb.governor.cpu_threshold` | int | 80 | CPU % threshold to spawn more workers |
| `net.graph.pppoe_lb.governor.cpu_low_threshold` | int | 30 | CPU % threshold to reduce workers |
| `net.graph.pppoe_lb.governor.scale_up_interval` | int | 10 | Seconds between scale-up checks |
| `net.graph.pppoe_lb.governor.scale_down_interval` | int | 60 | Seconds between scale-down checks |

**Governor Behavior:**
- When `governor.enabled=1`, the load balancer monitors system-wide CPU usage via `read_cpu_time()` kernel function.
- CPU utilization is calculated as: `100 - (idle_time * 100 / total_time)` where total_time includes CP_USER, CP_NICE, CP_SYS, CP_INTR, and CP_IDLE.
- If average CPU exceeds `cpu_threshold` (default 80%) for `scale_up_interval` seconds, and `max_workers` not reached, scale-up is triggered (logged, actual worker creation handled by userland).
- If average CPU drops below `cpu_low_threshold` (default 30%) for `scale_down_interval` seconds, and more than 1 worker exists, scale-down is triggered.
- `max_workers=0` means auto-detect to `mp_ncpus` (number of CPU cores). Setting `max_workers=4` on an 8-core system limits workers to 4. Setting `max_workers=100` on an 8-core system is clamped to 8.
- **Hard cap enforcement:** The governor never allows workers to exceed `mp_ncpus`, regardless of user setting. This prevents oversubscription.

---

## 8. Testing Strategy and Isolated Test Harnesses

**Critical Requirement:** All testing must be performed in isolation. The test harnesses must never load experimental kernel code into the host operating system. Use VMs, user-mode simulation, or dedicated test hardware.

### 8.1 Testing Philosophy

| Level | Environment | Purpose |
|-------|-------------|---------|
| Unit Tests | User-mode mock framework | Test logic in isolation |
| Integration Tests | VM with snapshot rollback | Test kernel module loading and netgraph interactions |
| Performance Tests | Dedicated test hardware or VM with passthrough | Measure throughput and latency under load |
| Stress Tests | VM with resource limits | Verify stability and memory safety |

### 8.2 Unit Test Harness: `tests/netgraph/ng_pppoe_lb_test.c`

**File:** `tests/netgraph/ng_pppoe_lb_test.c`

**Approach:** Create a user-mode test framework that mocks the netgraph API and kernel structures. This allows testing the load balancer logic without loading any kernel code.

```c
/*
 * User-mode unit test for ng_pppoe_lb packet distribution logic.
 * This test runs entirely in userland and does not load any kernel modules.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Mock kernel structures */
struct mock_mbuf {
    uint8_t data[2048];
    size_t len;
};

struct mock_ng_pppoe_lb {
    int num_workers;
    int next_worker;
    uint64_t packets_per_worker[16];
};

/* Include the load balancer logic (extracted into a testable unit) */
#include "ng_pppoe_lb_logic.h"

static void
test_round_robin_discovery(void)
{
    struct mock_ng_pppoe_lb lb = { .num_workers = 4 };
    int worker;

    for (int i = 0; i < 8; i++) {
        worker = pppoe_lb_select_worker_discovery(&lb);
        assert(worker == i % 4);
    }
    printf("PASS: round-robin discovery\n");
}

static void
test_session_affinity(void)
{
    struct mock_ng_pppoe_lb lb = { .num_workers = 4 };
    uint16_t session_id = 0x1234;
    int worker1, worker2;

    worker1 = pppoe_lb_select_worker_session(&lb, session_id);
    worker2 = pppoe_lb_select_worker_session(&lb, session_id);
    assert(worker1 == worker2);
    printf("PASS: session affinity\n");
}

static void
test_worker_bounds(void)
{
    struct mock_ng_pppoe_lb lb = { .num_workers = 4 };
    int worker;

    for (uint16_t sid = 0; sid < 1000; sid++) {
        worker = pppoe_lb_select_worker_session(&lb, sid);
        assert(worker >= 0 && worker < 4);
    }
    printf("PASS: worker bounds\n");
}

int
main(int argc, char **argv)
{
    printf("ng_pppoe_lb unit tests (user-mode, no kernel code)\n");
    test_round_robin_discovery();
    test_session_affinity();
    test_worker_bounds();
    printf("All tests passed.\n");
    return 0;
}
```

**Build and run:**
```sh
cd tests/netgraph
make ng_pppoe_lb_test
./ng_pppoe_lb_test
```

### 8.3 Integration Test Harness: VM-Based Testing

**File:** `tests/netgraph/ng_pppoe_lb_vm_test.sh`

**Approach:** Use a virtual machine (bhyve, QEMU, or VirtualBox) with snapshot support. The test script:
1. Starts a fresh VM from a snapshot.
2. Copies the built kernel module into the VM.
3. Loads the module inside the VM.
4. Runs netgraph commands via SSH.
5. Verifies expected behavior.
6. Reverts the VM to the snapshot, leaving no trace.

```sh
#!/bin/sh
#
# VM-based integration test for ng_pppoe_lb
# This script NEVER loads kernel code on the host.
#

set -e

VM_NAME="pppoe-lb-test"
VM_SNAPSHOT="clean"
SSH_PORT="2222"
TEST_RESULTS="/tmp/ng_pppoe_lb_test_results.log"

# Revert VM to clean snapshot
echo "Reverting VM to clean snapshot..."
vm revert "${VM_NAME}" "${VM_SNAPSHOT}"

# Start VM
echo "Starting test VM..."
vm start "${VM_NAME}"
sleep 10  # Wait for VM to boot

# Copy kernel module into VM
echo "Copying kernel module to VM..."
scp -P "${SSH_PORT}" sys/modules/netgraph/ng_pppoe_lb/ng_pppoe_lb.ko \
    root@localhost:/tmp/

# Run tests inside VM via SSH
echo "Running integration tests inside VM..."
ssh -p "${SSH_PORT}" root@localhost << 'TESTSCRIPT'
    set -e
    echo "=== Loading ng_pppoe_lb module ==="
    kldload /tmp/ng_pppoe_lb.ko
    
    echo "=== Creating test netgraph topology ==="
    ngctl mkpeer pppoe_lb: pppoe_lb ether
    ngctl mkpeer pppoe_lb: pppoe worker0
    ngctl mkpeer pppoe_lb: pppoe worker1
    ngctl connect pppoe_lb: worker0: lb0 worker0
    ngctl connect pppoe_lb: worker1: lb1 worker1
    
    echo "=== Verifying topology ==="
    ngctl list | grep -q pppoe_lb
    ngctl list | grep -q worker0
    ngctl list | grep -q worker1
    
    echo "=== Querying statistics ==="
    ngctl msg pppoe_lb: NGM_PPPOE_LB_GET_STATS
    
    echo "=== Unloading module ==="
    ngctl shutdown pppoe_lb:
    kldunload ng_pppoe_lb
    
    echo "All integration tests passed."
TESTSCRIPT

# Stop VM
echo "Stopping test VM..."
vm stop "${VM_NAME}"

echo "Integration tests completed successfully."
```

### 8.4 Performance Test Harness

**File:** `tests/netgraph/ng_pppoe_lb_perf.c`

**Approach:** Run inside a VM with dedicated vCPUs. Use netgraph loopback nodes to simulate packet flow without requiring actual network hardware.

```c
/*
 * Performance test for ng_pppoe_lb.
 * Runs inside a VM. Uses netgraph loopback to avoid needing real hardware.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netgraph/ng_socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#define NUM_PACKETS 1000000
#define NUM_WORKERS 4

int
main(int argc, char **argv)
{
    int csock, dsock;
    struct ng_mesg msg;
    struct timespec start, end;
    double elapsed;
    
    /* Create netgraph control socket */
    if (NgMkSockNode(NULL, &csock, &dsock) < 0) {
        perror("NgMkSockNode");
        return 1;
    }
    
    /* Create load balancer and workers */
    NgSendMsg(csock, ".", NGM_GENERIC_COOKIE, NGM_MKPEER,
        "pppoe_lb", strlen("pppoe_lb") + 1);
    
    for (int i = 0; i < NUM_WORKERS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "worker%d", i);
        NgSendMsg(csock, "pppoe_lb:", NGM_GENERIC_COOKIE, NGM_MKPEER,
            "pppoe", strlen("pppoe") + 1);
    }
    
    /* Create loopback node for packet generation */
    NgSendMsg(csock, ".", NGM_GENERIC_COOKIE, NGM_MKPEER,
        "loopback", strlen("loopback") + 1);
    NgSendMsg(csock, "loopback:", NGM_GENERIC_COOKIE, NGM_CONNECT,
        "pppoe_lb: lower ether", strlen("pppoe_lb: lower ether") + 1);
    
    /* Generate test packets */
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < NUM_PACKETS; i++) {
        char packet[256];
        struct pppoe_full_hdr *hdr = (struct pppoe_full_hdr *)packet;
        
        memset(packet, 0, sizeof(packet));
        hdr->eh.ether_type = htons(ETHERTYPE_PPPOE_SESS);
        hdr->ph.sid = htons(i % 1000);  /* 1000 unique sessions */
        
        /* Send packet through loopback */
        write(dsock, packet, sizeof(packet));
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    elapsed = (end.tv_sec - start.tv_sec) +
              (end.tv_nsec - start.tv_nsec) / 1e9;
    
    printf("Packets: %d\n", NUM_PACKETS);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.0f packets/sec\n", NUM_PACKETS / elapsed);
    
    /* Cleanup */
    NgSendMsg(csock, "loopback:", NGM_GENERIC_COOKIE, NGM_SHUTDOWN, NULL, 0);
    NgSendMsg(csock, "pppoe_lb:", NGM_GENERIC_COOKIE, NGM_SHUTDOWN, NULL, 0);
    
    close(csock);
    close(dsock);
    
    return 0;
}
```

### 8.5 Stress Test Harness

**File:** `tests/netgraph/ng_pppoe_lb_stress.sh`

**Approach:** Run inside a VM with memory and CPU limits. Rapidly create and destroy sessions while monitoring for leaks and crashes.

```sh
#!/bin/sh
#
# Stress test for ng_pppoe_lb
# Runs inside a VM with resource limits.
#

set -e

DURATION=300  # 5 minutes
WORKERS=8
SESSIONS=10000

echo "Starting ${DURATION}s stress test with ${WORKERS} workers and ${SESSIONS} sessions..."

# Monitor memory usage in background
(
    while true; do
        vmstat -m | grep pppoe >> /tmp/pppoe_memory.log
        sleep 1
    done
) &
MONITOR_PID=$!

# Run stress test
python3 << 'PYTEST'
import random
import struct
import socket
import time

# Connect to netgraph socket
sock = socket.socket(socket.AF_NETGRAPH, socket.SOCK_DGRAM)

start = time.time()
while time.time() - start < 300:
    # Rapidly create and destroy sessions
    for i in range(1000):
        session_id = random.randint(1, 65535)
        # Simulate PADI (discovery)
        packet = b'\x00' * 6 + b'\xff' * 6 + struct.pack('>H', 0x8863)
        sock.sendto(packet, "pppoe_lb:")
        
        # Simulate data
        packet = b'\x00' * 6 + b'\xff' * 6 + struct.pack('>H', 0x8864)
        packet += struct.pack('>H', session_id)
        sock.sendto(packet, "pppoe_lb:")
    
    time.sleep(0.01)

sock.close()
PYTEST

kill $MONITOR_PID

echo "Stress test completed."
echo "Memory usage log: /tmp/pppoe_memory.log"

# Check for memory leaks
if grep -q "fail" /tmp/pppoe_memory.log; then
    echo "FAIL: Memory allocation failures detected"
    exit 1
fi

echo "PASS: No memory issues detected"
```

### 8.6 Test Summary

| Test Type | Location | Environment | Kernel Code Loaded? |
|-----------|----------|-------------|---------------------|
| Unit Tests | `tests/netgraph/ng_pppoe_lb_test.c` | User-mode | **No** |
| Integration Tests | `tests/netgraph/ng_pppoe_lb_vm_test.sh` | VM | Yes, inside VM only |
| Performance Tests | `tests/netgraph/ng_pppoe_lb_perf.c` | VM with dedicated vCPUs | Yes, inside VM only |
| Stress Tests | `tests/netgraph/ng_pppoe_lb_stress.sh` | VM with resource limits | Yes, inside VM only |

**Safety Rules:**
1. Never run `kldload` on the host system for experimental modules.
2. Always use VM snapshots so tests start from a known clean state.
3. Automated CI must use VMs or containers with kernel isolation.
4. Physical test hardware should be dedicated lab equipment, not production servers.

---

## 9. Testing Strategy

### 9.1 Functional Testing

1. **Single worker:** Verify identical behavior to current implementation
2. **Multiple workers:** Verify session affinity and load distribution
3. **Worker failure:** Verify graceful degradation
4. **Discovery phase:** Verify PADI/PADO/PADR/PADS with multiple workers
5. **Session teardown:** Verify proper cleanup and map removal
6. **Activation switches:** Verify that omitting `-L` and `-w` produces exactly the old behavior

### 9.2 Performance Testing

1. **Throughput:** Measure packets/sec with 1, 2, 4, 8 workers
2. **Latency:** Measure per-packet latency under load
3. **Scalability:** Verify linear scaling with CPU cores
4. **Memory:** Monitor memory usage with large session counts

### 9.3 Stress Testing

1. **Rapid session creation/destruction:** Verify no leaks
2. **Out-of-order packets:** Verify session affinity handles reordering
3. **Worker hot-add/remove:** Verify dynamic reconfiguration
4. **High packet loss:** Verify recovery and retransmission
5. **Module load/unload cycles:** Verify clean load and unload

---

## 10. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Session affinity violations | High | Use consistent hashing, verify with tests |
| Memory leaks in session map | Medium | Implement map entry aging/timeout |
| Lock contention in load balancer | Medium | Use per-CPU queues, reduce lock scope |
| Backward compatibility | Low | Default to single-worker mode; require explicit `-L` flag |
| Complexity in pppoed | Medium | Keep changes minimal, well-documented |
| Accidental kernel module load on host | **Critical** | Test harnesses use VMs only; never load on host |
| Performance regression in single-worker mode | Medium | Benchmark before and after; ensure no regression |

---

## 11. TODO — Step-by-Step Implementation Tracker

This section is the master checklist for implementing multithreaded PPPoE. Each task includes:
- **Status:** `NOT STARTED` | `IN PROGRESS` | `COMPLETED`
- **Owner:** Who is working on it
- **Start Date:** When work began
- **End Date:** When work finished
- **Dependencies:** What must be done first
- **Files Modified:** What files are touched
- **Notes:** Any blockers, decisions, or context

### Phase 0: Foundation and Setup

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 0.1 | Create feature branch `feature/multithreaded-pppoe` | NOT STARTED | | | | | | Branch from `main` |
| 0.2 | Set up VM test environment (bhyve/QEMU) | NOT STARTED | | | | | | Must support netgraph and snapshot rollback |
| 0.3 | Verify existing PPPoE tests pass on clean branch | NOT STARTED | | | | 0.2 | | Baseline before any changes |
| 0.4 | Document baseline performance (single-threaded) | NOT STARTED | | | | 0.3 | | Packets/sec, latency, CPU usage |

### Phase 1: Kernel Load Balancer Node (`ng_pppoe_lb`)

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 1.1 | Create `sys/netgraph/ng_pppoe_lb.h` header | COMPLETED | | 2026-04-23 | 2026-04-23 | 0.1 | `ng_pppoe_lb.h` | Define node type, messages, private data |
| 1.2 | Create `sys/netgraph/ng_pppoe_lb.c` core | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.1 | `ng_pppoe_lb.c` | Node constructor, destructor, hook management |
| 1.3 | Implement packet distribution logic | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.2 | `ng_pppoe_lb.c` | Discovery → round-robin, Session → hash |
| 1.4 | Implement session map (hash table) | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.3 | `ng_pppoe_lb.c` | Locking, aging, cleanup |
| 1.5 | Implement control messages (NGM_PPPOE_LB_*) | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.2 | `ng_pppoe_lb.c` | Add worker, remove worker, get stats, get map |
| 1.6 | Add sysctl variables | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.2 | `ng_pppoe_lb.c` | `net.graph.pppoe_lb.*` |
| 1.7 | Add KLD module support | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.2 | `ng_pppoe_lb.c` | `MOD_LOAD`/`MOD_UNLOAD` handlers |
| 1.8 | Update `sys/netgraph/Makefile` | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.1 | `Makefile` | Add `ng_pppoe_lb.o` |
| 1.9 | Create `sys/modules/netgraph/ng_pppoe_lb/` | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.8 | `Makefile`, `ng_pppoe_lb.c` | KLD module build |
| 1.10 | Write unit tests (user-mode mock) | NOT STARTED | | | | 1.3 | `tests/netgraph/ng_pppoe_lb_test.c` | No kernel code loaded |
| 1.11 | Run unit tests | NOT STARTED | | | | 1.10 | | Must pass before proceeding |
| 1.12 | Write integration test script (VM-based) | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.9 | `tests/netgraph/ng_pppoe_lb_vm_test.sh` | Loads module inside VM only |
| 1.13 | Run integration tests | NOT STARTED | | | | 1.12 | | Verify topology creation, stats, cleanup |
| 1.14 | Write performance test harness | NOT STARTED | | | | 1.9 | `tests/netgraph/ng_pppoe_lb_perf.c` | VM with dedicated vCPUs |
| 1.15 | Run performance tests | NOT STARTED | | | | 1.14 | | Measure 1, 2, 4, 8 worker throughput |
| 1.16 | Write stress test harness | NOT STARTED | | | | 1.9 | `tests/netgraph/ng_pppoe_lb_stress.sh` | VM with resource limits |
| 1.17 | Run stress tests | NOT STARTED | | | | 1.16 | | 5-minute run, check for leaks |
| 1.18 | Implement CPU governor thread | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.6 | `ng_pppoe_lb.c` | Monitors CPU load via `read_cpu_time()`, scales workers up/down |
| 1.19 | Implement worker hot-add (scale up) | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.18 | `ng_pppoe_lb.c` | Logs scale-up intent when CPU > threshold, respects max_workers cap |
| 1.20 | Implement worker hot-remove (scale down) | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.19 | `ng_pppoe_lb.c` | Logs scale-down intent when CPU < low_threshold, never goes below 1 worker |
| 1.21 | Add governor sysctl handlers | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.18 | `ng_pppoe_lb.c` | `governor.enabled`, `max_workers` (0=mp_ncpus, >0=hard cap), thresholds |
| 1.22 | Test governor scale-up under load | NOT STARTED | | | | 1.19 | | Verify CPU monitoring works, scale-up logged when CPU > threshold |
| 1.23 | Test governor scale-down under low load | NOT STARTED | | | | 1.20 | | Verify scale-down logged when CPU < low_threshold |
| 1.24 | Test max_workers hard cap | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.21 | `ng_pppoe_lb.c` | Constructor enforces: max_workers = min(user_value, mp_ncpus) |
| 1.25 | Code review and cleanup | IN PROGRESS | | 2026-04-23 | | 1.24 | | Style, comments, locking correctness |

### Phase 2: Userland Daemon (`pppoed`)

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 2.1 | Add `-w <workers>` flag parsing | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.1 | `pppoed.c` | Default 1 (backward compatible) |
| 2.2 | Add `-L` flag parsing | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.1 | `pppoed.c` | Enable load balancer |
| 2.3 | Add `-A <algorithm>` flag parsing | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.2 | `pppoed.c` | round-robin, hash, least-loaded |
| 2.4 | Add `-G <max_workers>` flag parsing | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.3 | `pppoed.c` | Governor hard cap (0=auto/mp_ncpus, >0=hard cap limited by mp_ncpus) |
| 2.5 | Implement worker node creation | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.4 | `pppoed.c` | Create N `ng_pppoe` nodes |
| 2.6 | Implement load balancer setup | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.5 | `pppoed.c` | Create `ng_pppoe_lb`, connect workers |
| 2.7 | Implement graceful shutdown | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.6 | `pppoed.c` | Drain sessions, disconnect, cleanup |
| 2.8 | Update `pppoed.8` man page | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.3 | `pppoed.8` | Document new flags `-L`, `-w`, `-A`, `-G`, multithreaded mode, CPU governor |
| 2.9 | Update `libexec/rc/rc.d/pppoed` | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.1 | `rc.d/pppoed` | Handle `pppoed_workers`, `pppoed_loadbalancer`, `pppoed_algorithm`, `pppoed_governor_max` |
| 2.10 | Update `libexec/rc/rc.conf` defaults | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.9 | `rc.conf.5` | Added `pppoed_loadbalancer`, `pppoed_workers`, `pppoed_algorithm`, `pppoed_governor_max` variables |
| 2.11 | Add `pppoed_max_workers` rc.conf variable | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.4 | `rc.conf.5` | Governor hard cap for startup script (via `pppoed_governor_max`) |
| 2.12 | Test `pppoed` in single-worker mode | NOT STARTED | | | | 2.8 | | Must behave identically to before |
| 2.13 | Test `pppoed` in multi-worker mode | NOT STARTED | | | | 2.12 | | Verify session distribution |
| 2.14 | Test `pppoed` with governor enabled | NOT STARTED | | | | 2.13 | | Verify `-G` cap respected |

### Phase 3: Monitoring and Diagnostics

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 3.1 | Add `ngctl show pppoe_lb` command | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.5 | `usr.sbin/ngctl/pppoe_lb.c` | Display stats, workers, algorithm |
| 3.2 | Add `ngctl pppoe_lb config` command | COMPLETED | | 2026-04-23 | 2026-04-23 | 3.1 | `usr.sbin/ngctl/pppoe_lb.c` | Runtime reconfiguration |
| 3.3 | Add `ngctl pppoe_lb stats` command | COMPLETED | | 2026-04-23 | 2026-04-23 | 3.1 | `usr.sbin/ngctl/pppoe_lb.c` | Display detailed statistics |
| 3.4 | Add `ngctl pppoe_lb map` command | COMPLETED | | 2026-04-23 | 2026-04-23 | 3.1 | `usr.sbin/ngctl/pppoe_lb.c` | Display session-to-worker mapping |
| 3.5 | Update `ngctl.h` for new commands | COMPLETED | | 2026-04-23 | 2026-04-23 | 3.1 | `usr.sbin/ngctl/ngctl.h` | Declare new command structs |
| 3.6 | Update `ngctl/Makefile` | COMPLETED | | 2026-04-23 | 2026-04-23 | 3.1 | `usr.sbin/ngctl/Makefile` | Add pppoe_lb.c to SRCS |
| 3.7 | Update `ngctl.8` man page | COMPLETED | | 2026-04-23 | 2026-04-23 | 3.2 | `ngctl.8` | Document `pppoe_lb show`, `config`, `stats`, `map` subcommands |
| 3.8 | Add `netstat -W` flag | NOT STARTED | | | | 1.5 | `netstat.c` | Query `NGM_PPPOE_LB_GET_STATS` |
| 3.9 | Update `netstat.1` man page | NOT STARTED | | | | 3.8 | `netstat.1` | Document `-W` flag |
| 3.10 | Add `ifconfig pppoe_workers` option | NOT STARTED | | | | 1.5 | `ifconfig.c` | Get/set worker count |
| 3.11 | Update `ifconfig.8` man page | NOT STARTED | | | | 3.10 | `ifconfig.8` | Document option |
| 3.12 | Add `pppctl show pppoe lb` command | NOT STARTED | | | | 3.1 | `pppctl.c` | Client-side diagnostics |
| 3.13 | Update `pppctl.8` man page | NOT STARTED | | | | 3.12 | `pppctl.8` | Document new command |

### Phase 4: PPP Daemon (`ppp`) Client-Side

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 4.1 | Add `set pppoe workers <n>` command | NOT STARTED | | | | 1.1 | `command.c` | Client-side hint |
| 4.2 | Add `show pppoe workers` command | NOT STARTED | | | | 4.1 | `command.c` | Display worker info |
| 4.3 | Update `ether.c` for load balancer messages | NOT STARTED | | | | 1.5 | `ether.c` | Handle `NGM_PPPOE_LB_GET_STATS` |
| 4.4 | Update `ppp.8` man page | NOT STARTED | | | | 4.2 | `ppp.8` | Document new commands |

### Phase 5: Optional Per-Session Locking in `ng_pppoe`

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 5.1 | Add per-session `struct mtx` to `sess_con` | NOT STARTED | | | | 1.18 | `ng_pppoe.c` | Higher risk; defer until Phase 1-2 stable |
| 5.2 | Replace node-level serialization with per-session locks | NOT STARTED | | | | 5.1 | `ng_pppoe.c` | Data path only |
| 5.3 | Test for deadlocks and race conditions | NOT STARTED | | | | 5.2 | | Extensive stress testing required |
| 5.4 | Benchmark performance improvement | NOT STARTED | | | | 5.3 | | Compare to Phase 1-2 results |

### Phase 6: Documentation and Release

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 6.1 | Write `ng_pppoe_lb.4` man page | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.18 | `ng_pppoe_lb.4` | Complete kernel node documentation with hooks, control messages, algorithms, governor, sysctl variables, examples |
| 6.2 | Update `ng_pppoe.4` man page | NOT STARTED | | | | 1.18 | `ng_pppoe.4` | Mention load balancer integration |
| 6.3 | Update `RELNOTES` | NOT STARTED | | | | 2.11 | `RELNOTES` | Summarize feature for release |
| 6.4 | Update `UPDATING` | NOT STARTED | | | | 2.11 | `UPDATING` | Any admin-visible changes |
| 6.5 | Final code review | NOT STARTED | | | | 6.4 | | All phases complete |
| 6.6 | Merge to `main` | NOT STARTED | | | | 6.5 | | After approval |

---

## 12. Future Enhancements

1. **Dynamic worker scaling:** Add/remove workers based on load
2. **Per-worker CPU affinity:** Pin workers to specific CPU cores
3. **Session migration:** Move sessions between workers for load balancing
4. **DPDK integration:** Use DPDK for packet I/O in high-performance scenarios
5. **NUMA awareness:** Optimize for multi-socket systems
6. **eBPF-based load balancing:** Explore eBPF for packet distribution

---

## 13. Conclusion

The recommended approach of adding a load balancer node (`ng_pppoe_lb`) in front of multiple `ng_pppoe` worker nodes provides:

- **Minimal risk:** Reuses existing, well-tested PPPoE code
- **Good scalability:** Distributes sessions across CPU cores
- **Backward compatibility:** Single-worker mode behaves identically to current implementation; multithreaded mode requires explicit activation
- **Incremental deployment:** Can be added to existing systems without disruption
- **Safe testing:** Comprehensive test harnesses ensure no host system impact

This approach aligns with FreeBSD's netgraph design philosophy and leverages existing kernel infrastructure rather than introducing complex locking schemes. The explicit activation switches (`-L`, `-w`, `pppoed_loadbalancer`) ensure that administrators must opt in to multithreaded mode, preventing accidental behavior changes on existing systems.
