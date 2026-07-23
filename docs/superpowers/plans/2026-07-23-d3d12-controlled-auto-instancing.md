# D3D12 Controlled Auto-Instancing Implementation Plan

**Goal:** Convert count-agnostic, strictly compatible opaque D3D12 draw runs into complete hardware-instancing chunks while preserving ordered DrawPacket fallback.

**Architecture:** Add a side-effect-free Pass variant query, a world-matrix merge mode to `InstancedBuffer`, and per-RenderQueue prepared runs. `ForwardStage` prepares/uploads runs before the render pass; `RenderQueue` draws only fully ready runs and flushes pending DrawPackets at each ordering boundary.

**Tech Stack:** C++17, Cocos gfx/pipeline APIs, D3D12 Debug Layer, Python static regression suite, MSBuild Debug|x64.

---

## Task 1: Lock the count and fallback contract

**Files:**
- Modify: `native/tests/unit-test/d3d12_perf_static_test.py`

1. Add failing checks for a runtime-count chunk helper, boundary cases through 4097, a `ready` gate, full-run fallback, ordered packet flushes, and absence of a production `2500` constant.
2. Run the focused Python test and retain the expected RED result.

## Task 2: Add a side-effect-free shader variant query

**Files:**
- Modify: `native/cocos/scene/Pass.h`
- Modify: `native/cocos/scene/Pass.cpp`

1. Add a const-style variant query that copies current defines, applies SubModel patches and explicit overrides, and queries ProgramLibrary without changing Pass defines, shader, batching scheme, pipeline layout, or hash.
2. Keep the existing public patch behavior unchanged for existing callers.
3. Run the focused test.

## Task 3: Add count-safe controlled instance buffers

**Files:**
- Modify: `native/cocos/renderer/pipeline/InstancedBuffer.h`
- Modify: `native/cocos/renderer/pipeline/InstancedBuffer.cpp`

1. Add `mergeWorldMatrix()` that accepts the selected shader and validated optional shadow attribute.
2. Reuse chunks only below `MAX_CAPACITY`; allocate additional chunks for arbitrary `N`.
3. Expose pending instance total and validity so a caller can prove the complete run before skipping originals.
4. Upload only active bytes for each chunk.

## Task 4: Detect, prepare, and record complete runs

**Files:**
- Modify: `native/cocos/renderer/pipeline/RenderQueue.h`
- Modify: `native/cocos/renderer/pipeline/RenderQueue.cpp`

1. Enable only for D3D12 opaque queues without occlusion queries and only for builtin standard, batching `NONE`, default Model, supported macro/layout subsets.
2. Compare exact pass/shader/material, IA geometry and draw info, local non-world resources, and relevant Model state.
3. Scan only contiguous sorted runs; prepare all chunks and mark `ready` only when accumulated instances equal run count.
4. During recording, flush earlier DrawPackets before each ready run, draw every active chunk at the original position, and keep all originals when a run is not ready.

## Task 5: Upload before render pass

**Files:**
- Modify: `native/cocos/renderer/pipeline/forward/ForwardStage.cpp`

1. Call auto-instancing prepare/upload after queue sort and before framegraph render-pass execution.
2. Leave Vulkan/GLES behavior dormant behind the D3D12 runtime gate.

## Task 6: Verify correctness and performance

**Files:**
- Record: `AI/d3d12 optimize/fps_iteration/`

1. Run the complete static suite.
2. Build `D:\Work\CocosProjects\cocos-test-projects\build\windows\proj\test-cases.sln`, Debug|x64.
3. Run visible foreground with D3D12 Debug Layer and validate cold-start 2500 and 3000, plus capacity boundaries, with zero matched errors.
4. Run macro-off performance sampling: discard 60 FPS entries, average the next 30, require `>=59`.
5. Record start/end time, duration, code changes, reason, FPS and improvement.
