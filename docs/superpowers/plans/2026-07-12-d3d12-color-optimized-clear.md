# D3D12 Color Optimized Clear Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate D3D12 warning #820 for zero-cleared offscreen color attachments by supplying the matching optimized clear value at resource creation.

**Architecture:** Keep the change private to the D3D12 backend. `CCD3D12Texture::createResource` will attach a zero RGBA `D3D12_CLEAR_VALUE` only to textures explicitly created with `TextureUsageBit::COLOR_ATTACHMENT`; existing depth-stencil behavior and mipmap-only render-target capability remain unchanged.

**Tech Stack:** C++17, Direct3D 12, GoogleTest, pytest static regression checks, MSBuild.

## Global Constraints

- Do not change the cross-backend `TextureInfo` ABI.
- Do not suppress D3D12 debug-layer message #820.
- Do not attach a color clear value to textures that only gain render-target capability for mipmap generation.
- Preserve the existing depth value of 1.0 and stencil value of 0.

---

### Task 1: Add the failing optimized-clear regression

**Files:**
- Modify: `native/tests/unit-test/d3d12_perf_static_test.py`
- Test: `native/tests/unit-test/d3d12_perf_static_test.py`

**Interfaces:**
- Consumes: the body of `CCD3D12Texture::createResource`.
- Produces: a regression requiring an explicit color-attachment branch that fills all four color components and passes the clear value to `CreateCommittedResource`.

- [x] **Step 1: Write the failing test**

Add a test that extracts `CCD3D12Texture::createResource` and asserts that the optimized-clear selection contains a `TextureUsageBit::COLOR_ATTACHMENT` branch and zero-initializes `clearValue.Color[0..3]`.

- [x] **Step 2: Run test to verify it fails**

Run: `python -m pytest native/tests/unit-test/d3d12_perf_static_test.py -q -k color_attachments_supply_optimized_clear_value`

Expected: FAIL because the production code only selects an optimized clear value for depth-stencil attachments.

### Task 2: Supply the color optimized clear value

**Files:**
- Modify: `native/cocos/renderer/gfx-d3d12/D3D12Texture.cpp`
- Test: `native/tests/unit-test/d3d12_perf_static_test.py`

**Interfaces:**
- Consumes: `TextureUsageBit::COLOR_ATTACHMENT`, `viewFormat`, and the existing local `D3D12_CLEAR_VALUE`.
- Produces: `optimizedClearValue == &clearValue` with RGBA zero for owned color attachments.

- [x] **Step 1: Implement the minimal color branch**

After the depth-stencil branch, add an `else if` for `TextureUsageBit::COLOR_ATTACHMENT`, set `Format = viewFormat`, set all four `Color` values to `0.0F`, and select the clear value.

- [x] **Step 2: Run the focused regression**

Run: `python -m pytest native/tests/unit-test/d3d12_perf_static_test.py -q -k color_attachments_supply_optimized_clear_value`

Expected: PASS.

### Task 3: Build and validate D3D12 behavior

**Files:**
- Verify: `native/cocos/renderer/gfx-d3d12/D3D12Texture.cpp`
- Verify: `native/tests/unit-test/d3d12_perf_static_test.py`

**Interfaces:**
- Consumes: the existing D3D12 unit-test and engine build targets.
- Produces: successful Debug/Release compilation and clean relevant regressions.

- [x] **Step 1: Run relevant static regressions**

Run the D3D12 optimized-clear test together with the existing texture-resource and render-pass checks.

- [x] **Step 2: Compile Debug and Release D3D12 targets**

Build the existing `cocos_engine.vcxproj` targets with project references disabled, matching the established local verification workflow.

- [x] **Step 3: Run relevant D3D12 unit tests**

Run `D3D12RenderPassTest.*` plus the existing shader/cache suites to detect interaction regressions.

- [x] **Step 4: Check the patch**

Run: `git diff --check`

Expected: exit code 0.
