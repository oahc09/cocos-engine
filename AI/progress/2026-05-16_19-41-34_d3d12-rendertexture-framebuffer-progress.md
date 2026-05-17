# D3D12 RenderTexture / Framebuffer Debug Progress

Time: 2026-05-16 19:41 Asia/Shanghai
Workspace: D:\cocos_custome\cocos-engine
Scope constraint: only modify D3D12 backend under native/cocos/renderer/gfx-d3d12. NativePipeline changes were reverted/kept clean.

## Current Symptom

RenderTexture pass is expected to render into a 256x256 offscreen RT, then EID 605 samples that RT. RenderDoc still shows pass 1 drawing/clearing to the swapchain/backbuffer instead of the offscreen color RT.

Latest loaded RDoc observed:
- C:\Users\caosh\AppData\Local\Temp\RenderDoc\test-cases_2026.05.16_09.46.56_frame559.rdc
- API: D3D12
- Frame summary: 9 draws, 6 clears, 1 present, markers: Colour Pass #1 and Colour Pass #2.

Key RDoc evidence:
- Pass 1 EID 561 ClearRenderTargetView output: swapchain/backbuffer color.
- Pass 1 EID 581 DrawIndexedInstanced output: ResourceId::449, 800x600, R8G8B8A8_UNORM.
- Pass 1 EID 581 depth output: ResourceId::1965, 256x256, D24S8.
- EID 605 Pixel SRV slot 0: ResourceId::1939, 256x256, R8G8B8A8_UNORM.
- Therefore SRV binding is not the primary issue. The 256x256 RT exists and is sampled, but pass 1 color target is still the 800x600 swapchain/backbuffer.

Important interpretation:
- Current evidence is stronger for framebuffer input mismatch: pass 1 framebuffer color attachment appears to be swapchain/backbuffer while depth attachment is the 256x256 offscreen depth.
- This is not safely fixable in D3D12 backend by guessing/replacing the color RT. Backend cannot infer which 256x256 color texture should be used if FramebufferInfo already contains swapchain color.

## Changes Attempted And Reverted

Tried but reverted/removed because it caused crash or changed offscreen behavior:
- D3D12Texture explicit _isSwapchainTexture flag.
- Global offscreen color texture registry.
- Automatic replacement of mismatched swapchain color + 256 depth with a guessed offscreen color RT.
- beginRenderPass syncAttachments() call.
- DIAG-BRP logging that called getD3D12ResourceHandle() during beginRenderPass.
- isOffscreen/syncAttachments/getRTVResourceHandle/getDSVResourceHandle public framebuffer APIs.

Crash observed after unsafe registry/replacement attempt:
- test-cases.exe!std::unique_ptr<cc::gfx::CCD3D12Swapchain::Impl>::operator bool()
- cc::gfx::CCD3D12Swapchain::getCurrentBackBufferHandle() line 124
- cc::gfx::CCD3D12Texture::getD3D12ResourceHandle() line 327
Conclusion: avoid adding paths that call getCurrentBackBufferHandle() from diagnostic/replacement logic using possibly stale swapchain pointers.

## Current Code State

Current dirty files:
- native/cocos/renderer/gfx-d3d12/D3D12CommandBuffer.cpp
- native/cocos/renderer/gfx-d3d12/D3D12Framebuffer.cpp
- native/cocos/renderer/gfx-d3d12/D3D12Framebuffer.h
- native/cocos/renderer/gfx-d3d12/D3D12Texture.cpp

NativePipeline.cpp has no diff in the scoped status check.

Current remaining meaningful D3D12 changes:
1. D3D12Framebuffer.cpp
   - RTV descriptor slot advance moved before null/swapchain checks.
   - This fixes descriptor slot skew when _colorTextures[i] is null.
   - _swapchain reset to nullptr at doInit start.

2. D3D12CommandBuffer.cpp
   - Warn if collected RTV handle is null before OMSetRenderTargets.
   - Added helper getPostTransferTextureState().
   - copy/blit/resolve post-transfer states now use usage-aware helper instead of always returning to shader-resource-ish state.
   - Existing blend factor/stencil ref/descriptor heap changes appear in diff; verify ownership before reverting because some may predate this debugging thread.

3. D3D12Texture.cpp
   - doInit(TextureInfo) checks createResource() return and logs error on failure.
   - doResize() resets _currentState to COMMON after recreating resource.
   - createResource() sets _currentState to COMMON after success.

Current build verification:
- cmake --build build --config Debug --target cocos_engine
- Last run succeeded and produced build\Debug\cocos_engine.lib.

Known caveat:
- git diff still has line-ending noise in D3D12CommandBuffer.cpp / D3D12Texture.cpp because files use CRLF and git diff/check reports CRLF as trailing whitespace. Do not over-interpret diff size.

## Next Recommended Steps

1. Do not reintroduce backend guessing/replacement of framebuffer color attachments.
2. Add targeted logging at framebuffer creation input boundary, preferably in D3D12Framebuffer::doInit only, without calling getD3D12ResourceHandle() on swapchain textures except in safe branches.
3. Log for each D3D12Framebuffer::doInit:
   - framebuffer pointer
   - color count
   - color[i] pointer, objectID, hash, width, height, format, usage, isSwapchainColorTexture, getSwapchain pointer only
   - depth pointer, objectID, hash, width, height, format, usage, getSwapchain pointer
   - renderPass hash / color attachment count if accessible safely
4. Confirm which framebuffer is created for pass 1 and whether FramebufferInfo.colorTextures[0] is already swapchain/backbuffer.
5. If FramebufferInfo is already wrong, root is likely in framegraph/resource mapping before D3D12 backend, such as imported target / renderWindow / RenderSwapchain resource key. User currently requested D3D12-only changes, so backend should only diagnose and avoid breaking offscreen behavior.
6. In RenderDoc validation after any fix:
   - Pass 1 EID 561/581 color output must be the same 256x256 RT sampled at EID 605.
   - Pass 1 color/depth must both be 256x256.
   - EID 605 SRV remains the 256x256 RT.

## Commands Used

Build command:
cmake --build build --config Debug --target cocos_engine

RenderDoc MCP checks used:
- get_capture_status
- get_frame_summary
- get_draw_calls for EID 550-620
- get_draw_call_details for EID 581 and 605
- get_pipeline_state for EID 605
- get_texture_info for color/depth/SRV resources

## Current Working Hypothesis

The D3D12 backend is binding the framebuffer it was given. The framebuffer for Colour Pass #1 likely contains:
- color: swapchain/backbuffer, 800x600
- depth: offscreen depth, 256x256
while the intended 256x256 color RT exists separately and is later sampled.

This points to wrong framebuffer/resource selection before or during D3D12Framebuffer::doInit, not an EID 605 SRV binding issue.
