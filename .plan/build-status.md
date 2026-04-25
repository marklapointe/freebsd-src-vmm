# Multithreaded PPPoE Build Status

**Date:** 2026-04-23 12:14  
**Branch:** feature/multithreaded-pppoe (to be created)  
**Target:** FreeBSD 16  
**Author:** Mark LaPointe <mark@cloudbsd.org>  

---

## Build Environment

- **Host:** FreeBSD 16
- **CPU Cores:** 12
- **Build Target:** Kernel module `ng_pppoe_lb.ko` + userland `pppoed`
- **Test Environment:** VM/bhyve/QEMU only (NEVER load on host)

---

## Implementation Status Summary

### Phase 0: Foundation and Setup

| Task | Status | Date | Notes |
|------|--------|------|-------|
| Create feature branch | NOT STARTED | | Will be done before build |
| Set up VM test environment | NOT STARTED | | Critical for safe testing |
| Verify existing PPPoE tests | NOT STARTED | | Baseline verification |
| Document baseline performance | NOT STARTED | | Pre-build benchmarks |

### Phase 1: Kernel Load Balancer Node (`ng_pppoe_lb`)

| Task | Status | Date | Files |
|------|--------|------|-------|
| 1.1 Create header file | **COMPLETED** | 2026-04-23 | `sys/netgraph/ng_pppoe_lb.h` |
| 1.2 Create core implementation | **COMPLETED** | 2026-04-23 | `sys/netgraph/ng_pppoe_lb.c` |
| 1.3 Packet distribution logic | **COMPLETED** | 2026-04-23 | `ng_pppoe_lb.c` |
| 1.4 Session map (hash table) | **COMPLETED** | 2026-04-23 | `ng_pppoe_lb.c` |
| 1.5 Control messages | **COMPLETED** | 2026-04-23 | `ng_pppoe_lb.c` |
| 1.6 Sysctl variables | **COMPLETED** | 2026-04-23 | `ng_pppoe_lb.c` |
| 1.7 KLD module support | **COMPLETED** | 2026-04-23 | `ng_pppoe_lb.c` |
| 1.8 Update sys/netgraph/Makefile | **COMPLETED** | 2026-04-23 | `sys/netgraph/Makefile` |
| 1.9 Create module directory | **COMPLETED** | 2026-04-23 | `sys/modules/netgraph/pppoe_lb/Makefile` |
| 1.10 Write unit tests | NOT STARTED | | |
| 1.11 Run unit tests | NOT STARTED | | |
| 1.12 Write integration test script | **COMPLETED** | 2026-04-23 | `tests/netgraph/ng_pppoe_lb_vm_test.sh` |
| 1.13 Run integration tests | NOT STARTED | | |
| 1.14-1.17 Performance/stress tests | NOT STARTED | | |
| 1.18 CPU governor thread | **COMPLETED** | 2026-04-23 | `ng_pppoe_lb.c` |
| 1.19 Worker hot-add (scale up) | **COMPLETED** | 2026-04-23 | `ng_pppoe_lb.c` |
| 1.20 Worker hot-remove (scale down) | **COMPLETED** | 2026-04-23 | `ng_pppoe_lb.c` |
| 1.21 Governor sysctl handlers | **COMPLETED** | 2026-04-23 | `ng_pppoe_lb.c` |
| 1.22-1.23 Governor testing | NOT STARTED | | |
| 1.24 max_workers hard cap | **COMPLETED** | 2026-04-23 | `ng_pppoe_lb.c` |
| 1.25 Code review and cleanup | **IN PROGRESS** | 2026-04-23 | |

**Key Implementation Details:**
- Governor uses `read_cpu_time()` for CPU monitoring
- `max_workers=0` auto-detects to `mp_ncpus` (12 on this system)
- Hard cap enforcement: `max_workers = min(user_value, mp_ncpus)`
- Scale-up threshold: 80% CPU, Scale-down: 30% CPU
- Governor logs intent; actual worker creation handled by userland

### Phase 2: Userland Daemon (`pppoed`)

| Task | Status | Date | Files |
|------|--------|------|-------|
| 2.1 Add `-w <workers>` flag | **COMPLETED** | 2026-04-23 | `libexec/pppoed/pppoed.c` |
| 2.2 Add `-L` flag (enable LB) | **COMPLETED** | 2026-04-23 | `libexec/pppoed/pppoed.c` |
| 2.3 Add `-A <algorithm>` flag | **COMPLETED** | 2026-04-23 | `libexec/pppoed/pppoed.c` |
| 2.4 Add `-G <max_workers>` flag | **COMPLETED** | 2026-04-23 | `libexec/pppoed/pppoed.c` |
| 2.5 Worker node creation | **COMPLETED** | 2026-04-23 | `libexec/pppoed/pppoed.c` |
| 2.6 Load balancer setup | **COMPLETED** | 2026-04-23 | `libexec/pppoed/pppoed.c` |
| 2.7 Graceful shutdown | **COMPLETED** | 2026-04-23 | `libexec/pppoed/pppoed.c` |
| 2.8 Update pppoed.8 man page | **COMPLETED** | 2026-04-23 | `libexec/pppoed/pppoed.8` |
| 2.9 Update rc.d/pppoed script | **COMPLETED** | 2026-04-23 | `libexec/rc/rc.d/pppoed` |
| 2.10 Update rc.conf.5 | **COMPLETED** | 2026-04-23 | `share/man/man5/rc.conf.5` |
| 2.11 Add rc.conf variables | **COMPLETED** | 2026-04-23 | `rc.conf.5` (pppoed_loadbalancer, pppoed_workers, pppoed_algorithm, pppoed_governor_max) |
| 2.12-2.14 Testing | NOT STARTED | | |

