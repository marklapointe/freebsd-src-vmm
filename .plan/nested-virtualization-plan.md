# Nested Virtualization for FreeBSD VMM — Investigation & Implementation Plan

## 1. Executive Summary

This document investigates why nested virtualization does not work with the FreeBSD VMM (bhyve) and outlines a sensible, incremental approach to adding support. Nested virtualization is the ability to run a hypervisor inside a virtual machine (an "L2 guest").

**Primary Finding:** FreeBSD VMM actively prevents nested virtualization on all supported architectures (amd64 Intel VT-x, amd64 AMD-V, arm64) by hiding virtualization CPUID features, intercepting virtualization instructions without emulation, and explicitly disabling nested virtualization in MSR virtualization.

**Primary Recommendation:** A phased approach starting with Intel VT-x (nVMX) as the most mature hardware-assisted nested virtualization technology, followed by AMD-V (Nested SVM) and arm64 (Nested Virtualization extensions).

---

## 2. Current Architecture Analysis

### 2.1 Components

| Component | Location | Role |
|-----------|----------|------|
| `vmm.ko` (Intel) | `sys/amd64/vmm/intel/vmx.c` | Intel VT-x VMX root operation, guest execution, VM exit handling |
| `vmm.ko` (AMD) | `sys/amd64/vmm/amd/svm.c` | AMD SVM host operation, guest execution, #VMEXIT handling |
| `vmm.ko` (ARM64) | `sys/arm64/vmm/` | ARM64 EL2 hypervisor, VHE/NVHE guest switching |
| `x86.c` | `sys/amd64/vmm/x86.c` | Guest CPUID emulation, feature masking |
| `bhyve` (userland) | `usr.sbin/bhyve/` | Userland hypervisor handling VM exits, device emulation |
| `xmsr.c` | `usr.sbin/bhyve/amd64/xmsr.c` | Guest MSR virtualization |

### 2.2 How Virtualization Features Are Hidden from Guests

#### 2.2.1 Intel VT-x — CPUID Leaf 1

In `sys/amd64/vmm/x86.c`:

```c
case CPUID_0000_0001:
    do_cpuid(1, regs);
    /*
     * Don't expose VMX, SpeedStep, TME or SMX capability.
     * Advertise x2APIC capability and Hypervisor guest.
     */
    regs[2] &= ~(CPUID2_VMX | CPUID2_EST | CPUID2_TM2);
    regs[2] &= ~(CPUID2_SMX);
    regs[2] |= CPUID2_HV;
```

**Impact:** Guest OS cannot detect VMX support via CPUID. Software that checks `CPUID.1:ECX[5]` (VMX bit) will conclude the CPU does not support virtualization.

#### 2.2.2 AMD-V — CPUID Leaf 0x80000001

In `sys/amd64/vmm/x86.c`:

```c
case CPUID_8000_0001:
    cpuid_count(func, param, regs);
    /*
     * Hide SVM from guest.
     */
    regs[2] &= ~AMDID2_SVM;
```

**Impact:** Guest OS cannot detect SVM support via CPUID. Software that checks `CPUID.80000001H:ECX[2]` (SVM bit) will conclude the CPU does not support virtualization.

#### 2.2.3 ARM64

No explicit CPUID-style feature hiding exists because ARM64 uses a different discovery mechanism (ID registers). However, the ARM64 VMM code contains no references to nested virtualization support (no `HCR_EL2.NV` bit manipulation, no nested Stage-2 page table support).

### 2.3 How Virtualization Instructions Are Handled

#### 2.3.1 Intel VT-x — VMX Instructions Cause Unhandled VM Exits

In `sys/amd64/vmm/intel/vmx.c`, the VM exit handler catches all VMX instructions:

```c
case EXIT_REASON_VMCALL:
case EXIT_REASON_VMCLEAR:
case EXIT_REASON_VMLAUNCH:
case EXIT_REASON_VMPTRLD:
case EXIT_REASON_VMPTRST:
case EXIT_REASON_VMREAD:
case EXIT_REASON_VMRESUME:
case EXIT_REASON_VMWRITE:
case EXIT_REASON_VMXOFF:
case EXIT_REASON_VMXON:
    SDT_PROBE3(vmm, vmx, exit, vminsn, vmx, vcpuid, vmexit);
    vmexit->exitcode = VM_EXITCODE_VMINSN;
    break;
```

