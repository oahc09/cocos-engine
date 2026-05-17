/****************************************************************************
 Copyright (c) 2020-2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#include "D3D12CommandBuffer.h"
#include "D3D12Buffer.h"
#include "D3D12DescriptorSet.h"
#include "D3D12DescriptorSetLayout.h"
#include "D3D12DescriptorHeapPool.h"
#include "D3D12Device.h"
#include "D3D12Framebuffer.h"
#include "D3D12InputAssembler.h"
#include "D3D12PipelineLayout.h"
#include "D3D12PipelineState.h"
#include "D3D12QueryPool.h"
#include "D3D12RenderPass.h"
#include "D3D12Swapchain.h"
#include "D3D12Texture.h"
#include "base/Log.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <algorithm>
    #include <cstring>
    #include <d3d12.h>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

// Maximum number of descriptor sets that can be bound simultaneously
static constexpr uint32_t D3D12_MAX_BOUND_SETS = 4;

// Maximum resource barriers per render pass transition (swapchain + color attachments + depth)
static constexpr uint32_t MAX_PASS_BARRIERS = 16;

namespace {
D3D12_RESOURCE_STATES getPostTransferTextureState(const TextureInfo &info) {
    if (hasFlag(info.usage, TextureUsageBit::SAMPLED)) {
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (hasFlag(info.usage, TextureUsageBit::DEPTH_STENCIL_ATTACHMENT)) {
        return D3D12_RESOURCE_STATE_DEPTH_READ;
    }
    if (hasFlag(info.usage, TextureUsageBit::COLOR_ATTACHMENT)) {
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}
} // namespace

struct CCD3D12CommandBuffer::Impl {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice; // cached ref, not owning

    // Track state for the current recording
    PipelineState *boundPipelineState{nullptr};
    PipelineLayout *boundPipelineLayout{nullptr};
    InputAssembler *boundIA{nullptr};
    bool isRecording{false};

    // Track swapchain for resource barriers during render pass
    CCD3D12Swapchain *activeSwapchain{nullptr};
    ID3D12Resource *activeSwapchainBackBuffer{nullptr};
    ID3D12Resource *activeDepthStencil{nullptr};
    CCD3D12Texture *activeDepthTexture{nullptr};
    struct ActiveColorTarget {
        ID3D12Resource *resource{nullptr};
        CCD3D12Texture *texture{nullptr};
        bool hasTextureState{false};
    };
    ccstd::vector<ActiveColorTarget> activeColorTargets;
    bool inRenderPass{false};

    // Deferred descriptor binding state — collected during bindDescriptorSet,
    // flushed to GPU during draw/dispatch to avoid multiple SetDescriptorHeaps calls.
    struct PendingDescriptorSet {
        DescriptorSet *set{nullptr};
        uint32_t setIndex{0};
        bool valid{false};
    };
    PendingDescriptorSet pendingSets[D3D12_MAX_BOUND_SETS]{};
    uint32_t pendingSetCount{0};
    bool descriptorSetsDirty{false};

    // Upload resources created during copyBuffersToTexture must remain alive
    // until the GPU finishes executing the command list. They are released at
    // the start of the next begin() call, by which point the Queue has
    // already waited for the previous frame's GPU work to complete.
    ccstd::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> pendingUploadResources;
};

CCD3D12CommandBuffer::CCD3D12CommandBuffer()
: _impl(std::make_unique<Impl>()) {
}

CCD3D12CommandBuffer::~CCD3D12CommandBuffer() = default;

void CCD3D12CommandBuffer::doInit(const CommandBufferInfo &info) {
    (void)info;
    auto *device = CCD3D12Device::getInstance();
    if (!device) {
        CC_LOG_ERROR("D3D12CommandBuffer: device not available.");
        return;
    }

    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12CommandBuffer: D3D12 device handle is null.");
        return;
    }

    _impl->d3dDevice = d3dDevice;

    HRESULT hr = d3dDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&_impl->commandAllocator));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12CommandBuffer: CreateCommandAllocator failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    hr = d3dDevice->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        _impl->commandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&_impl->commandList));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12CommandBuffer: CreateCommandList failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    // D3D12 command lists are created in open state, close it initially
    hr = _impl->commandList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12CommandBuffer: initial Close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    CC_LOG_INFO("D3D12CommandBuffer initialized.");
}

void CCD3D12CommandBuffer::doDestroy() {
    _impl->commandList.Reset();
    _impl->commandAllocator.Reset();
    _impl->d3dDevice.Reset();
    _impl->boundPipelineState = nullptr;
    _impl->boundPipelineLayout = nullptr;
}

void CCD3D12CommandBuffer::begin(RenderPass *renderPass, uint32_t subpass, Framebuffer *frameBuffer) {
    (void)renderPass;
    (void)subpass;
    (void)frameBuffer;
    if (!_impl->commandAllocator || !_impl->commandList) {
        CC_LOG_ERROR("D3D12CommandBuffer::begin - allocator or command list is null.");
        return;
    }

    HRESULT hr = _impl->commandAllocator->Reset();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12CommandBuffer::begin - allocator reset failed. HRESULT=0x%08x. "
                      "This typically means the GPU is still executing the previous command list. "
                      "The Queue::submit fence wait should prevent this.", static_cast<unsigned>(hr));
        // Do NOT continue recording commands with a stale allocator — it would corrupt GPU state.
        return;
    }

    hr = _impl->commandList->Reset(_impl->commandAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12CommandBuffer::begin - command list reset failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    if (auto *device = CCD3D12Device::getInstance()) {
        if (auto *heapPool = device->getGPUDescriptorHeapPool()) {
            heapPool->reset();
        }
        if (auto *samplerPool = device->getSamplerDescriptorHeapPool()) {
            samplerPool->reset();
        }
    }

    _impl->isRecording = true;
    _impl->boundPipelineState = nullptr;
    _impl->boundPipelineLayout = nullptr;
    _impl->activeSwapchain = nullptr;
    _impl->activeSwapchainBackBuffer = nullptr;
    _impl->activeDepthStencil = nullptr;
    _impl->activeDepthTexture = nullptr;
    _impl->activeColorTargets.clear();
    _impl->inRenderPass = false;
    // Queue::submit() has already waited for GPU completion.
    _impl->pendingUploadResources.clear();
    // Clear pending descriptor sets
    for (uint32_t i = 0; i < D3D12_MAX_BOUND_SETS; ++i) {
        _impl->pendingSets[i] = {};
    }
    _impl->pendingSetCount = 0;
    _impl->descriptorSetsDirty = false;
    _numDrawCalls = 0;
    _numInstances = 0;
    _numTriangles = 0;
}

void CCD3D12CommandBuffer::end() {
    if (!_impl->commandList) return;

    HRESULT hr = _impl->commandList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12CommandBuffer::end - Close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        if (_impl->d3dDevice) {
            Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
            if (SUCCEEDED(_impl->d3dDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
                const UINT64 msgCount = infoQueue->GetNumStoredMessages();
                CC_LOG_ERROR("[DIAG-CLOSE] InfoQueue pending messages: %llu",
                             static_cast<unsigned long long>(msgCount));
                for (UINT64 i = 0; i < msgCount; ++i) {
                    SIZE_T msgSize = 0;
                    infoQueue->GetMessage(i, nullptr, &msgSize);
                    if (msgSize == 0) {
                        continue;
                    }
                    auto *msgData = static_cast<D3D12_MESSAGE *>(malloc(msgSize));
                    if (!msgData) {
                        continue;
                    }
                    if (SUCCEEDED(infoQueue->GetMessage(i, msgData, &msgSize))) {
                        CC_LOG_ERROR("[DIAG-CLOSE] ID=%u Severity=%u: %.*s",
                                     static_cast<unsigned>(msgData->ID),
                                     static_cast<unsigned>(msgData->Severity),
                                     static_cast<int>(msgData->DescriptionByteLength),
                                     msgData->pDescription);
                    }
                    free(msgData);
                }
                infoQueue->ClearStoredMessages();
            }
        }
    }
    _impl->isRecording = false;
}

void CCD3D12CommandBuffer::beginRenderPass(RenderPass *renderPass, Framebuffer *fbo, const Rect &renderArea, const Color *colors, float depth, uint32_t stencil, CommandBuffer *const *secondaryCBs, uint32_t secondaryCBCount) {
    (void)secondaryCBs;
    (void)secondaryCBCount;
    if (!_impl->commandList) return;

    auto *d3d12Fbo = static_cast<CCD3D12Framebuffer *>(fbo);
    if (!d3d12Fbo) {
        CC_LOG_WARNING("D3D12CommandBuffer::beginRenderPass - framebuffer is null.");
        return;
    }

    // Fixed-size stack array for pre-pass barriers: swapchain(1) + colors(8) + depth(1) ≤ 10
    D3D12_RESOURCE_BARRIER prePassBarriers[MAX_PASS_BARRIERS];
    uint32_t prePassBarrierCount = 0;

    // If this framebuffer renders to a swapchain, insert PRESENT → RENDER_TARGET barrier
    CCD3D12Swapchain *swapchain = d3d12Fbo->getSwapchain();
    if (swapchain) {
        auto *backBuffer = static_cast<ID3D12Resource *>(swapchain->getCurrentBackBufferHandle());
        if (backBuffer) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = backBuffer;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (prePassBarrierCount < MAX_PASS_BARRIERS) {
                prePassBarriers[prePassBarrierCount++] = barrier;
            }

            _impl->activeSwapchain = swapchain;
            _impl->activeSwapchainBackBuffer = backBuffer;
        }
    }

    const uint32_t fboWidth = d3d12Fbo->getWidth();
    const uint32_t fboHeight = d3d12Fbo->getHeight();

    // Count color attachments from the D3D12 framebuffer cache. Repaired
    // offscreen targets may only have a cached ID3D12Resource, not a live
    // CCD3D12Texture actor.
    const uint32_t colorCount = d3d12Fbo->getColorTextureCount();

    // Transition non-swapchain color attachments to RENDER_TARGET
    _impl->activeColorTargets.clear();
    for (uint32_t i = 0; i < colorCount; ++i) {
        auto *d3d12Tex = d3d12Fbo->getColorTexture(i);
        auto *resource = static_cast<ID3D12Resource *>(d3d12Fbo->getColorResource(i));
        if (!resource) continue;

        const bool hasTextureState = d3d12Tex && d3d12Fbo->hasColorTextureState(i);
        _impl->activeColorTargets.push_back({resource, d3d12Tex, hasTextureState});

        if (d3d12Tex && d3d12Tex->isSwapchainColorTexture()) continue;

        D3D12_RESOURCE_STATES prevState = hasTextureState
                                               ? d3d12Tex->getCurrentState()
                                               : CCD3D12Texture::getTrackedResourceState(resource, D3D12_RESOURCE_STATE_COMMON);
        if (prevState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = prevState;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (prePassBarrierCount < MAX_PASS_BARRIERS) {
                prePassBarriers[prePassBarrierCount++] = barrier;
            }
            if (hasTextureState) {
                d3d12Tex->setCurrentState(D3D12_RESOURCE_STATE_RENDER_TARGET);
            } else {
                CCD3D12Texture::setTrackedResourceState(resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
    }

    // Collect RTV handles
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[MAX_ATTACHMENTS]{};
    for (uint32_t i = 0; i < colorCount && i < MAX_ATTACHMENTS; ++i) {
        auto handle = d3d12Fbo->getRTVHandle(i);
        rtvHandles[i].ptr = handle.ptr;
        if (handle.ptr == 0) {
            CC_LOG_WARNING("D3D12 beginRenderPass: RTV handle[%u] is NULL (swapchain=%s, colorCount=%u). "
                           "Off-screen RT will not be bound!",
                           i, swapchain ? "yes" : "no", colorCount);
        }
    }

    // Get DSV handle
    auto dsvPair = d3d12Fbo->getDSVHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    dsvHandle.ptr = dsvPair.ptr;
    bool hasDSV = (dsvPair.ptr != 0);
    auto *depthStencilTexture = d3d12Fbo->getDepthStencilTexture();
    auto *depthStencilResource = static_cast<ID3D12Resource *>(d3d12Fbo->getDepthStencilResource());
    if (hasDSV && depthStencilResource) {
        D3D12_RESOURCE_STATES dsPrevState = depthStencilTexture ? depthStencilTexture->getCurrentState() : D3D12_RESOURCE_STATE_COMMON;
        if (dsPrevState != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            D3D12_RESOURCE_BARRIER depthBarrier{};
            depthBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            depthBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            depthBarrier.Transition.pResource = depthStencilResource;
            depthBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            depthBarrier.Transition.StateBefore = dsPrevState;
            depthBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            if (prePassBarrierCount < MAX_PASS_BARRIERS) {
                prePassBarriers[prePassBarrierCount++] = depthBarrier;
            }
        }
        if (depthStencilTexture) {
            depthStencilTexture->setCurrentState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
        _impl->activeDepthStencil = depthStencilResource;
        _impl->activeDepthTexture = depthStencilTexture;
    } else {
        _impl->activeDepthStencil = nullptr;
        _impl->activeDepthTexture = nullptr;
    }

    // Submit all pre-pass barriers at once
    if (prePassBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(prePassBarrierCount, prePassBarriers);
    }

    // Set render targets
    _impl->commandList->OMSetRenderTargets(
        colorCount,
        rtvHandles,
        FALSE,
        hasDSV ? &dsvHandle : nullptr);

    // Clear render targets based on loadOp from RenderPass
    // CRITICAL: Only clear when loadOp == CLEAR. For LOAD, preserve existing content.
    // This allows multiple render passes to share the same RT (e.g. 3D scene + UI overlay).
    const auto &rpColorAttachments = renderPass ? renderPass->getColorAttachments() : ColorAttachmentList();
    for (uint32_t i = 0; i < colorCount; ++i) {
        if (rtvHandles[i].ptr == 0) continue;

        // Determine loadOp for this attachment
        LoadOp loadOp = LoadOp::CLEAR; // default: clear if no RenderPass info
        if (i < rpColorAttachments.size()) {
            loadOp = rpColorAttachments[i].loadOp;
        }

        if (loadOp == LoadOp::CLEAR && colors) {
            float clearColor[4] = {colors[i].x, colors[i].y, colors[i].z, colors[i].w};
            _impl->commandList->ClearRenderTargetView(rtvHandles[i], clearColor, 0, nullptr);
        }
        // LoadOp::LOAD: do nothing, preserve existing content
        // LoadOp::DISCARD: do nothing, D3D12 DISCARD optimization could be added later
    }

    // Clear depth-stencil based on depthLoadOp/stencilLoadOp from RenderPass
    if (hasDSV) {
        LoadOp depthLoadOp = LoadOp::CLEAR;
        LoadOp stencilLoadOp = LoadOp::CLEAR;
        if (renderPass) {
            const auto &dsAttachment = renderPass->getDepthStencilAttachment();
            depthLoadOp = dsAttachment.depthLoadOp;
            stencilLoadOp = dsAttachment.stencilLoadOp;
        }

        D3D12_CLEAR_FLAGS clearFlags = static_cast<D3D12_CLEAR_FLAGS>(0);
        if (depthLoadOp == LoadOp::CLEAR) {
            clearFlags |= D3D12_CLEAR_FLAG_DEPTH;
        }
        if (stencilLoadOp == LoadOp::CLEAR) {
            clearFlags |= D3D12_CLEAR_FLAG_STENCIL;
        }

        if (clearFlags != 0) {
            _impl->commandList->ClearDepthStencilView(dsvHandle, clearFlags, depth, stencil, 0, nullptr);
        }
    }

    // Set viewport from render area
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(renderArea.x);
    vp.TopLeftY = static_cast<float>(renderArea.y);
    vp.Width = static_cast<float>(renderArea.width > 0 ? renderArea.width : fboWidth);
    vp.Height = static_cast<float>(renderArea.height > 0 ? renderArea.height : fboHeight);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    _impl->commandList->RSSetViewports(1, &vp);

    D3D12_RECT scissorRect{};
    scissorRect.left = renderArea.x;
    scissorRect.top = renderArea.y;
    scissorRect.right = static_cast<LONG>(renderArea.x + (renderArea.width > 0 ? renderArea.width : fboWidth));
    scissorRect.bottom = static_cast<LONG>(renderArea.y + (renderArea.height > 0 ? renderArea.height : fboHeight));
    _impl->commandList->RSSetScissorRects(1, &scissorRect);

    _impl->inRenderPass = true;
}

void CCD3D12CommandBuffer::endRenderPass() {
    D3D12_RESOURCE_BARRIER postPassBarriers[MAX_PASS_BARRIERS];
    uint32_t postPassBarrierCount = 0;

    // If we transitioned a swapchain back buffer to RENDER_TARGET, transition it back to PRESENT
    if (_impl->activeSwapchainBackBuffer) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = _impl->activeSwapchainBackBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        if (postPassBarrierCount < MAX_PASS_BARRIERS) {
            postPassBarriers[postPassBarrierCount++] = barrier;
        }

        _impl->activeSwapchain = nullptr;
        _impl->activeSwapchainBackBuffer = nullptr;
    }

    // Transition non-swapchain color attachments from RENDER_TARGET to SHADER_RESOURCE
    // (the next pass will likely read them as textures)
    for (const auto &target : _impl->activeColorTargets) {
        auto *d3d12Tex = target.texture;
        if (d3d12Tex && d3d12Tex->isSwapchainColorTexture()) continue;
        auto *resource = target.resource;
        if (!resource) continue;
        const D3D12_RESOURCE_STATES trackedState =
            CCD3D12Texture::getTrackedResourceState(resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const bool shouldTransition = target.hasTextureState
                                          ? (d3d12Tex && d3d12Tex->getCurrentState() == D3D12_RESOURCE_STATE_RENDER_TARGET)
                                          : (trackedState == D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (shouldTransition) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if (postPassBarrierCount < MAX_PASS_BARRIERS) {
                postPassBarriers[postPassBarrierCount++] = barrier;
            }
            if (target.hasTextureState && d3d12Tex) {
                d3d12Tex->setCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            } else {
                CCD3D12Texture::setTrackedResourceState(resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
    }
    _impl->activeColorTargets.clear();

    // Transition depth-stencil back from DEPTH_WRITE
    if (_impl->activeDepthStencil) {
        if (_impl->activeDepthTexture && _impl->activeDepthTexture->getCurrentState() == D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            D3D12_RESOURCE_BARRIER depthBarrier{};
            depthBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            depthBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            depthBarrier.Transition.pResource = _impl->activeDepthStencil;
            depthBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            depthBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            depthBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_READ;
            if (postPassBarrierCount < MAX_PASS_BARRIERS) {
                postPassBarriers[postPassBarrierCount++] = depthBarrier;
            }
            _impl->activeDepthTexture->setCurrentState(D3D12_RESOURCE_STATE_DEPTH_READ);
        }
        _impl->activeDepthStencil = nullptr;
        _impl->activeDepthTexture = nullptr;
    }

    if (postPassBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(postPassBarrierCount, postPassBarriers);
    }

    _impl->inRenderPass = false;
    // D3D12 has no explicit endRenderPass beyond resource barriers
}

void CCD3D12CommandBuffer::insertMarker(const MarkerInfo &marker) {
    (void)marker;
}

void CCD3D12CommandBuffer::beginMarker(const MarkerInfo &marker) {
    (void)marker;
}

void CCD3D12CommandBuffer::endMarker() {
}

void CCD3D12CommandBuffer::execute(CommandBuffer *const *cmdBuffs, uint32_t count) {
    (void)cmdBuffs;
    (void)count;
    // Bundles could be executed here but not needed for PoC
}

void CCD3D12CommandBuffer::bindPipelineState(PipelineState *pso) {
    if (!_impl->commandList || !pso) return;

    auto *d3d12PSO = static_cast<CCD3D12PipelineState *>(pso);
    auto *d3d12PipelineState = static_cast<ID3D12PipelineState *>(d3d12PSO->getID3D12PipelineState());
    if (!d3d12PipelineState) {
        CC_LOG_WARNING("D3D12CommandBuffer::bindPipelineState - PSO handle is null.");
        return;
    }

    _impl->commandList->SetPipelineState(d3d12PipelineState);

    // Apply blend constants from PSO's BlendState.
    // This matches WebGL backend behavior (gl.blendColor) and is required
    // when any target uses CONSTANT_COLOR / CONSTANT_ALPHA blend factors.
    {
        const auto &bs = pso->getBlendState();
        float blendFactor[4] = { bs.blendColor.x, bs.blendColor.y, bs.blendColor.z, bs.blendColor.w };
        _impl->commandList->OMSetBlendFactor(blendFactor);
    }

    // D3D12 treats stencil reference as dynamic command-list state.
    // Passes such as planar-shadow rely on stencilRefFront being active.
    {
        const auto &ds = pso->getDepthStencilState();
        const uint32_t stencilRef = ds.stencilTestFront ? ds.stencilRefFront : ds.stencilRefBack;
        _impl->commandList->OMSetStencilRef(stencilRef);
    }

    // Set primitive topology from PSO
    D3D12_PRIMITIVE_TOPOLOGY topology = static_cast<D3D12_PRIMITIVE_TOPOLOGY>(d3d12PSO->getD3D12PrimitiveTopology());
    _impl->commandList->IASetPrimitiveTopology(topology);

    auto *rootSig = static_cast<ID3D12RootSignature *>(d3d12PSO->getID3D12RootSignature());
    if (rootSig) {
        _impl->commandList->SetGraphicsRootSignature(rootSig);
        // Setting a graphics root signature invalidates root descriptor table
        // assumptions. Re-emit pending descriptor tables before the next draw.
        if (_impl->pendingSetCount > 0) {
            _impl->descriptorSetsDirty = true;
        }
    }

    auto *pipelineLayout = pso->getPipelineLayout();
    if (pipelineLayout && d3d12PSO->usesPipelineLayoutRootSignature()) {
        _impl->boundPipelineLayout = const_cast<PipelineLayout *>(pipelineLayout);
    } else {
        _impl->boundPipelineLayout = nullptr;
    }

    _impl->boundPipelineState = pso;
}

void CCD3D12CommandBuffer::bindDescriptorSet(uint32_t set, DescriptorSet *descriptorSet, uint32_t dynamicOffsetCount, const uint32_t *dynamicOffsets) {
    if (!_impl->commandList || !descriptorSet) return;

    // Defer the actual GPU binding until draw time.
    // D3D12 only allows one CBV/SRV/UAV heap and one Sampler heap bound at a time,
    // so we must collect all sets and flush them together before each draw call.
    auto *d3d12Set = static_cast<CCD3D12DescriptorSet *>(descriptorSet);
    d3d12Set->forceUpdate(); // ensure CPU staging descriptors are up to date
    if (dynamicOffsetCount > 0 && dynamicOffsets) {
        d3d12Set->applyDynamicOffsets(dynamicOffsetCount, dynamicOffsets);
    }

    // Store in pending list (replace if same set index already recorded)
    bool replaced = false;
    for (uint32_t i = 0; i < _impl->pendingSetCount; ++i) {
        if (_impl->pendingSets[i].valid && _impl->pendingSets[i].setIndex == set) {
            _impl->pendingSets[i].set = descriptorSet;
            replaced = true;
            break;
        }
    }
    if (!replaced && _impl->pendingSetCount < D3D12_MAX_BOUND_SETS) {
        _impl->pendingSets[_impl->pendingSetCount].set = descriptorSet;
        _impl->pendingSets[_impl->pendingSetCount].setIndex = set;
        _impl->pendingSets[_impl->pendingSetCount].valid = true;
        ++_impl->pendingSetCount;
    }
    _impl->descriptorSetsDirty = true;
}

void CCD3D12CommandBuffer::flushDescriptorSets() {
    if (!_impl->descriptorSetsDirty || !_impl->commandList) return;
    _impl->descriptorSetsDirty = false;

    auto *device = CCD3D12Device::getInstance();
    if (!device) return;

    auto *boundLayout = static_cast<CCD3D12PipelineLayout *>(_impl->boundPipelineLayout);
    if (!boundLayout) return;

    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    auto *heapPool = device->getGPUDescriptorHeapPool();
    auto *samplerPool = device->getSamplerDescriptorHeapPool();
    if (!d3dDevice || !heapPool) return;

    // Collect all CBV/SRV/UAV and Sampler descriptors across all pending sets,
    // copy them into a single GPU-visible heap allocation per type, then bind once.
    ID3D12DescriptorHeap *boundHeaps[2] = {};
    UINT boundHeapCount = 0;

    // Fixed-size stack arrays — avoid per-draw-call malloc/free.
    // D3D12 root signature allows at most D3D12_MAX_ROOT_COST (64 DWORDs);
    // in practice we bind ≤ 4 descriptor sets × 2 tables = 8 entries each.
    constexpr uint32_t MAX_ROOT_TABLE_ENTRIES = 16;
    struct RootTableEntry {
        UINT rootParameterIndex;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
    };
    RootTableEntry cbvEntries[MAX_ROOT_TABLE_ENTRIES];
    RootTableEntry samplerEntries[MAX_ROOT_TABLE_ENTRIES];
    uint32_t cbvEntryCount = 0;
    uint32_t samplerEntryCount = 0;
    ID3D12DescriptorHeap *cbvHeap = nullptr;
    ID3D12DescriptorHeap *samplerHeap = nullptr;

    struct SetBindingInfo {
        CCD3D12DescriptorSet *set{nullptr};
        uint32_t cbvCount{0};
        uint32_t samplerCount{0};
        int cbvRootIndex{-1};
        int samplerRootIndex{-1};
    };
    SetBindingInfo bindings[D3D12_MAX_BOUND_SETS];
    uint32_t bindingCount = 0;
    uint32_t totalCbvCount = 0;
    uint32_t totalSamplerCount = 0;

    for (uint32_t i = 0; i < _impl->pendingSetCount; ++i) {
        if (!_impl->pendingSets[i].valid || !_impl->pendingSets[i].set) continue;

        const uint32_t setIdx = _impl->pendingSets[i].setIndex;
        auto *d3d12Set = static_cast<CCD3D12DescriptorSet *>(_impl->pendingSets[i].set);

        const auto cbvCount = d3d12Set->getCbvSrvUavDescriptorCount();
        const auto samplerCount = d3d12Set->getSamplerDescriptorCount();
        const auto cbvRootIndex = boundLayout->getCbvSrvUavRootParameterIndex(setIdx);
        const auto samplerRootIndex = boundLayout->getSamplerRootParameterIndex(setIdx);

        if (bindingCount < D3D12_MAX_BOUND_SETS) {
            bindings[bindingCount++] = {d3d12Set, cbvCount, samplerCount, cbvRootIndex, samplerRootIndex};
        }
        if (cbvCount > 0 && cbvRootIndex >= 0) {
            totalCbvCount += cbvCount;
        }
        if (samplerCount > 0 && samplerRootIndex >= 0 && samplerPool) {
            totalSamplerCount += samplerCount;
        }
    }

    D3D12DescriptorHeapPool::Allocation cbvAlloc;
    if (totalCbvCount > 0) {
        cbvAlloc = heapPool->allocate(totalCbvCount);
        if (cbvAlloc.isValid) {
            cbvHeap = static_cast<ID3D12DescriptorHeap *>(heapPool->getHeap(cbvAlloc.heapIndex));
        }
    }

    D3D12DescriptorHeapPool::Allocation samplerAlloc;
    if (totalSamplerCount > 0 && samplerPool) {
        samplerAlloc = samplerPool->allocate(totalSamplerCount);
        if (samplerAlloc.isValid) {
            samplerHeap = static_cast<ID3D12DescriptorHeap *>(samplerPool->getHeap(samplerAlloc.heapIndex));
        }
    }

    uint32_t cbvOffset = 0;
    uint32_t samplerOffset = 0;
    const uint32_t cbvDescriptorSize = heapPool->getDescriptorSize();
    const uint32_t samplerDescriptorSize = samplerPool ? samplerPool->getDescriptorSize() : 0;
    for (uint32_t i = 0; i < bindingCount; ++i) {
        auto &binding = bindings[i];

        if (binding.cbvCount > 0 && binding.cbvRootIndex >= 0 && cbvAlloc.isValid && cbvHeap) {
            auto *srcHeap = static_cast<ID3D12DescriptorHeap *>(binding.set->getCbvSrvUavDescriptorHeap());
            if (srcHeap) {
                D3D12_CPU_DESCRIPTOR_HANDLE srcStart = srcHeap->GetCPUDescriptorHandleForHeapStart();
                D3D12_CPU_DESCRIPTOR_HANDLE dstStart{};
                dstStart.ptr = reinterpret_cast<SIZE_T>(cbvAlloc.cpuHandle) +
                               static_cast<SIZE_T>(cbvOffset) * cbvDescriptorSize;
                d3dDevice->CopyDescriptorsSimple(binding.cbvCount, dstStart, srcStart, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                if (cbvEntryCount < MAX_ROOT_TABLE_ENTRIES) {
                    cbvEntries[cbvEntryCount++] = {
                        static_cast<UINT>(binding.cbvRootIndex),
                        {cbvAlloc.gpuHandle + static_cast<uint64_t>(cbvOffset) * cbvDescriptorSize},
                    };
                }
            }
            cbvOffset += binding.cbvCount;
        }

        if (binding.samplerCount > 0 && binding.samplerRootIndex >= 0 && samplerAlloc.isValid && samplerHeap) {
            auto *srcHeap = static_cast<ID3D12DescriptorHeap *>(binding.set->getSamplerDescriptorHeap());
            if (srcHeap) {
                D3D12_CPU_DESCRIPTOR_HANDLE srcStart = srcHeap->GetCPUDescriptorHandleForHeapStart();
                D3D12_CPU_DESCRIPTOR_HANDLE dstStart{};
                dstStart.ptr = reinterpret_cast<SIZE_T>(samplerAlloc.cpuHandle) +
                               static_cast<SIZE_T>(samplerOffset) * samplerDescriptorSize;
                d3dDevice->CopyDescriptorsSimple(binding.samplerCount, dstStart, srcStart, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                if (samplerEntryCount < MAX_ROOT_TABLE_ENTRIES) {
                    samplerEntries[samplerEntryCount++] = {
                        static_cast<UINT>(binding.samplerRootIndex),
                        {samplerAlloc.gpuHandle + static_cast<uint64_t>(samplerOffset) * samplerDescriptorSize},
                    };
                }
            }
            samplerOffset += binding.samplerCount;
        }
    }

    // Set descriptor heaps ONCE for all sets
    if (cbvHeap) {
        boundHeaps[boundHeapCount++] = cbvHeap;
    }
    if (samplerHeap) {
        // Avoid duplicate if sampler heap is same as CBV heap (shouldn't happen, but be safe)
        bool alreadyBound = false;
        for (UINT h = 0; h < boundHeapCount; ++h) {
            if (boundHeaps[h] == samplerHeap) { alreadyBound = true; break; }
        }
        if (!alreadyBound) {
            boundHeaps[boundHeapCount++] = samplerHeap;
        }
    }

    if (boundHeapCount > 0) {
        _impl->commandList->SetDescriptorHeaps(boundHeapCount, boundHeaps);
    }

    // Set all root descriptor tables
    for (uint32_t i = 0; i < cbvEntryCount; ++i) {
        _impl->commandList->SetGraphicsRootDescriptorTable(cbvEntries[i].rootParameterIndex, cbvEntries[i].gpuHandle);
    }
    for (uint32_t i = 0; i < samplerEntryCount; ++i) {
        _impl->commandList->SetGraphicsRootDescriptorTable(samplerEntries[i].rootParameterIndex, samplerEntries[i].gpuHandle);
    }
}

void CCD3D12CommandBuffer::bindInputAssembler(InputAssembler *ia) {
    if (!_impl->commandList || !ia) return;

    _impl->boundIA = ia;
    auto *d3d12IA = static_cast<CCD3D12InputAssembler *>(ia);

    // Set vertex buffers — use stack array (max vertex attributes = 16 per caps)
    const uint32_t vbCount = d3d12IA->getVertexBufferCount();
    if (vbCount > 0) {
        D3D12_VERTEX_BUFFER_VIEW vbViews[16];
        d3d12IA->fillVertexBufferViews(vbViews);
        _impl->commandList->IASetVertexBuffers(0, vbCount, vbViews);
    }

    // Set index buffer
    if (d3d12IA->hasIndexBuffer()) {
        D3D12_INDEX_BUFFER_VIEW ibView{};
        d3d12IA->fillIndexBufferView(&ibView);
        _impl->commandList->IASetIndexBuffer(&ibView);
    }

    // Note: primitive topology is set in bindPipelineState from PSO info
}

void CCD3D12CommandBuffer::setViewport(const Viewport &vp) {
    if (!_impl->commandList) return;

    D3D12_VIEWPORT d3dViewport{};
    d3dViewport.TopLeftX = static_cast<float>(vp.left);
    d3dViewport.TopLeftY = static_cast<float>(vp.top);
    d3dViewport.Width = static_cast<float>(vp.width);
    d3dViewport.Height = static_cast<float>(vp.height);
    d3dViewport.MinDepth = vp.minDepth;
    d3dViewport.MaxDepth = vp.maxDepth;
    _impl->commandList->RSSetViewports(1, &d3dViewport);
}

void CCD3D12CommandBuffer::setScissor(const Rect &rect) {
    if (!_impl->commandList) return;

    D3D12_RECT d3dRect{};
    d3dRect.left = rect.x;
    d3dRect.top = rect.y;
    d3dRect.right = static_cast<LONG>(rect.x + rect.width);
    d3dRect.bottom = static_cast<LONG>(rect.y + rect.height);
    _impl->commandList->RSSetScissorRects(1, &d3dRect);
}

void CCD3D12CommandBuffer::setLineWidth(float width) {
    (void)width;
    if (width != 1.0f) {
        CC_LOG_WARNING("[D3D12] setLineWidth(%.1f) ignored — D3D12 does not support line width > 1", width);
    }
}

void CCD3D12CommandBuffer::setDepthBias(float constant, float clamp, float slope) {
    (void)constant;
    (void)clamp;
    (void)slope;
    // D3D12 does not support dynamic depth bias via command list.
    // Depth bias is set in the rasterizer state during PSO creation.
}

void CCD3D12CommandBuffer::setBlendConstants(const Color &constants) {
    if (!_impl->commandList) return;
    _impl->commandList->OMSetBlendFactor(&constants.x);
}

void CCD3D12CommandBuffer::setDepthBound(float minBounds, float maxBounds) {
    if (!_impl->commandList) return;
    // OMSetDepthBounds is available on ID3D12GraphicsCommandList1 (D3D12.1+).
    // Query the extended interface; fall back silently if unavailable.
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> cmdList1;
    if (SUCCEEDED(_impl->commandList->QueryInterface(IID_PPV_ARGS(&cmdList1)))) {
        cmdList1->OMSetDepthBounds(minBounds, maxBounds);
    }
}

void CCD3D12CommandBuffer::setStencilWriteMask(StencilFace face, uint32_t mask) {
    (void)face;
    (void)mask;
    CC_LOG_WARNING("[D3D12] setStencilWriteMask(face=%u, mask=0x%x) ignored — set in pipeline state", static_cast<unsigned>(face), mask);
}

void CCD3D12CommandBuffer::setStencilCompareMask(StencilFace face, uint32_t ref, uint32_t mask) {
    if (!_impl->commandList) return;
    (void)face;
    (void)mask;
    _impl->commandList->OMSetStencilRef(ref);
}

void CCD3D12CommandBuffer::nextSubpass() {
    // D3D12 doesn't have explicit subpasses
}

void CCD3D12CommandBuffer::draw(const DrawInfo &info) {
    if (!_impl->commandList) return;

    // Flush any pending descriptor set bindings before drawing
    flushDescriptorSets();

    const uint32_t instanceCount = std::max<uint32_t>(info.instanceCount, 1);
    const uint32_t firstInstance = info.firstInstance;
    auto *d3d12PSO = static_cast<CCD3D12PipelineState *>(_impl->boundPipelineState);

    if (d3d12PSO && d3d12PSO->isDiagnosticFallback()) {
        _impl->commandList->DrawInstanced(3, 1, 0, 0);
        ++_numDrawCalls;
        _numInstances += 1;
        _numTriangles += 1;
        return;
    }

    // Check for indirect draw via InputAssembler's indirect buffer
    if (_impl->boundIA) {
        auto *ia = static_cast<CCD3D12InputAssembler *>(_impl->boundIA);
        Buffer *indirectBuf = ia->getIndirectBuffer();
        if (indirectBuf) {
            auto *d3d12Buf = static_cast<CCD3D12Buffer *>(indirectBuf);
            ID3D12Resource *resource = static_cast<ID3D12Resource *>(d3d12Buf->getD3D12ResourceHandle());
            if (resource) {
                auto *device = CCD3D12Device::getInstance();
                if (info.indexCount > 0) {
                    auto *sig = static_cast<ID3D12CommandSignature *>(device->getDrawIndexedIndirectSignature());
                    if (sig) {
                        _impl->commandList->ExecuteIndirect(sig, 1, resource, 0, nullptr, 0);
                    }
                } else {
                    auto *sig = static_cast<ID3D12CommandSignature *>(device->getDrawIndirectSignature());
                    if (sig) {
                        _impl->commandList->ExecuteIndirect(sig, 1, resource, 0, nullptr, 0);
                    }
                }
                ++_numDrawCalls;
                return;
            }
        }
    }

    if (info.indexCount > 0) {
        // Indexed draw
        _impl->commandList->DrawIndexedInstanced(
            info.indexCount,
            instanceCount,
            info.firstIndex,
            info.vertexOffset,
            firstInstance);
    } else {
        // Non-indexed draw
        _impl->commandList->DrawInstanced(
            info.vertexCount,
            instanceCount,
            info.firstVertex,
            firstInstance);
    }

    ++_numDrawCalls;
    _numInstances += instanceCount;
    _numTriangles += info.indexCount > 0 ? (info.indexCount / 3) * instanceCount : (info.vertexCount / 3) * instanceCount;
}

void CCD3D12CommandBuffer::updateBuffer(Buffer *buff, const void *data, uint32_t size) {
    if (!_impl->commandList || !buff || !data || size == 0) return;

    auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(buff);
    auto *resource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
    if (!resource) return;

    void *mappedData = nullptr;
    D3D12_RANGE readRange{};
    HRESULT hr = resource->Map(0, &readRange, &mappedData);
    if (FAILED(hr) || !mappedData) {
        CC_LOG_ERROR("D3D12CommandBuffer::updateBuffer - Map failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    std::memcpy(mappedData, data, size);
    D3D12_RANGE writeRange{0, size};
    resource->Unmap(0, &writeRange);
}

void CCD3D12CommandBuffer::copyBuffersToTexture(const uint8_t *const *buffers, Texture *texture, const BufferTextureCopy *regions, uint32_t count) {
    if (!_impl->commandList || !buffers || !texture || !regions || count == 0) return;

    auto *d3d12Texture = static_cast<CCD3D12Texture *>(texture);
    auto *textureResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
    if (!textureResource) return;

    auto *device = CCD3D12Device::getInstance();
    if (!device) return;

    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    if (!d3dDevice) return;

    const auto &textureInfo = texture->getInfo();
    const uint32_t bytesPerTexel = GFX_FORMAT_INFOS[toNumber(textureInfo.format)].size;
    if (bytesPerTexel == 0) return;

    // Transition to copy dest
    D3D12_RESOURCE_BARRIER toCopyDest{};
    toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopyDest.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    toCopyDest.Transition.pResource = textureResource;
    toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopyDest.Transition.StateBefore = d3d12Texture->getCurrentState();
    toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    _impl->commandList->ResourceBarrier(1, &toCopyDest);
    d3d12Texture->setCurrentState(D3D12_RESOURCE_STATE_COPY_DEST);

    for (uint32_t i = 0; i < count; ++i) {
        if (!buffers[i]) continue;

        const auto &region = regions[i];
        const uint32_t mipLevel = region.texSubres.mipLevel;
        const uint32_t arrayLayer = textureInfo.type == TextureType::TEX3D ? 0 : region.texSubres.baseArrayLayer;
        const uint32_t subresource = mipLevel + arrayLayer * textureInfo.levelCount;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 uploadSize = 0;
        D3D12_RESOURCE_DESC texDesc = textureResource->GetDesc();
        d3dDevice->GetCopyableFootprints(&texDesc, subresource, 1, 0, &footprint, &rowCount, &rowSizeInBytes, &uploadSize);
        if (uploadSize == 0 || rowCount == 0) continue;

        // Create upload buffer
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Alignment = 0;
        uploadDesc.Width = uploadSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3D12Resource> uploadResource;
        HRESULT hr = d3dDevice->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&uploadResource));
        if (FAILED(hr)) continue;

        void *mappedData = nullptr;
        D3D12_RANGE readRange{};
        hr = uploadResource->Map(0, &readRange, &mappedData);
        if (FAILED(hr) || !mappedData) continue;

        const uint32_t srcRowTexels = region.buffStride > 0 ? region.buffStride : region.texExtent.width;
        const uint32_t srcRows = region.buffTexHeight > 0 ? region.buffTexHeight : region.texExtent.height;
        const uint32_t srcRowPitch = srcRowTexels * bytesPerTexel;
        const uint32_t srcSlicePitch = srcRowPitch * srcRows;
        const uint32_t copyRowBytes = region.texExtent.width * bytesPerTexel;
        const uint32_t copyRows = std::min<uint32_t>(region.texExtent.height, rowCount);
        const uint32_t copyDepth = std::max<uint32_t>(region.texExtent.depth, 1);
        const auto *src = buffers[i] + region.buffOffset;
        auto *dst = static_cast<uint8_t *>(mappedData) + footprint.Offset;

        for (uint32_t z = 0; z < copyDepth; ++z) {
            for (uint32_t row = 0; row < copyRows; ++row) {
                const uint8_t *srcRow = src + z * srcSlicePitch + row * srcRowPitch;
                uint8_t *dstRow = dst + z * footprint.Footprint.RowPitch * rowCount + row * footprint.Footprint.RowPitch;
                std::memcpy(dstRow, srcRow, std::min<uint32_t>(copyRowBytes, static_cast<uint32_t>(rowSizeInBytes)));
            }
        }

        D3D12_RANGE writeRange{0, static_cast<SIZE_T>(uploadSize)};
        uploadResource->Unmap(0, &writeRange);

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = uploadResource.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprint;

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = textureResource;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = subresource;

        D3D12_BOX srcBox{};
        srcBox.left = 0;
        srcBox.top = 0;
        srcBox.front = 0;
        srcBox.right = region.texExtent.width;
        srcBox.bottom = region.texExtent.height;
        srcBox.back = copyDepth;

        _impl->commandList->CopyTextureRegion(&dstLoc, region.texOffset.x, region.texOffset.y, region.texOffset.z, &srcLoc, &srcBox);
        d3d12Texture->markMipLevelUploaded(mipLevel);

        // Transfer ownership to the pending list — keeps resource alive until
        // the next begin() call, by which point the GPU has finished execution.
        _impl->pendingUploadResources.push_back(std::move(uploadResource));
    }

    // Transition back to appropriate state after copy
    D3D12_RESOURCE_STATES postCopyState = getPostTransferTextureState(textureInfo);
    D3D12_RESOURCE_BARRIER toPostCopy{};
    toPostCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPostCopy.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    toPostCopy.Transition.pResource = textureResource;
    toPostCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toPostCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toPostCopy.Transition.StateAfter = postCopyState;
    _impl->commandList->ResourceBarrier(1, &toPostCopy);
    d3d12Texture->setCurrentState(postCopyState);
}

void CCD3D12CommandBuffer::blitTexture(Texture *srcTexture, Texture *dstTexture, const TextureBlit *regions, uint32_t count, Filter filter) {
    if (!_impl->commandList || !srcTexture || !dstTexture || !regions || count == 0) return;

    auto *srcD3D12 = static_cast<CCD3D12Texture *>(srcTexture);
    auto *dstD3D12 = static_cast<CCD3D12Texture *>(dstTexture);
    auto *srcResource = static_cast<ID3D12Resource *>(srcD3D12->getD3D12ResourceHandle());
    auto *dstResource = static_cast<ID3D12Resource *>(dstD3D12->getD3D12ResourceHandle());
    if (!srcResource || !dstResource) return;

    const auto &srcInfo = srcTexture->getInfo();
    const auto &dstInfo = dstTexture->getInfo();

    CC_LOG_INFO("[D3D12] blitTexture: src=%ux%u dst=%ux%u regions=%u filter=%d",
                srcInfo.width, srcInfo.height, dstInfo.width, dstInfo.height,
                static_cast<unsigned>(count), static_cast<int>(filter));

    // Transition src to COPY_SOURCE, dst to COPY_DEST
    D3D12_RESOURCE_BARRIER preBarriers[4];
    uint32_t preBarrierCount = 0;

    D3D12_RESOURCE_STATES srcPrevState = srcD3D12->getCurrentState();
    if (srcPrevState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = srcPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (preBarrierCount < 4) preBarriers[preBarrierCount++] = b;
    }

    D3D12_RESOURCE_STATES dstPrevState = dstD3D12->getCurrentState();
    if (dstPrevState != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = dstPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        if (preBarrierCount < 4) preBarriers[preBarrierCount++] = b;
    }

    if (preBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(preBarrierCount, preBarriers);
    }
    srcD3D12->setCurrentState(D3D12_RESOURCE_STATE_COPY_SOURCE);
    dstD3D12->setCurrentState(D3D12_RESOURCE_STATE_COPY_DEST);

    for (uint32_t i = 0; i < count; ++i) {
        const auto &region = regions[i];

        // Check if scaling is needed
        const bool needsScaling = (region.srcExtent.width != region.dstExtent.width ||
                                    region.srcExtent.height != region.dstExtent.height ||
                                    region.srcExtent.depth != region.dstExtent.depth);

        if (needsScaling) {
            // D3D12 has no native blit with scaling. For now, log a warning and
            // copy the source box to the destination offset without scaling.
            // TODO: Implement full-screen quad render pass for scaled blit.
            CC_LOG_WARNING("[D3D12] blitTexture region %u requires scaling (%ux%ux%u -> %ux%ux%u), "
                           "which is not yet supported. Copying source size.",
                           i, region.srcExtent.width, region.srcExtent.height, region.srcExtent.depth,
                           region.dstExtent.width, region.dstExtent.height, region.dstExtent.depth);
        }

        // Use CopyTextureRegion (same-size copy, no scaling)
        const uint32_t srcSubresource = region.srcSubres.mipLevel +
            region.srcSubres.baseArrayLayer * srcInfo.levelCount;
        const uint32_t dstSubresource = region.dstSubres.mipLevel +
            region.dstSubres.baseArrayLayer * dstInfo.levelCount;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = srcResource;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = srcSubresource;

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = dstResource;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = dstSubresource;

        D3D12_BOX srcBox{};
        srcBox.left = static_cast<UINT>(region.srcOffset.x);
        srcBox.top = static_cast<UINT>(region.srcOffset.y);
        srcBox.front = static_cast<UINT>(region.srcOffset.z);
        srcBox.right = srcBox.left + region.srcExtent.width;
        srcBox.bottom = srcBox.top + region.srcExtent.height;
        srcBox.back = srcBox.front + std::max<uint32_t>(region.srcExtent.depth, 1);

        _impl->commandList->CopyTextureRegion(
            &dstLoc,
            region.dstOffset.x, region.dstOffset.y, region.dstOffset.z,
            &srcLoc,
            &srcBox);
    }

    // Transition back to shader resource
    D3D12_RESOURCE_BARRIER postBarriers[4];
    uint32_t postBarrierCount = 0;

    D3D12_RESOURCE_STATES srcPostState = getPostTransferTextureState(srcInfo);
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = srcPostState;
        if (postBarrierCount < 4) postBarriers[postBarrierCount++] = b;
    }

    D3D12_RESOURCE_STATES dstPostState = getPostTransferTextureState(dstInfo);
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = dstPostState;
        if (postBarrierCount < 4) postBarriers[postBarrierCount++] = b;
    }

    if (postBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(postBarrierCount, postBarriers);
    }
    srcD3D12->setCurrentState(srcPostState);
    dstD3D12->setCurrentState(dstPostState);

    (void)filter; // Filter is not used for same-size copy (D3D12 CopyTextureRegion doesn't support filtering)
}

void CCD3D12CommandBuffer::copyTexture(Texture *srcTexture, Texture *dstTexture, const TextureCopy *regions, uint32_t count) {
    if (!_impl->commandList || !srcTexture || !dstTexture || !regions || count == 0) return;

    auto *srcD3D12 = static_cast<CCD3D12Texture *>(srcTexture);
    auto *dstD3D12 = static_cast<CCD3D12Texture *>(dstTexture);
    auto *srcResource = static_cast<ID3D12Resource *>(srcD3D12->getD3D12ResourceHandle());
    auto *dstResource = static_cast<ID3D12Resource *>(dstD3D12->getD3D12ResourceHandle());
    if (!srcResource || !dstResource) {
        CC_LOG_WARNING("[D3D12] copyTexture: null resource (src=%p dst=%p)", srcResource, dstResource);
        return;
    }

    const auto &srcInfo = srcTexture->getInfo();
    const auto &dstInfo = dstTexture->getInfo();

    CC_LOG_INFO("[D3D12] copyTexture: src=%ux%u dst=%ux%u regions=%u",
                srcInfo.width, srcInfo.height, dstInfo.width, dstInfo.height, count);

    // Transition src to COPY_SOURCE, dst to COPY_DEST
    D3D12_RESOURCE_BARRIER preBarriers[4];
    uint32_t preBarrierCount = 0;

    D3D12_RESOURCE_STATES srcPrevState = srcD3D12->getCurrentState();
    if (srcPrevState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = srcPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (preBarrierCount < 4) preBarriers[preBarrierCount++] = b;
    }

    D3D12_RESOURCE_STATES dstPrevState = dstD3D12->getCurrentState();
    if (dstPrevState != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = dstPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        if (preBarrierCount < 4) preBarriers[preBarrierCount++] = b;
    }

    if (preBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(preBarrierCount, preBarriers);
    }
    srcD3D12->setCurrentState(D3D12_RESOURCE_STATE_COPY_SOURCE);
    dstD3D12->setCurrentState(D3D12_RESOURCE_STATE_COPY_DEST);

    // Perform texture-to-texture copy for each region
    for (uint32_t i = 0; i < count; ++i) {
        const auto &region = regions[i];

        // Calculate D3D12 subresource indices:
        // subresource = mipLevel + baseArrayLayer * mipLevelCount
        const uint32_t srcSubresource = region.srcSubres.mipLevel +
            region.srcSubres.baseArrayLayer * srcInfo.levelCount;
        const uint32_t dstSubresource = region.dstSubres.mipLevel +
            region.dstSubres.baseArrayLayer * dstInfo.levelCount;

        // Build source copy location (subresource index)
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = srcResource;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = srcSubresource;

        // Build destination copy location (subresource index)
        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = dstResource;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = dstSubresource;

        // Build source box (optional — if null, copies entire subresource)
        D3D12_BOX srcBox{};
        srcBox.left = static_cast<UINT>(region.srcOffset.x);
        srcBox.top = static_cast<UINT>(region.srcOffset.y);
        srcBox.front = static_cast<UINT>(region.srcOffset.z);
        srcBox.right = srcBox.left + region.extent.width;
        srcBox.bottom = srcBox.top + region.extent.height;
        srcBox.back = srcBox.front + std::max<uint32_t>(region.extent.depth, 1);

        _impl->commandList->CopyTextureRegion(
            &dstLoc,
            region.dstOffset.x, region.dstOffset.y, region.dstOffset.z,
            &srcLoc,
            &srcBox);
    }

    // Transition src and dst back to reasonable states after copy
    D3D12_RESOURCE_BARRIER postBarriers[4];
    uint32_t postBarrierCount = 0;

    // Src back to shader resource
    D3D12_RESOURCE_STATES srcPostState = getPostTransferTextureState(srcInfo);
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = srcPostState;
        if (postBarrierCount < 4) postBarriers[postBarrierCount++] = b;
    }

    // Dst back to shader resource (or render target)
    D3D12_RESOURCE_STATES dstPostState = getPostTransferTextureState(dstInfo);
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = dstPostState;
        if (postBarrierCount < 4) postBarriers[postBarrierCount++] = b;
    }

    if (postBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(postBarrierCount, postBarriers);
    }
    srcD3D12->setCurrentState(srcPostState);
    dstD3D12->setCurrentState(dstPostState);
}

void CCD3D12CommandBuffer::resolveTexture(Texture *srcTexture, Texture *dstTexture, const TextureCopy *regions, uint32_t count) {
    if (!_impl->commandList || !srcTexture || !dstTexture || !regions || count == 0) return;

    auto *srcD3D12 = static_cast<CCD3D12Texture *>(srcTexture);
    auto *dstD3D12 = static_cast<CCD3D12Texture *>(dstTexture);
    auto *srcResource = static_cast<ID3D12Resource *>(srcD3D12->getD3D12ResourceHandle());
    auto *dstResource = static_cast<ID3D12Resource *>(dstD3D12->getD3D12ResourceHandle());
    if (!srcResource || !dstResource) return;

    const auto &srcInfo = srcTexture->getInfo();
    const auto &dstInfo = dstTexture->getInfo();

    // Verify src is MSAA and dst is non-MSAA
    const bool srcIsMSAA = (srcInfo.samples != SampleCount::X1);
    if (!srcIsMSAA) {
        CC_LOG_WARNING("[D3D12] resolveTexture: source is not MSAA (samples=%u), falling back to copyTexture",
                       srcInfo.samples);
        // Fall back to regular copy for non-MSAA sources
        copyTexture(srcTexture, dstTexture, regions, count);
        return;
    }

    CC_LOG_INFO("[D3D12] resolveTexture: src=%ux%u(msaa=%u) dst=%ux%u regions=%u",
                srcInfo.width, srcInfo.height, srcInfo.samples,
                dstInfo.width, dstInfo.height, count);

    // Transition src to RESOLVE_SOURCE, dst to RESOLVE_DEST
    D3D12_RESOURCE_BARRIER preBarriers[4];
    uint32_t preBarrierCount = 0;

    D3D12_RESOURCE_STATES srcPrevState = srcD3D12->getCurrentState();
    if (srcPrevState != D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = srcPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        if (preBarrierCount < 4) preBarriers[preBarrierCount++] = b;
    }

    D3D12_RESOURCE_STATES dstPrevState = dstD3D12->getCurrentState();
    if (dstPrevState != D3D12_RESOURCE_STATE_RESOLVE_DEST) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = dstPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        if (preBarrierCount < 4) preBarriers[preBarrierCount++] = b;
    }

    if (preBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(preBarrierCount, preBarriers);
    }
    srcD3D12->setCurrentState(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    dstD3D12->setCurrentState(D3D12_RESOURCE_STATE_RESOLVE_DEST);

    // ResolveSubresource operates on a single subresource pair at a time.
    // For each region, resolve the corresponding mip+layer combination.
    for (uint32_t i = 0; i < count; ++i) {
        const auto &region = regions[i];
        const uint32_t srcSubresource = region.srcSubres.mipLevel +
            region.srcSubres.baseArrayLayer * srcInfo.levelCount;
        const uint32_t dstSubresource = region.dstSubres.mipLevel +
            region.dstSubres.baseArrayLayer * dstInfo.levelCount;

        // ResolveSubresource requires a DXGI format when the source and destination
        // formats differ (format conversion resolve). For same-format resolves, pass
        // DXGI_FORMAT_UNKNOWN which lets the runtime use the source format.
        _impl->commandList->ResolveSubresource(
            dstResource, dstSubresource,
            srcResource, srcSubresource,
            DXGI_FORMAT_UNKNOWN);
    }

    // Transition back to shader resource after resolve
    D3D12_RESOURCE_BARRIER postBarriers[4];
    uint32_t postBarrierCount = 0;

    {
        D3D12_RESOURCE_STATES srcPostState = getPostTransferTextureState(srcInfo);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        b.Transition.StateAfter = srcPostState;
        if (postBarrierCount < 4) postBarriers[postBarrierCount++] = b;
        srcD3D12->setCurrentState(srcPostState);
    }
    {
        D3D12_RESOURCE_STATES dstPostState = getPostTransferTextureState(dstInfo);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        b.Transition.StateAfter = dstPostState;
        if (postBarrierCount < 4) postBarriers[postBarrierCount++] = b;
        dstD3D12->setCurrentState(dstPostState);
    }

    if (postBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(postBarrierCount, postBarriers);
    }
}

void CCD3D12CommandBuffer::dispatch(const DispatchInfo &info) {
    if (!_impl->commandList) return;
    // Flush any pending descriptor set bindings before dispatching
    flushDescriptorSets();

    if (info.indirectBuffer) {
        // Indirect dispatch — arguments come from a GPU buffer
        auto *d3d12Buf = static_cast<CCD3D12Buffer *>(info.indirectBuffer);
        ID3D12Resource *resource = static_cast<ID3D12Resource *>(d3d12Buf->getD3D12ResourceHandle());
        if (resource) {
            auto *sig = static_cast<ID3D12CommandSignature *>(
                CCD3D12Device::getInstance()->getDispatchIndirectSignature());
            if (sig) {
                _impl->commandList->ExecuteIndirect(sig, 1, resource, info.indirectOffset, nullptr, 0);
            }
        }
    } else {
        _impl->commandList->Dispatch(info.groupCountX, info.groupCountY, info.groupCountZ);
    }
}

void CCD3D12CommandBuffer::pipelineBarrier(const GeneralBarrier *barrier, const BufferBarrier *const *bufferBarriers, const Buffer *const *buffers, uint32_t bufferCount, const TextureBarrier *const *textureBarriers, const Texture *const *textures, uint32_t textureBarrierCount) {
    if (!_impl->commandList) return;

    // Map Cocos AccessFlags to D3D12_RESOURCE_STATES
    auto accessFlagsToD3D12State = [](AccessFlags access) -> D3D12_RESOURCE_STATES {
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        if (hasFlag(access, AccessFlagBit::COLOR_ATTACHMENT_WRITE) ||
            hasFlag(access, AccessFlagBit::COLOR_ATTACHMENT_READ)) {
            state |= D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        if (hasFlag(access, AccessFlagBit::DEPTH_STENCIL_ATTACHMENT_WRITE) ||
            hasFlag(access, AccessFlagBit::DEPTH_STENCIL_ATTACHMENT_READ)) {
            state |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }
        if (hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE) ||
            hasFlag(access, AccessFlagBit::VERTEX_SHADER_READ_TEXTURE) ||
            hasFlag(access, AccessFlagBit::COMPUTE_SHADER_READ_TEXTURE)) {
            state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        if (hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_READ_COLOR_INPUT_ATTACHMENT)) {
            state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        if (hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_READ_DEPTH_STENCIL_INPUT_ATTACHMENT)) {
            state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        if (hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_WRITE) ||
            hasFlag(access, AccessFlagBit::COMPUTE_SHADER_WRITE)) {
            state |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (hasFlag(access, AccessFlagBit::VERTEX_SHADER_READ_UNIFORM_BUFFER) ||
            hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_READ_UNIFORM_BUFFER) ||
            hasFlag(access, AccessFlagBit::COMPUTE_SHADER_READ_UNIFORM_BUFFER)) {
            state |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        }
        if (hasFlag(access, AccessFlagBit::INDEX_BUFFER)) {
            state |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
        }
        if (hasFlag(access, AccessFlagBit::VERTEX_BUFFER)) {
            state |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        }
        if (hasFlag(access, AccessFlagBit::INDIRECT_BUFFER)) {
            state |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        }
        if (hasFlag(access, AccessFlagBit::TRANSFER_READ)) {
            state |= D3D12_RESOURCE_STATE_COPY_SOURCE;
        }
        if (hasFlag(access, AccessFlagBit::TRANSFER_WRITE)) {
            state |= D3D12_RESOURCE_STATE_COPY_DEST;
        }
        if (hasFlag(access, AccessFlagBit::PRESENT)) {
            state = D3D12_RESOURCE_STATE_PRESENT;
        }
        if (hasFlag(access, AccessFlagBit::VERTEX_SHADER_READ_OTHER) ||
            hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_READ_OTHER) ||
            hasFlag(access, AccessFlagBit::COMPUTE_SHADER_READ_OTHER)) {
            state |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        return state;
    };

    ccstd::vector<D3D12_RESOURCE_BARRIER> barriers;

    // Process texture barriers
    for (uint32_t i = 0; i < textureBarrierCount; ++i) {
        if (!textureBarriers[i] || !textures[i]) continue;

        const auto &texBarrierInfo = textureBarriers[i]->getInfo();
        auto *d3d12Texture = static_cast<CCD3D12Texture *>(const_cast<Texture *>(textures[i]));
        if (!d3d12Texture) continue;

        auto *resource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
        if (!resource) continue;

        // Skip swapchain textures only during a render pass — their barriers are
        // managed by beginRenderPass/endRenderPass. Outside a render pass, allow
        // barriers for swapchain textures (e.g., for copy operations).
        if (d3d12Texture->isSwapchainColorTexture() && _impl->inRenderPass) continue;

        D3D12_RESOURCE_STATES prevState = accessFlagsToD3D12State(texBarrierInfo.prevAccesses);
        D3D12_RESOURCE_STATES nextState = accessFlagsToD3D12State(texBarrierInfo.nextAccesses);

        // If no explicit prevAccesses, use tracked state
        if (texBarrierInfo.prevAccesses == AccessFlagBit::NONE) {
            prevState = d3d12Texture->getCurrentState();
        }

        // If states match, no barrier needed
        if (prevState == nextState) continue;

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = prevState;
        b.Transition.StateAfter = nextState;

        barriers.push_back(b);

        // Update tracked state
        d3d12Texture->setCurrentState(nextState);
    }

    // Process buffer barriers — for D3D12 these are mainly UAV barriers
    for (uint32_t i = 0; i < bufferCount; ++i) {
        if (!bufferBarriers[i] || !buffers[i]) continue;

        const auto &bufBarrierInfo = bufferBarriers[i]->getInfo();
        // If a buffer is written then read, we need a UAV barrier
        if (hasFlag(bufBarrierInfo.prevAccesses, AccessFlagBit::FRAGMENT_SHADER_WRITE) ||
            hasFlag(bufBarrierInfo.prevAccesses, AccessFlagBit::COMPUTE_SHADER_WRITE) ||
            hasFlag(bufBarrierInfo.prevAccesses, AccessFlagBit::TRANSFER_WRITE)) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            b.UAV.pResource = nullptr; // nullptr means all UAV resources
            barriers.push_back(b);
            break; // One global UAV barrier is sufficient
        }
    }

    if (!barriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }

    (void)barrier;
}

void CCD3D12CommandBuffer::beginQuery(QueryPool *queryPool, uint32_t id) {
    if (!_impl || !_impl->commandList) return;
    auto *d3d12Pool = static_cast<CCD3D12QueryPool *>(queryPool);
    auto *heap = static_cast<ID3D12QueryHeap *>(d3d12Pool->getD3D12QueryHeap());
    if (!heap) return;
    D3D12_QUERY_TYPE queryType = (d3d12Pool->getType() == QueryType::OCCLUSION)
                                     ? D3D12_QUERY_TYPE_OCCLUSION
                                     : D3D12_QUERY_TYPE_TIMESTAMP;
    _impl->commandList->BeginQuery(heap, queryType, id);
}

void CCD3D12CommandBuffer::endQuery(QueryPool *queryPool, uint32_t id) {
    if (!_impl || !_impl->commandList) return;
    auto *d3d12Pool = static_cast<CCD3D12QueryPool *>(queryPool);
    auto *heap = static_cast<ID3D12QueryHeap *>(d3d12Pool->getD3D12QueryHeap());
    if (!heap) return;
    D3D12_QUERY_TYPE queryType = (d3d12Pool->getType() == QueryType::OCCLUSION)
                                     ? D3D12_QUERY_TYPE_OCCLUSION
                                     : D3D12_QUERY_TYPE_TIMESTAMP;
    _impl->commandList->EndQuery(heap, queryType, id);
}

void CCD3D12CommandBuffer::resetQueryPool(QueryPool *queryPool) {
    if (!_impl || !_impl->commandList) return;
    auto *d3d12Pool = static_cast<CCD3D12QueryPool *>(queryPool);
    auto *heap = static_cast<ID3D12QueryHeap *>(d3d12Pool->getD3D12QueryHeap());
    if (!heap) return;
    // D3D12 doesn't have a direct "reset query pool" on command list.
    // The results are overwritten on next BeginQuery/EndQuery cycle.
    // We clear the CPU-side results here.
    (void)heap;
}

void CCD3D12CommandBuffer::customCommand(CustomCommand &&cmd) {
    if (cmd && _impl && _impl->commandList) {
        cmd(static_cast<void *>(_impl->commandList.Get()));
    }
}

void *CCD3D12CommandBuffer::getD3D12CommandList() const {
    return _impl ? _impl->commandList.Get() : nullptr;
}

} // namespace gfx
} // namespace cc