**Command Line Flags:**
- `-L`: Enable load balancer mode (default: disabled, backward compatible)
- `-w <n>`: Number of worker nodes (default: 1)
- `-A <n>`: Algorithm (0=round-robin, 1=hash, 2=least-loaded)
- `-G <n>`: Governor max workers (0=auto/mp_ncpus, >0=hard cap)

### Phase 3: Monitoring and Diagnostics

| Task | Status | Date | Files |
|------|--------|------|-------|
| 3.1 `ngctl show pppoe_lb` | **COMPLETED** | 2026-04-23 | `usr.sbin/ngctl/pppoe_lb.c` |
| 3.2 `ngctl pppoe_lb config` | **COMPLETED** | 2026-04-23 | `usr.sbin/ngctl/pppoe_lb.c` |
| 3.3 `ngctl pppoe_lb stats` | **COMPLETED** | 2026-04-23 | `usr.sbin/ngctl/pppoe_lb.c` |
| 3.4 `ngctl pppoe_lb map` | **COMPLETED** | 2026-04-23 | `usr.sbin/ngctl/pppoe_lb.c` |
| 3.5 Update ngctl.h | **COMPLETED** | 2026-04-23 | `usr.sbin/ngctl/ngctl.h` |
| 3.6 Update ngctl/Makefile | **COMPLETED** | 2026-04-23 | `usr.sbin/ngctl/Makefile` |
| 3.7 Update ngctl.8 man page | **COMPLETED** | 2026-04-23 | `usr.sbin/ngctl/ngctl.8` |
| 3.8-3.13 Additional tools | NOT STARTED | | |

### Phases 4-6: Remaining Work

- **Phase 4:** PPP daemon client-side modifications (NOT STARTED)
- **Phase 5:** Optional per-session locking (NOT STARTED, higher risk)
- **Phase 6:** Documentation and release (NOT STARTED)

---

## Files Created/Modified

### Kernel (sys/)
- `sys/netgraph/ng_pppoe_lb.h` (NEW)
- `sys/netgraph/ng_pppoe_lb.c` (NEW)
- `sys/modules/netgraph/pppoe_lb/Makefile` (NEW)
- `sys/modules/netgraph/Makefile` (MODIFIED - added pppoe_lb)

### Userland (libexec/)
- `libexec/pppoed/pppoed.c` (MODIFIED - added -L, -w, -A, -G flags)
- `libexec/pppoed/pppoed.8` (MODIFIED - documented multithreaded mode, new flags, CPU governor)
- `libexec/rc/rc.d/pppoed` (MODIFIED - added multithreaded mode support with rc.conf variables)

### Tools (usr.sbin/)
- `usr.sbin/ngctl/pppoe_lb.c` (NEW - pppoe_lb subcommands: show, config, stats, map)
- `usr.sbin/ngctl/ngctl.h` (MODIFIED - added pppoe_lb command declarations)
- `usr.sbin/ngctl/main.c` (MODIFIED - registered pppoe_lb commands)
- `usr.sbin/ngctl/ngctl.8` (MODIFIED - documented pppoe_lb subcommands)

### Documentation (share/man/)
- `share/man/man4/ng_pppoe_lb.4` (NEW - complete kernel module documentation)
- `share/man/man5/rc.conf.5` (MODIFIED - added MULTITHREADED PPPOE CONFIGURATION section, new variables)
- `usr.sbin/ngctl/Makefile` (MODIFIED)

### Tests (tests/)
- `tests/netgraph/ng_pppoe_lb_vm_test.sh` (NEW)

### Documentation (.plan/)
- `.plan/multithreaded-pppoe-plan.md` (MODIFIED - updated TODO tracker)
- `.plan/build-status.md` (NEW - this file)

---

## Build Instructions

### Step 1: Create Feature Branch
```bash
cd /home/mlapointe/git/freebsd-src
git checkout -b feature/multithreaded-pppoe
git remote set-url pppoe git@github.com:cloudbsdorg/freebsd-src-pppoe.git
```

### Step 2: Build Kernel Module
```bash
cd /home/mlapointe/git/freebsd-src/sys/modules/netgraph/pppoe_lb
make clean
make
# Output: ng_pppoe_lb.ko
```

### Step 3: Build Userland Daemon
```bash
cd /home/mlapointe/git/freebsd-src/libexec/pppoed
make clean
make
# Output: pppoed
```