**Critical Gap:** The userland hypervisor (`bhyve`) does **not** have a handler for `VM_EXITCODE_VMINSN`:

```c
const vmexit_handler_t vmexit_handlers[VM_EXITCODE_MAX] = {
    [VM_EXITCODE_INOUT]  = vmexit_inout,
    [VM_EXITCODE_VMX]    = vmexit_vmx,
    [VM_EXITCODE_SVM]    = vmexit_svm,
    /* ... */
    /* NO VM_EXITCODE_VMINSN handler */
};
```

If a guest executes a VMX instruction, bhyve will hit:
```c
warnx("vm_loop: unexpected exitcode 0x%x", exitcode);
```

#### 2.3.2 AMD-V — SVM Instructions Inject #UD

In `sys/amd64/vmm/amd/svm.c`:

```c
case VMCB_EXIT_SHUTDOWN:
case VMCB_EXIT_VMRUN:
case VMCB_EXIT_VMMCALL:
case VMCB_EXIT_VMLOAD:
case VMCB_EXIT_VMSAVE:
case VMCB_EXIT_STGI:
case VMCB_EXIT_CLGI:
case VMCB_EXIT_SKINIT:
case VMCB_EXIT_ICEBP:
case VMCB_EXIT_INVLPGA:
    vm_inject_ud(vcpu->vcpu);
    handled = 1;
    break;
```

**Impact:** When a guest executes any SVM instruction, the hypervisor injects an undefined instruction (`#UD`) exception into the guest. The guest will crash or panic.

Additionally, the VMRUN intercept bit **must** be set to pass AMD's VMCB consistency check:

```c
/*
 * From section "Canonicalization and Consistency Checks" in APMv2
 * the VMRUN intercept bit must be set to pass the consistency check.
 */
svm_enable_intercept(vcpu, VMCB_CTRL2_INTCPT, VMCB_INTCPT_VMRUN);
```

#### 2.3.3 ARM64 — No Nested Virtualization Infrastructure

The ARM64 VMM (`sys/arm64/vmm/`) has no code to:
- Set `HCR_EL2.NV` (Nested Virtualization) bit
- Trap EL1 accesses to virtualization registers
- Support nested Stage-2 translation (Stage-1 for L1 guest, Stage-2 for L2 guest)

### 2.4 MSR Virtualization Explicitly Blocks Nested Virtualization

In `usr.sbin/bhyve/amd64/xmsr.c`:

```c
case MSR_VM_CR:
    /*
     * We currently don't support nested virt.
     * Windows seems to ignore the cpuid bits and reads this
     * MSR anyways.
     */
    *val = VM_CR_SVMDIS;
    break;
```

