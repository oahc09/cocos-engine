# D3D12 Descriptor Staging Heap Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent D3D12 SDK Layers failures when `CCD3D12DescriptorSet::forceUpdate` writes descriptors through a missing or stale CPU staging-heap handle.

**Architecture:** Centralize creation and validation of the per-set CBV/SRV/UAV and sampler staging heaps. Initialization and update both use this helper; update reacquires CPU start handles from the live heap immediately before descriptor writes and aborts without clearing dirty state if recovery fails.

**Tech Stack:** C++17, Direct3D 12, GoogleTest, pytest static regression tests, MSBuild.

## Global Constraints

- Do not suppress D3D12 SDK Layers messages.
- Do not change the public DescriptorSet API.
- Do not write any descriptor unless its required heap, CPU start handle, increment size, and offset are valid.
- Preserve retryability when heap recovery fails.

## Deviations

- The first implementation recreated missing per-set staging heaps. Runtime evidence later showed
  `CreateDescriptorHeap` returning `E_OUTOFMEMORY` after many DescriptorSets, so retrying could not
  fix the root cause. The final implementation suballocates persistent CPU staging ranges from two
  device-level paged pools and coalesces adjacent freed ranges. DescriptorSet binding semantics and
  retryability are preserved while heap-object growth is no longer linear in the set count.

---

### Task 1: Lock the descriptor-heap safety contract

**Files:**
- Modify: `native/tests/unit-test/d3d12_perf_static_test.py`
- Modify: `native/tests/unit-test/src/d3d12_render_pass_test.cpp`

**Interfaces:**
- Consumes: `CCD3D12DescriptorSet::forceUpdate` and the existing D3D12 device factory APIs.
- Produces: static assertions for heap recovery/live-handle acquisition and a runtime null-sampler descriptor update test.

- [x] **Step 1: Add the failing static regression**

Assert that `forceUpdate` calls a staging-heap readiness helper before walking bindings, reacquires both CPU heap starts from the live heaps, and leaves `_isDirty` set when heap recovery fails.

- [x] **Step 2: Verify RED**

Run `python -m pytest native/tests/unit-test/d3d12_perf_static_test.py -q -k descriptor_set_recovers_staging_heaps_before_writes` and confirm it fails because the helper is absent.

- [x] **Step 3: Add the runtime null-sampler regression**

Create a one-binding `SAMPLER_TEXTURE` descriptor set, bind a sampled texture with a null sampler, update it, and assert the D3D12 InfoQueue contains no error or corruption messages.

### Task 2: Make staging heaps recoverable and handles live

**Files:**
- Modify: `native/cocos/renderer/gfx-d3d12/D3D12DescriptorSet.cpp`

**Interfaces:**
- Consumes: descriptor counts and `ID3D12Device::CreateDescriptorHeap`.
- Produces: `ensureStagingHeaps(ID3D12Device *) -> bool` on the private implementation.

- [x] **Step 1: Implement the readiness helper**

Create missing required heaps, validate nonzero start handles and descriptor increments, reset invalid heap state on failure, and log the failing heap type and HRESULT.

- [x] **Step 2: Use the helper from initialization and update**

Call it from `doInit`; in `forceUpdate`, keep `_isDirty = true` and return if readiness cannot be restored.

- [x] **Step 3: Reacquire live CPU start handles**

After readiness succeeds, call `GetCPUDescriptorHandleForHeapStart` on the live heaps and use those local handles for all address calculations.

- [x] **Step 4: Reset complete staging state on destroy**

Zero cached starts, increments, counts, and need flags after releasing the heaps.

### Task 3: Verify the crash path and regressions

**Files:**
- Verify: `native/cocos/renderer/gfx-d3d12/D3D12DescriptorSet.cpp`
- Verify: `native/tests/unit-test/d3d12_perf_static_test.py`
- Verify: `native/tests/unit-test/src/d3d12_render_pass_test.cpp`

**Interfaces:**
- Consumes: existing Debug/Release D3D12 build trees.
- Produces: successful compilation and clean D3D12 runtime tests.

- [x] **Step 1: Verify GREEN on focused tests**

Run the focused static and runtime descriptor tests and confirm zero D3D12 error markers.

- [x] **Step 2: Compile Debug and Release**

Invoke `ClCompile` and `_Lib` directly with project references disabled to avoid the known unrelated CMake regeneration failure.

- [x] **Step 3: Run the complete relevant D3D12 regression filter**

Run render-pass, descriptor, Shader Cache, compile-scheduler, lifecycle, and cache-determinism tests.

- [x] **Step 4: Check patch formatting**

Run `git diff --check` and require exit code 0.