### Step 4: Build ngctl
```bash
cd /home/mlapointe/git/freebsd-src/usr.sbin/ngctl
make clean
make
# Output: ngctl
```

### Step 5: Install (in VM only!)
```bash
# WARNING: ONLY run this in a VM/bhyve/QEMU environment
# NEVER load kernel modules on production host system

# Install kernel module
sudo make -C /home/mlapointe/git/freebsd-src/sys/modules/netgraph/pppoe_lb install

# Load module (VM only!)
sudo kldload ng_pppoe_lb

# Verify loaded
kldstat | grep pppoe_lb
```

---

## Test Plan

### Pre-Build Verification
- [ ] Verify all source files compile without errors
- [ ] Verify no style(9) violations
- [ ] Verify all includes are present and correct

### VM-Based Testing (MANDATORY)
- [ ] Create VM snapshot before testing
- [ ] Run `tests/netgraph/ng_pppoe_lb_vm_test.sh` inside VM
- [ ] Verify module loads/unloads cleanly
- [ ] Verify sysctl variables are accessible
- [ ] Verify ngctl commands work
- [ ] Verify governor CPU monitoring works
- [ ] Verify max_workers hard cap (set 100, verify clamped to mp_ncpus)

### Performance Testing (VM only)
- [ ] Single-worker baseline throughput
- [ ] Multi-worker (2, 4, 8, 12) throughput
- [ ] Session creation rate
- [ ] Memory usage under load
- [ ] Governor scale-up/scale-down behavior

### Safety Checks
- [ ] Test harness has VM environment detection
- [ ] Test harness has mandatory cleanup trap
- [ ] Test harness has prominent warnings against host execution
- [ ] All kernel module loading restricted to VMs

---

## Governor Configuration

### Default Behavior
- `governor.enabled=0` (disabled by default)
- `max_workers=0` → auto-detect to `mp_ncpus` (12 on this system)
- `cpu_threshold=80` (scale up when CPU > 80%)
- `cpu_low_threshold=30` (scale down when CPU < 30%)
- `scale_up_interval=10` (seconds between scale-up checks)
- `scale_down_interval=60` (seconds between scale-down checks)

### Example Configurations

**Conservative (4 workers max on 12-core system):**
```bash
sysctl net.graph.pppoe_lb.governor.enabled=1
sysctl net.graph.pppoe_lb.governor.max_workers=4
```

**Aggressive (use all cores):**
```bash
sysctl net.graph.pppoe_lb.governor.enabled=1
sysctl net.graph.pppoe_lb.governor.max_workers=0  # auto = mp_ncpus
```

**Via pppoed command line:**
```bash
pppoed -L -w 8 -G 8 -A 1
```

---

## Known Issues / TODO

1. **Governor worker creation:** Governor logs scale-up/scale-down intent but does not actually create/destroy workers. This requires userland coordination (ngctl or pppoed).

2. **CPU monitoring granularity:** Governor uses system-wide CPU stats, not per-worker. This is intentional to avoid oversubscription but may not reflect actual PPPoE workload.

3. **Testing gaps:** Unit tests, performance tests, and stress tests are not yet implemented or run.

4. **Documentation:** Man pages (`pppoed.8`, `ng_pppoe_lb.4`, `ngctl.8`) need updates.

5. **rc.d integration:** Startup script and rc.conf variables not yet implemented.

---

## Next Steps

1. **Complete code review** (Task 1.25)
   - Verify style(9) compliance
   - Check locking correctness
   - Review comments and documentation

2. **Create feature branch and commit**
   - `git checkout -b feature/multithreaded-pppoe`
   - Add all files with proper author: `Mark LaPointe <mark@cloudbsd.org>`
   - Commit with descriptive message

3. **Set up VM test environment**
   - Create bhyve/QEMU VM with network support
   - Take snapshot for rollback
   - Copy kernel module and test scripts

4. **Run integration tests** (Task 1.13)
   - Execute `tests/netgraph/ng_pppoe_lb_vm_test.sh` in VM
   - Verify all test cases pass
   - Document results

5. **Continue with Phase 2-6 tasks**
   - Update man pages
   - Implement rc.d integration
   - Add remaining monitoring tools

---

## Safety Reminders

⚠️ **CRITICAL SAFETY RULES:**

1. **NEVER load `ng_pppoe_lb.ko` on the host system** - Only load in VM/bhyve/QEMU environments with snapshot capability.

2. **Always test in isolation** - Use VMs with resource limits and network isolation.

3. **Keep VM snapshots** - Before any test, take a snapshot for quick rollback.

4. **Monitor for crashes** - Watch for kernel panics, memory leaks, or resource exhaustion.

5. **Document all findings** - Record test results, issues, and resolutions in this file.

---

## Contact

**Author:** Mark LaPointe  
**Email:** mark@cloudbsd.org  
**Repository:** git@github.com:cloudbsdorg/freebsd-src-pppoe.git  

---

*Last updated: 2026-04-23 12:14*