**Impact:** Even if a guest ignores the CPUID bits and probes `MSR_VM_CR` (AMD's SVM lock/disable MSR), it finds that SVM is disabled at the firmware/BIOS level.

### 2.5 "Nested Paging" Is Not Nested Virtualization

The term "nested" appears in the VMM code in the context of:
- **AMD NPT (Nested Page Tables)** — `sys/amd64/vmm/amd/npt.c`
- **Intel EPT (Extended Page Tables)** — `sys/amd64/vmm/intel/ept.c`
- **Nested Page Faults** — page faults during guest physical → host physical translation

These are **first-level** virtualization features (host → guest), not nested virtualization (guest → nested guest).

### 2.6 Summary of Blockers

| # | Blocker | Architecture | Location |
|---|---------|------------|----------|
| 1 | CPUID hides VMX/SVM bits | Intel, AMD | `sys/amd64/vmm/x86.c` |
| 2 | VMX instructions not emulated | Intel | `sys/amd64/vmm/intel/vmx.c`, `usr.sbin/bhyve/amd64/vmexit.c` |
| 3 | SVM instructions inject #UD | AMD | `sys/amd64/vmm/amd/svm.c` |
| 4 | MSR_VM_CR returns SVMDIS | AMD | `usr.sbin/bhyve/amd64/xmsr.c` |
| 5 | No nested VMCS / shadow VMCS | Intel | Missing entirely |
| 6 | No nested NPT/EPT shadowing | Intel, AMD | Missing entirely |
| 7 | No ARM64 nested virt support | ARM64 | Missing entirely |
| 8 | No userland handler for VMINSN | Intel | `usr.sbin/bhyve/amd64/vmexit.c` |

---

## 3. Proposed Architecture for Nested Virtualization

### 3.1 High-Level Design

```
+-------------------------------------------------------------+
|                    Host (FreeBSD + bhyve)                   |
|                    L0 Hypervisor (VMM Root)                  |
+----------------------------+--------------------------------+
                             |
                             v
+-------------------------------------------------------------+
|                    L1 Guest (bhyve VM)                      |
|              Runs guest OS (e.g., FreeBSD, Linux)           |
|                                                             |
|   +-----------------------------------------------------+   |
|   |  L1 Guest tries to enable virtualization:            |   |
|   |  - CPUID shows VMX/SVM supported (emulated)          |   |
|   |  - Guest sets CR4.VMXE / EFER.SVME                   |   |
|   |  - Guest executes VMXON / VMRUN                      |   |
|   |  → VM exit to L0                                    |   |
|   +-----------------------------------------------------+   |
|                                                             |
|   +-----------------------------------------------------+   |
|   |  L2 Guest (nested VM inside L1)                     |   |
|   |  - L0 creates shadow VMCS / nested VMCB             |   |
|   |  - L0 runs L2 directly using hardware assist         |   |
|   +-----------------------------------------------------+   |
+-------------------------------------------------------------+
```

### 3.2 Key Design Principles

1. **Hardware-assisted where possible:** Use Intel VMCS shadowing (if available) and AMD's Nested SVM (if available) rather than pure software emulation.
2. **L0 runs L2 directly:** The L0 hypervisor should run the L2 guest directly on hardware when possible, with L1 only involved in control operations.
3. **Minimal L1 guest changes:** The L1 guest should believe it has full virtualization support without knowing it's virtualized.
4. **Incremental enablement:** Start with one architecture (Intel nVMX), then extend to AMD and ARM64.
5. **Explicit opt-in:** Nested virtualization should require an explicit VM capability or configuration flag.

---

## 4. Implementation Phases

### Phase 0: Foundation — Expose Virtualization Features to L1 Guest

#### 4.0.1 Intel — Unhide VMX in CPUID

**File:** `sys/amd64/vmm/x86.c`

```c
case CPUID_0000_0001:
    do_cpuid(1, regs);
    /* ... existing masking ... */
    
    /* Conditionally expose VMX if nested virtualization is enabled */
    if (vm_nested_virt_enabled(vm)) {
        /* VMX requires additional CPUID checks (see Phase 1) */
        regs[2] |= CPUID2_VMX;
    }
    break;
```

**Risks:** Simply exposing VMX without full emulation will cause guest crashes when it tries to use VMX instructions.

#### 4.0.2 AMD — Unhide SVM in CPUID

**File:** `sys/amd64/vmm/x86.c`

```c
case CPUID_8000_0001:
    cpuid_count(func, param, regs);
    if (vm_nested_virt_enabled(vm)) {
        regs[2] |= AMDID2_SVM;
    } else {
        regs[2] &= ~AMDID2_SVM;  /* keep existing behavior */
    }
    break;
```

#### 4.0.3 MSR_VM_CR — Allow SVM Enable

**File:** `usr.sbin/bhyve/amd64/xmsr.c`

```c
case MSR_VM_CR:
    if (vm_nested_virt_enabled(vm)) {
        /* Return 0 to indicate SVM is not locked/disabled */
        *val = 0;
    } else {
        *val = VM_CR_SVMDIS;
    }
    break;
```

#### 4.0.4 Add VM Capability for Nested Virtualization

**File:** `sys/amd64/include/vmm.h`

Add to `enum vm_cap_type`:
```c
VM_CAP_NESTED_VIRT,
```

**File:** `sys/amd64/vmm/vmm.c`

Implement `vm_get_capability()` / `vm_set_capability()` support for `VM_CAP_NESTED_VIRT`.

### Phase 1: Intel nVMX — Basic VMX Instruction Emulation

#### 4.1.1 Add bhyve Handler for VM_EXITCODE_VMINSN

**File:** `usr.sbin/bhyve/amd64/vmexit.c`

```c
static int
vmexit_vminsn(struct vmctx *ctx, struct vcpu *vcpu, struct vm_run *vmrun)
{
    struct vm_exit *vme = &vmrun->vm_exit;
    
    if (!vm_nested_virt_enabled(ctx)) {
        /* Inject #UD if nested virt is not enabled */
        vm_inject_exception(vcpu, IDT_UD, false, 0, 0);
        return (VMEXIT_CONTINUE);
    }
    
    /* Dispatch to nested VMX instruction emulator */
    return (nvmm_vmx_handle_vminsn(ctx, vcpu, vme));
}
```

Add to `vmexit_handlers[]`:
```c
[VM_EXITCODE_VMINSN] = vmexit_vminsn,
```

#### 4.1.2 Create Nested VMX Kernel Module (`nvmm_vmx`)

**New file:** `sys/amd64/vmm/intel/nvmm_vmx.c`

Core responsibilities:
- Maintain shadow VMCS for L2 guests
- Emulate VMXON/VMCLEAR/VMPTRLD/VMPTRST/VMREAD/VMWRITE/VMRESUME/VMLAUNCH/VMXOFF
- Handle VM exits from L2 guests (merge L1 and L2 VMCS controls)
- Support VMCS shadowing if hardware supports it

```c
struct nvmm_vmx {
    struct vmcs *shadow_vmcs;      /* Shadow VMCS for L2 */
    uint64_t vmxon_region_gpa;     /* L1's VMXON region */
    uint64_t current_vmcs_gpa;     /* L1's current VMCS pointer */
    bool vmx_operation;             /* VMX operation enabled in L1 */
    bool vmx_root;                  /* L1 in VMX root operation */
};
```

#### 4.1.3 VMXON Emulation

```c
static int
nvmm_vmx_handle_vmxon(struct nvmm_vmx *nvmm, uint64_t vmxon_region)
{
    if (nvmm->vmx_operation) {
        /* VMfailValid: VMXON with VMX operation already active */
        return (VMX_FAIL_VALID);
    }
    
    /* Validate VMXON region alignment and memory type */
    if (!page_aligned(vmxon_region)) {
        return (VMX_FAIL_INVALID);
    }
    
    nvmm->vmxon_region_gpa = vmxon_region;
    nvmm->vmx_operation = true;
    nvmm->vmx_root = true;
    
    return (VMX_SUCCESS);
}
```

#### 4.1.4 VMLAUNCH/VMRESUME Emulation

```c
static int
nvmm_vmx_handle_vmlaunch(struct nvmm_vmx *nvmm, struct vmcs *guest_vmcs)
{
    /*
     * 1. Read L1's VMCS from guest memory
     * 2. Create/merge shadow VMCS for L2
     * 3. Switch to L2 execution (L0 runs L2 directly)
     * 4. On L2 VM exit, determine if it should be handled by L0 or L1
     */
    
    if (!nvmm->vmx_operation || !nvmm->current_vmcs_gpa) {
        return (VMX_FAIL_VALID);
    }
    
    /* Load L2 guest state from L1's VMCS */
    nvmm_vmx_load_l2(nvmm, guest_vmcs);
    
    /* Enter L2 guest (L0 runs L2 directly on hardware) */
    return (nvmm_vmx_enter_l2(nvmm));
}
```

#### 4.1.5 L2 VM Exit Handling

When L2 causes a VM exit, L0 must decide:
- **Handle in L0:** EPT violations, external interrupts, I/O instructions → handle directly
- **Forward to L1:** CPUID, MSR access, control-register access → inject VM exit into L1

```c
static int
nvmm_vmx_handle_l2_exit(struct nvmm_vmx *nvmm, struct vm_exit *l2_exit)
{
    uint32_t exit_reason = l2_exit->u.vmx.exit_reason;
    
    switch (exit_reason) {
    case EXIT_REASON_EPT_FAULT:
    case EXIT_REASON_EXT_INTR:
    case EXIT_REASON_INOUT:
        /* Handle in L0 */
        return (HANDLE_IN_L0);
        
    case EXIT_REASON_CPUID:
    case EXIT_REASON_RDMSR:
    case EXIT_REASON_WRMSR:
    case EXIT_REASON_CR_ACCESS:
        /* Forward to L1 as a VM exit */
        return (nvmm_vmx_forward_to_l1(nvmm, l2_exit));
        
    default:
        /* Conservative: forward to L1 */
        return (nvmm_vmx_forward_to_l1(nvmm, l2_exit));
    }
}
```

### Phase 2: AMD Nested SVM

#### 4.2.1 Stop Injecting #UD for SVM Instructions

**File:** `sys/amd64/vmm/amd/svm.c`

```c
case VMCB_EXIT_VMRUN:
case VMCB_EXIT_VMMCALL:
case VMCB_EXIT_VMLOAD:
case VMCB_EXIT_VMSAVE:
case VMCB_EXIT_STGI:
case VMCB_EXIT_CLGI:
case VMCB_EXIT_SKINIT:
case VMCB_EXIT_ICEBP:
case VMCB_EXIT_INVLPGA:
    if (vm_nested_virt_enabled(vcpu->vcpu)) {
        /* Forward to nested SVM handler */
        handled = nvmm_svm_handle_vminsn(vcpu, vmexit);
    } else {
        vm_inject_ud(vcpu->vcpu);
        handled = 1;
    }
    break;
```

#### 4.2.2 Create Nested SVM Kernel Module (`nvmm_svm`)

**New file:** `sys/amd64/vmm/amd/nvmm_svm.c`

Core responsibilities:
- Maintain nested VMCB for L2 guests
- Emulate VMRUN/VMMCALL/VMLOAD/VMSAVE/STGI/CLGI/SKINIT/INVLPGA
- Handle #VMEXIT from L2 guests
- Support nested NPT (L0 NPT → L1 NPT → L2 guest physical)

```c
struct nvmm_svm {
    struct vmcb *nested_vmcb;      /* L1's VMCB (guest's VMCB) */
    struct vmcb *shadow_vmcb;      /* L0's shadow VMCB for L2 */
    uint64_t hsave_pa;             /* Host save area */
    bool svm_enabled;             /* SVM enabled in L1 */
};
```

#### 4.2.3 VMRUN Emulation

```c
static int
nvmm_svm_handle_vmrun(struct nvmm_svm *nvmm, struct svm_vcpu *vcpu, uint64_t vmcb_pa)
{
    /*
     * 1. Read L1's VMCB from guest physical memory
     * 2. Create shadow VMCB for L2
     * 3. Merge intercepts: L0 intercepts ∪ L1 intercepts
     * 4. Set up nested NPT: L0 NPT maps L1's NPT pages
     * 5. Execute VMRUN to enter L2
     */
    
    /* Read L1 VMCB */
    nvmm_svm_read_guest_vmcb(nvmm, vmcb_pa);
    
    /* Build shadow VMCB for L2 */
    nvmm_svm_build_shadow_vmcb(nvmm, vcpu);
    
    /* Set up nested NPT */
    nvmm_svm_setup_nested_npt(nvmm);
    
    /* Enter L2 */
    return (nvmm_svm_enter_l2(nvmm, vcpu));
}
```

#### 4.2.4 Nested NPT (Nested Page Tables)

AMD Nested SVM requires "nested NPT" — L0's NPT must translate L2 guest physical → L1 guest physical → L0 host physical.

Options:
1. **Shadow-on-shadow:** L0 creates a merged NPT that walks both L1 and L2 page tables
2. **Hardware-assisted:** Use AMD's "Nested Paging" feature if the CPU supports it (some AMD CPUs support a limited form)

**Recommendation:** Start with shadow-on-shadow (software merge), optimize with hardware assist later.

### Phase 3: ARM64 Nested Virtualization

#### 4.3.1 ARM64 Nested Virtualization Hardware Support

ARMv8.3+ introduces nested virtualization via:
- `HCR_EL2.NV` bit — traps EL1 virtualization register accesses to EL2
- `HCR_EL2.NV1` bit — additional nesting support
- `HCR_EL2.NV2` bit (ARMv8.4+) — enhanced nested virtualization
- Nested Stage-2 translation

#### 4.3.2 Required Changes

**File:** `sys/arm64/vmm/vmm_hyp.c`, `sys/arm64/vmm/vmm_arm64.c`

```c
/* Enable nested virtualization for this VM */
if (vm_nested_virt_enabled(vm)) {
    hcr_el2 |= HCR_NV | HCR_NV1;
    /* Set up nested Stage-2 translation */
    nvmm_arm64_setup_nested_s2(vm);
}
```

#### 4.3.3 EL2 Register Trapping

When `HCR_EL2.NV` is set, EL1 accesses to EL2 registers (e.g., `VTTBR_EL2`, `VTCR_EL2`) trap to EL2. L0 must:
1. Maintain a "virtual EL2" context for L1
2. Emulate EL2 register accesses by L1
3. Run L2 using the virtual EL2 context

### Phase 4: Userland Integration

#### 4.4.1 bhyve Configuration Option

**File:** `usr.sbin/bhyve/bhyverun.c`, `usr.sbin/bhyve/config.c`

Add command-line option:
```sh
bhyve --nested-virt ...
```

Or config file option:
```
nested_virt="YES"
```

#### 4.4.2 bhyvectl Support

**File:** `usr.sbin/bhyvectl/bhyvectl.c`

Add commands to query nested virtualization state:
```sh
bhyvectl --vm=<name> --get-nested-virt
bhyvectl --vm=<name> --set-nested-virt
```

### Phase 5: Testing and Validation

#### 4.5.1 Unit Test Harness

**New file:** `tests/sys/amd64/vmm/nvmm_test.c`

Tests:
- CPUID correctly reports VMX/SVM when nested virt is enabled
- VMXON/VMCLEAR/VMPTRLD succeed in L1 guest
- VMLAUNCH enters L2 guest
- L2 guest can execute basic instructions
- L2 VM exits are correctly forwarded to L1 or handled in L0

#### 4.5.2 Integration Test Harness

**New file:** `tests/sys/amd64/vmm/nvmm_integration.sh`

Test matrix:
| Host CPU | L1 Guest OS | L2 Guest OS | Test |
|----------|-------------|-------------|------|
| Intel | FreeBSD | FreeBSD | bhyve-in-bhyve |
| Intel | FreeBSD | Linux | KVM-in-bhyve |
| Intel | Linux | FreeBSD | bhyve-in-KVM |
| AMD | FreeBSD | FreeBSD | bhyve-in-bhyve |
| AMD | Linux | Linux | KVM-in-KVM |

#### 4.5.3 Performance Test Harness

Measure:
- L2 guest performance relative to bare metal
- L2 guest performance relative to L1 guest
- VM exit latency from L2 → L0 → L1 (if forwarded)
- Memory overhead of shadow VMCS / nested NPT

---

## 5. Alternative Approaches Considered

### 5.1 Pure Software Emulation of VMX/SVM (Rejected)

**Idea:** Emulate all VMX/SVM instructions in software without hardware assist.

**Rejection reasons:**
- Performance would be unusable (1000x+ slowdown)
- Complexity is enormous (full x86 emulator for VMX mode)
- No existing codebase to leverage

### 5.2 Paravirtualized Nested Virtualization (Rejected)

**Idea:** Modify L1 guest hypervisor to be aware it's running virtualized and cooperate with L0.

**Rejection reasons:**
- Requires modifying guest OSes (FreeBSD, Linux, Windows)
- Defeats the purpose of nested virtualization (running unmodified hypervisors)
- Not compatible with existing hypervisors

### 5.3 KVM's Approach — Shadow VMCS + Merge VMCS (Adopted for Intel)

**Idea:** Follow KVM's nested virtualization implementation:
- Use shadow VMCS for L2 (if hardware supports VMCS shadowing)
- Merge L1 and L2 VMCS controls
- Run L2 directly on hardware

**Adoption rationale:**
- KVM's nVMX is mature and well-tested
- Intel SDM documents the shadow VMCS mechanism
- Best performance among feasible options

### 5.4 AMD's Approach — Nested Paging + Nested VMCB (Adopted for AMD)

**Idea:** Follow KVM's or Xen's nested SVM implementation:
- Create shadow VMCB for L2
- Merge L0 and L1 intercepts
- Use nested NPT or shadow page tables

**Adoption rationale:**
- AMD's Nested SVM is simpler than Intel's nVMX
- KVM has a working implementation to reference

---

## 6. Risks and Mitigations

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| Intel/AMD CPU doesn't support nested virtualization | Cannot run L2 | Medium | Check CPU features at init; fail gracefully with clear error |
| Shadow VMCS / nested NPT bugs cause host crashes | High | Medium | Extensive testing in VMs before physical hardware; use VM-based test harness |
| Performance degradation makes L2 unusable | High | High | Benchmark early; document expected performance; optimize critical paths |
| Security vulnerabilities in nested virtualization | Critical | Medium | Security audit; follow KVM's security model; restrict nested virt to root |
| Complexity overwhelms maintainers | Medium | Medium | Incremental implementation; thorough documentation; start with Intel only |
| ARM64 nested virtualization requires ARMv8.3+ | Limited hardware support | Medium | Gate on CPU feature detection; document minimum requirements |

---

## 7. TODO — Step-by-Step Implementation Tracker

### Phase 0: Foundation
- [ ] Add `VM_CAP_NESTED_VIRT` to `enum vm_cap_type`
- [ ] Implement `vm_get_capability()` / `vm_set_capability()` for nested virt
- [ ] Intel: Conditionally expose `CPUID2_VMX` in `x86_emulate_cpuid()`
- [ ] AMD: Conditionally expose `AMDID2_SVM` in `x86_emulate_cpuid()`
- [ ] AMD: Return `0` from `MSR_VM_CR` when nested virt is enabled
- [ ] Add `--nested-virt` flag to bhyve command line
- [ ] Add `nested_virt` config option to bhyve

### Phase 1: Intel nVMX
- [ ] Add `vmexit_vminsn()` handler to bhyve
- [ ] Create `sys/amd64/vmm/intel/nvmm_vmx.c` with basic data structures
- [ ] Implement VMXON/VMCLEAR/VMPTRLD/VMPTRST emulation
- [ ] Implement VMREAD/VMWRITE emulation
- [ ] Implement VMLAUNCH/VMRESUME emulation (enter L2)
- [ ] Implement VMXOFF emulation
- [ ] Implement L2 VM exit handling (forward to L1 or handle in L0)
- [ ] Implement shadow VMCS support (if hardware supports it)
- [ ] Implement nested EPT (L0 merges L1 and L2 EPT)
- [ ] Unit tests for Intel nVMX
- [ ] Integration tests: bhyve-in-bhyve, KVM-in-bhyve

### Phase 2: AMD Nested SVM
- [ ] Modify SVM exit handler to forward SVM instructions to nested handler
- [ ] Create `sys/amd64/vmm/amd/nvmm_svm.c` with basic data structures
- [ ] Implement VMRUN emulation (enter L2)
- [ ] Implement VMMCALL/VMLOAD/VMSAVE/STGI/CLGI/SKINIT/INVLPGA emulation
- [ ] Implement L2 #VMEXIT handling
- [ ] Implement nested NPT (shadow-on-shadow)
- [ ] Unit tests for AMD nested SVM
- [ ] Integration tests: bhyve-in-bhyve on AMD

### Phase 3: ARM64 Nested Virtualization
- [ ] Detect ARMv8.3+ nested virtualization support
- [ ] Set `HCR_EL2.NV` / `HCR_EL2.NV1` for nested VMs
- [ ] Implement virtual EL2 context for L1
- [ ] Implement EL2 register trapping and emulation
- [ ] Implement nested Stage-2 translation
- [ ] Unit tests for ARM64 nested virtualization

### Phase 4: Performance Optimization
- [ ] Benchmark L2 vs L1 vs bare metal performance
- [ ] Optimize VM exit forwarding path
- [ ] Implement hardware-assisted nested paging where available
- [ ] Optimize shadow VMCS updates

### Phase 5: Documentation and Release
- [ ] Update `vmm.4` man page with nested virtualization documentation
- [ ] Update `bhyve.8` man page with `--nested-virt` option
- [ ] Write FreeBSD Handbook chapter on nested virtualization
- [ ] Add release notes entry

---

## 8. Future Enhancements

1. **Live Migration of L2 Guests:** Extend bhyve's migration support to handle nested VM state
2. **Nested Virtualization for PCI Passthrough:** Allow L2 guests to use PCI devices passed through at the L0 level
3. **Nested APIC Virtualization:** Optimize interrupt delivery for L2 guests using virtual interrupt delivery
4. **Triple-Nesting (L3):** Support running hypervisors inside L2 guests (theoretical; hardware rarely supports this)
5. **KVM Compatibility:** Ensure L1 Linux guests can run KVM with full feature parity

---

## 9. Conclusion

Nested virtualization is currently **completely unsupported** in FreeBSD VMM due to explicit feature hiding, instruction interception without emulation, and missing infrastructure for shadow VMCS, nested NPT/EPT, and nested Stage-2 translation.

The recommended implementation path is:
1. **Phase 0:** Expose virtualization features conditionally
2. **Phase 1:** Implement Intel nVMX (highest impact, most mature hardware support)
3. **Phase 2:** Implement AMD Nested SVM
4. **Phase 3:** Implement ARM64 nested virtualization

This is a significant undertaking requiring changes across the kernel VMM, userland bhyve, and architecture-specific code. However, the foundation is sound — FreeBSD VMM already has robust VM exit handling, EPT/NPT support, and CPUID emulation that can be extended for nested virtualization.
