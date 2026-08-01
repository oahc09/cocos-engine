## [LRN-20260524-001] correction

**Logged**: 2026-05-24T13:35:00+08:00
**Priority**: medium
**Status**: pending
**Area**: renderer

### Summary
D3D12-only rendering bugs should be treated as backend-specific until evidence proves a shared scene/pipeline state bug.

### Details
The user correctly pointed out that Vulkan and GLES3 rendering correctly means scene data and high-level parameters are likely valid. Shader macro anomalies observed in D3D12 logs may still be a D3D12 backend symptom, such as shader translation, binding reflection, pass ordering, or resource format handling, rather than the root cause in native scene objects.

### Suggested Action
For D3D12-only shadow issues, first compare D3D12 shader/binding/depth behavior against Vulkan/GLES3 and avoid broad scene-layer fixes unless the same incorrect macro state is reproduced in other backends or in backend-independent pipeline state.

### Metadata
- Source: user_feedback
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12Shader.cpp, native/cocos/renderer/gfx-d3d12/D3D12DescriptorSet.cpp
- Tags: d3d12, debugging, shadow

---

## [LRN-20260606-002] correction

**Logged**: 2026-06-06T16:30:00+08:00
**Priority**: high
**Status**: pending
**Area**: renderer

### Summary
Do not treat a confirmed D3D12 sampler semantic defect as the complete root cause without validating the original rendering symptom in a new capture.

### Details
The `mipFilter == NONE` mapping defect was real and the resulting sampler state changed in the next RDC, but the offscreen black region remained. The latest capture shows the material sampler legitimately requests mip filtering and the visible artifact also follows the albedo eye pattern, so the original completion claim was premature.

### Suggested Action
For rendering fixes, require both state-level evidence and a new-capture visual regression check before claiming the user-visible issue is resolved.

### Metadata
- Source: user_feedback
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12DescriptorSet.cpp
- Tags: d3d12, renderdoc, verification, correction

---

## [LRN-20260713-001] correction

**Logged**: 2026-07-13T23:55:00+08:00
**Priority**: high
**Status**: pending
**Area**: tests

### Summary
D3D12 engine changes must be built and tested through the cocos-test-projects Windows project.

### Details
The authoritative integration build directory is `D:\Work\CocosProjects\cocos-test-projects\build\windows\proj`, whose solution is `test-cases.sln`. Engine-local build trees can be used for fast compilation checks, but they do not replace the final project build used for every test run.

### Suggested Action
After modifying engine code, build `test-cases.sln` from the authoritative directory and use its generated `test-cases.exe` for runtime, log, and RenderDoc validation.

### Metadata
- Source: user_feedback
- Related Files: D:\Work\CocosProjects\cocos-test-projects\build\windows\proj\test-cases.sln
- Tags: d3d12, build, integration-test, correction

---

## [LRN-20260714-002] correction

**Logged**: 2026-07-14T22:35:00+08:00
**Priority**: high
**Status**: resolved
**Area**: renderer

### Summary
D3D12 backend actors unwrapped by Validator must not gain temporary `IntrusivePtr` ownership in deferred queues.

### Details
`BufferValidator` owns its backend actor through a raw pointer and deletes it in its destructor. The actor starts with a zero intrusive reference count, so adding it to an `IntrusivePtr<CCD3D12Buffer>` queue increments the count to one and draining the queue decrements it to zero, deleting an actor that the Validator and InputAssembler still use. This produced the dangling buffer pointer in `CCD3D12InputAssembler::fillVertexBufferViews`.

### Suggested Action
Before adding intrusive ownership around Validator actors, audit the wrapper's ownership contract. Deferred queues for these actors should be non-owning and resources must unregister themselves during destruction, matching the existing deferred texture upload pattern.

### Metadata
- Source: user_feedback
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12Device.cpp, native/cocos/renderer/gfx-d3d12/D3D12Buffer.cpp, native/cocos/renderer/gfx-base/GFXDef-common.h
- Tags: d3d12, validator, intrusive-ptr, lifetime, crash

---

## [LRN-20260714-003] correction

**Logged**: 2026-07-14T22:47:00+08:00
**Priority**: critical
**Status**: pending
**Area**: renderer

### Summary
Repeated D3D12 performance optimization must be evidence-gated, minimally scoped, and recorded round by round.

### Details
The user requires up to ten compile-run-measure cycles against the same `test-cases.sln` Debug x64 scene. Every retained code change needs a confirmed root cause, a single-variable minimal patch, an authoritative build, a runtime FPS comparison, correctness checks, and a written keep-or-revert decision. FPS measurement must be added before further optimization.

### Suggested Action
Maintain `native/cocos/renderer/gfx-d3d12/D3D12_PERF_ROUNDS.md` as the audit trail. Do not stack speculative changes, and immediately revert any round that crashes, regresses rendering, emits validation errors, or reduces measured FPS.

### Metadata
- Source: user_feedback
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12_PERF_ROUNDS.md, D:\Work\CocosProjects\cocos-test-projects\build\windows\proj\test-cases.sln
- Tags: d3d12, performance, measurement, reproducibility, minimal-change
- See Also: LRN-20260713-001, LRN-20260714-002

---

## [LRN-20260718-001] knowledge_gap

**Logged**: 2026-07-18T07:35:00+08:00
**Priority**: high
**Status**: resolved
**Area**: renderer

### Summary
`CCD3D12DescriptorSet::Impl::descriptors` is not populated by the descriptor-write path and cannot identify static descriptor reuse.

### Details
The initial local-set diagnostic used `DescriptorData` fields to build a static signature. The fields remained zero for all sampled local sets, producing a false one-signature result. Descriptor counts remain valid, but resource identity must be derived from the live `_buffers` and `_textures` binding arrays together with layout binding metadata.

### Suggested Action
For descriptor diagnostics or cache keys, derive identity from the same live bindings used to create CPU staging descriptors; do not use `DescriptorData` unless its write lifecycle is explicitly established and tested.

### Metadata
- Source: error
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12DescriptorSet.cpp
- Tags: d3d12, descriptor, diagnostics, cache-key

---
