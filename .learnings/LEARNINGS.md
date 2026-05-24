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
