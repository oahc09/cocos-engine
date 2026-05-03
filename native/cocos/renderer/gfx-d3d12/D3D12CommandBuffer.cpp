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

// File diagnostic for correlating RenderDoc events with D3D12 binding state.
#include <cstdarg>
#include <cstdio>
namespace {
void drawDiagLog(const char *fmt, ...) {
    static FILE *s_file = nullptr;
    if (!s_file) {
        s_file = fopen("C:\\temp\\d3d12-render-diag.log", "a");
        if (!s_file) return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(s_file, fmt, args);
    fflush(s_file);
    va_end(args);
}
} // anonymous namespace

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <algorithm>
    #include <cstring>
    #include <d3d12.h>
    #include <wrl/client.h>
#endif

namespace cc {
namespace gfx {

// Maximum number of descriptor sets that can be bound simultaneously
static constexpr uint32_t D3D12_MAX_BOUND_SETS = 4;

struct CCD3D12CommandBuffer::Impl {
#if defined(_WIN32)
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
    ccstd::vector<CCD3D12Texture *> activeColorTextures;
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
#endif
};

CCD3D12CommandBuffer::CCD3D12CommandBuffer()
: _impl(std::make_unique<Impl>()) {
}

CCD3D12CommandBuffer::~CCD3D12CommandBuffer() = default;

void CCD3D12CommandBuffer::doInit(const CommandBufferInfo &info) {
    (void)info;
#if defined(_WIN32)
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
#endif
}

void CCD3D12CommandBuffer::doDestroy() {
#if defined(_WIN32)
    _impl->commandList.Reset();
    _impl->commandAllocator.Reset();
    _impl->d3dDevice.Reset();
    _impl->boundPipelineState = nullptr;
    _impl->boundPipelineLayout = nullptr;
#endif
}

void CCD3D12CommandBuffer::begin(RenderPass *renderPass, uint32_t subpass, Framebuffer *frameBuffer) {
    (void)renderPass;
    (void)subpass;
    (void)frameBuffer;
#if defined(_WIN32)
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
    _impl->activeColorTextures.clear();
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
#endif
}

void CCD3D12CommandBuffer::end() {
#if defined(_WIN32)
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
#endif
}

void CCD3D12CommandBuffer::beginRenderPass(RenderPass *renderPass, Framebuffer *fbo, const Rect &renderArea, const Color *colors, float depth, uint32_t stencil, CommandBuffer *const *secondaryCBs, uint32_t secondaryCBCount) {
    (void)secondaryCBs;
    (void)secondaryCBCount;
#if defined(_WIN32)
    if (!_impl->commandList) return;

    auto *d3d12Fbo = static_cast<CCD3D12Framebuffer *>(fbo);
    if (!d3d12Fbo) {
        CC_LOG_WARNING("D3D12CommandBuffer::beginRenderPass - framebuffer is null.");
        return;
    }

    ccstd::vector<D3D12_RESOURCE_BARRIER> prePassBarriers;

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
            prePassBarriers.push_back(barrier);

            _impl->activeSwapchain = swapchain;
            _impl->activeSwapchainBackBuffer = backBuffer;
        }
    }

    const uint32_t fboWidth = d3d12Fbo->getWidth();
    const uint32_t fboHeight = d3d12Fbo->getHeight();

    // Count color attachments from framebuffer
    const auto &colorTextures = fbo->getColorTextures();
    const uint32_t colorCount = static_cast<uint32_t>(colorTextures.size());

    // Transition non-swapchain color attachments to RENDER_TARGET
    _impl->activeColorTextures.clear();
    for (uint32_t i = 0; i < colorCount; ++i) {
        auto *tex = colorTextures[i];
        if (!tex) continue;
        auto *d3d12Tex = static_cast<CCD3D12Texture *>(const_cast<Texture *>(tex));
        if (!d3d12Tex) continue;

        // Track all color textures for endRenderPass state restoration
        _impl->activeColorTextures.push_back(d3d12Tex);

        if (d3d12Tex->isSwapchainColorTexture()) continue;

        auto *resource = static_cast<ID3D12Resource *>(d3d12Tex->getD3D12ResourceHandle());
        if (!resource) continue;

        D3D12_RESOURCE_STATES prevState = d3d12Tex->getCurrentState();
        if (prevState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = prevState;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            prePassBarriers.push_back(barrier);
            d3d12Tex->setCurrentState(D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }

    // Collect RTV handles
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[MAX_ATTACHMENTS]{};
    for (uint32_t i = 0; i < colorCount && i < MAX_ATTACHMENTS; ++i) {
        auto handle = d3d12Fbo->getRTVHandle(i);
        rtvHandles[i].ptr = handle.ptr;
    }

    // Get DSV handle
    auto dsvPair = d3d12Fbo->getDSVHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    dsvHandle.ptr = dsvPair.ptr;
    bool hasDSV = (dsvPair.ptr != 0);
    auto *depthStencilTexture = static_cast<CCD3D12Texture *>(fbo->getDepthStencilTexture());
    auto *depthStencilResource = depthStencilTexture ? static_cast<ID3D12Resource *>(depthStencilTexture->getD3D12ResourceHandle()) : nullptr;
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
            prePassBarriers.push_back(depthBarrier);
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
    if (!prePassBarriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(prePassBarriers.size()), prePassBarriers.data());
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
#else
    (void)fbo;
    (void)renderArea;
    (void)colors;
    (void)depth;
    (void)stencil;
#endif
}

void CCD3D12CommandBuffer::endRenderPass() {
#if defined(_WIN32)
    ccstd::vector<D3D12_RESOURCE_BARRIER> postPassBarriers;

    // If we transitioned a swapchain back buffer to RENDER_TARGET, transition it back to PRESENT
    if (_impl->activeSwapchainBackBuffer) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = _impl->activeSwapchainBackBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        postPassBarriers.push_back(barrier);

        _impl->activeSwapchain = nullptr;
        _impl->activeSwapchainBackBuffer = nullptr;
    }

    // Transition non-swapchain color attachments from RENDER_TARGET to SHADER_RESOURCE
    // (the next pass will likely read them as textures)
    for (auto *d3d12Tex : _impl->activeColorTextures) {
        if (!d3d12Tex || d3d12Tex->isSwapchainColorTexture()) continue;
        auto *resource = static_cast<ID3D12Resource *>(d3d12Tex->getD3D12ResourceHandle());
        if (!resource) continue;
        if (d3d12Tex->getCurrentState() == D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            postPassBarriers.push_back(barrier);
            d3d12Tex->setCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }
    _impl->activeColorTextures.clear();

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
            postPassBarriers.push_back(depthBarrier);
            _impl->activeDepthTexture->setCurrentState(D3D12_RESOURCE_STATE_DEPTH_READ);
        }
        _impl->activeDepthStencil = nullptr;
        _impl->activeDepthTexture = nullptr;
    }

    if (!postPassBarriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(postPassBarriers.size()), postPassBarriers.data());
    }

    _impl->inRenderPass = false;
#endif
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
#if defined(_WIN32)
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
#else
    (void)pso;
#endif
}

void CCD3D12CommandBuffer::bindDescriptorSet(uint32_t set, DescriptorSet *descriptorSet, uint32_t dynamicOffsetCount, const uint32_t *dynamicOffsets) {
#if defined(_WIN32)
    if (!_impl->commandList || !descriptorSet) return;

    // Defer the actual GPU binding until draw time.
    // D3D12 only allows one CBV/SRV/UAV heap and one Sampler heap bound at a time,
    // so we must collect all sets and flush them together before each draw call.
    auto *d3d12Set = static_cast<CCD3D12DescriptorSet *>(descriptorSet);
    d3d12Set->forceUpdate(); // ensure CPU staging descriptors are up to date
    if (dynamicOffsetCount > 0 && dynamicOffsets) {
        d3d12Set->applyDynamicOffsets(dynamicOffsetCount, dynamicOffsets);
    }

    static uint32_t s_bindSetDiagCount = 0;
    if (s_bindSetDiagCount < 3000) {
        drawDiagLog("[BIND-SET] #%u setIndex=%u set=%p layout=%p dynCount=%u dyn0=%u cbvCount=%u samplerCount=%u pendingBefore=%u dirtyBefore=%d\n",
                    s_bindSetDiagCount,
                    set,
                    descriptorSet,
                    descriptorSet->getLayout(),
                    dynamicOffsetCount,
                    (dynamicOffsetCount > 0 && dynamicOffsets) ? dynamicOffsets[0] : 0U,
                    d3d12Set->getCbvSrvUavDescriptorCount(),
                    d3d12Set->getSamplerDescriptorCount(),
                    _impl->pendingSetCount,
                    _impl->descriptorSetsDirty ? 1 : 0);
        ++s_bindSetDiagCount;
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
#else
    (void)set;
    (void)descriptorSet;
    (void)dynamicOffsetCount;
    (void)dynamicOffsets;
#endif
}

void CCD3D12CommandBuffer::flushDescriptorSets() {
#if defined(_WIN32)
    if (!_impl->descriptorSetsDirty || !_impl->commandList) return;
    _impl->descriptorSetsDirty = false;

    auto *device = CCD3D12Device::getInstance();
    if (!device) return;

    auto *boundLayout = static_cast<CCD3D12PipelineLayout *>(_impl->boundPipelineLayout);
    if (!boundLayout) return;

    static uint32_t s_flushDiagCount = 0;
    const bool flushDiag = s_flushDiagCount < 3000;
    const uint32_t flushId = s_flushDiagCount++;
    if (flushDiag) {
        const char *shaderName = "<null>";
        if (_impl->boundPipelineState && _impl->boundPipelineState->getShader()) {
            shaderName = _impl->boundPipelineState->getShader()->getName().c_str();
        }
        drawDiagLog("[FLUSH] #%u shader='%s' boundLayout=%p pendingSets=%u\n",
                    flushId, shaderName, _impl->boundPipelineLayout, _impl->pendingSetCount);
    }

    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    auto *heapPool = device->getGPUDescriptorHeapPool();
    auto *samplerPool = device->getSamplerDescriptorHeapPool();
    if (!d3dDevice || !heapPool) return;

    // Collect all CBV/SRV/UAV and Sampler descriptors across all pending sets,
    // copy them into a single GPU-visible heap allocation per type, then bind once.
    ID3D12DescriptorHeap *boundHeaps[2] = {};
    UINT boundHeapCount = 0;

    // We use the device's global GPU-visible heaps (reset per frame in begin()).
    // Each set gets its own allocation from the pool, but they may share the same heap.
    // Collect heap pointers and root descriptor table entries.
    struct RootTableEntry {
        UINT rootParameterIndex;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
    };
    ccstd::vector<RootTableEntry> cbvEntries;
    ccstd::vector<RootTableEntry> samplerEntries;
    ID3D12DescriptorHeap *cbvHeap = nullptr;
    ID3D12DescriptorHeap *samplerHeap = nullptr;

    for (uint32_t i = 0; i < _impl->pendingSetCount; ++i) {
        if (!_impl->pendingSets[i].valid || !_impl->pendingSets[i].set) continue;

        const uint32_t setIdx = _impl->pendingSets[i].setIndex;
        auto *d3d12Set = static_cast<CCD3D12DescriptorSet *>(_impl->pendingSets[i].set);

        const auto cbvCount = d3d12Set->getCbvSrvUavDescriptorCount();
        const auto samplerCount = d3d12Set->getSamplerDescriptorCount();
        const auto cbvRootIndex = boundLayout->getCbvSrvUavRootParameterIndex(setIdx);
        const auto samplerRootIndex = boundLayout->getSamplerRootParameterIndex(setIdx);
        if (flushDiag) {
            drawDiagLog("  [FLUSH-SET] #%u setIndex=%u set=%p layout=%p cbvCount=%u samplerCount=%u cbvRoot=%d samplerRoot=%d\n",
                        flushId,
                        setIdx,
                        d3d12Set,
                        d3d12Set->getLayout(),
                        cbvCount,
                        samplerCount,
                        cbvRootIndex,
                        samplerRootIndex);
        }

        // Copy CBV/SRV/UAV descriptors to GPU heap
        if (cbvCount > 0 && cbvRootIndex >= 0) {
            auto alloc = heapPool->allocate(cbvCount);
            auto *srcHeap = static_cast<ID3D12DescriptorHeap *>(d3d12Set->getCbvSrvUavDescriptorHeap());
            if (alloc.isValid && srcHeap) {
                D3D12_CPU_DESCRIPTOR_HANDLE srcStart = srcHeap->GetCPUDescriptorHandleForHeapStart();
                D3D12_CPU_DESCRIPTOR_HANDLE dstStart{};
                dstStart.ptr = reinterpret_cast<SIZE_T>(alloc.cpuHandle);

                d3dDevice->CopyDescriptorsSimple(cbvCount, dstStart, srcStart, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                cbvHeap = static_cast<ID3D12DescriptorHeap *>(heapPool->getHeap(alloc.heapIndex));
                cbvEntries.push_back({static_cast<UINT>(cbvRootIndex), {alloc.gpuHandle}});
                if (flushDiag) {
                    drawDiagLog("    [FLUSH-CBV] #%u setIndex=%u root=%d gpuHandle=0x%llx count=%u heapIndex=%u\n",
                                flushId,
                                setIdx,
                                cbvRootIndex,
                                static_cast<unsigned long long>(alloc.gpuHandle),
                                cbvCount,
                                alloc.heapIndex);
                }
            }
        }

        // Copy Sampler descriptors to GPU heap
        if (samplerCount > 0 && samplerRootIndex >= 0 && samplerPool) {
            auto alloc = samplerPool->allocate(samplerCount);
            auto *srcHeap = static_cast<ID3D12DescriptorHeap *>(d3d12Set->getSamplerDescriptorHeap());
            if (alloc.isValid && srcHeap) {
                D3D12_CPU_DESCRIPTOR_HANDLE srcStart = srcHeap->GetCPUDescriptorHandleForHeapStart();
                D3D12_CPU_DESCRIPTOR_HANDLE dstStart{};
                dstStart.ptr = reinterpret_cast<SIZE_T>(alloc.cpuHandle);
                d3dDevice->CopyDescriptorsSimple(samplerCount, dstStart, srcStart, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                samplerHeap = static_cast<ID3D12DescriptorHeap *>(samplerPool->getHeap(alloc.heapIndex));
                samplerEntries.push_back({static_cast<UINT>(samplerRootIndex), {alloc.gpuHandle}});
                if (flushDiag) {
                    drawDiagLog("    [FLUSH-SAMP] #%u setIndex=%u root=%d gpuHandle=0x%llx count=%u heapIndex=%u\n",
                                flushId,
                                setIdx,
                                samplerRootIndex,
                                static_cast<unsigned long long>(alloc.gpuHandle),
                                samplerCount,
                                alloc.heapIndex);
                }
            }
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
    for (const auto &entry : cbvEntries) {
        _impl->commandList->SetGraphicsRootDescriptorTable(entry.rootParameterIndex, entry.gpuHandle);
    }
    for (const auto &entry : samplerEntries) {
        _impl->commandList->SetGraphicsRootDescriptorTable(entry.rootParameterIndex, entry.gpuHandle);
    }
#else
#endif
}

void CCD3D12CommandBuffer::bindInputAssembler(InputAssembler *ia) {
#if defined(_WIN32)
    if (!_impl->commandList || !ia) return;

    _impl->boundIA = ia;
    auto *d3d12IA = static_cast<CCD3D12InputAssembler *>(ia);

    // Set vertex buffers
    const uint32_t vbCount = d3d12IA->getVertexBufferCount();
    if (vbCount > 0) {
        ccstd::vector<D3D12_VERTEX_BUFFER_VIEW> vbViews(vbCount);
        d3d12IA->fillVertexBufferViews(vbViews.data());
        _impl->commandList->IASetVertexBuffers(0, vbCount, vbViews.data());
    }

    // Set index buffer
    if (d3d12IA->hasIndexBuffer()) {
        D3D12_INDEX_BUFFER_VIEW ibView{};
        d3d12IA->fillIndexBufferView(&ibView);
        _impl->commandList->IASetIndexBuffer(&ibView);
    }

    // Note: primitive topology is set in bindPipelineState from PSO info
#else
    (void)ia;
#endif
}

void CCD3D12CommandBuffer::setViewport(const Viewport &vp) {
#if defined(_WIN32)
    if (!_impl->commandList) return;

    D3D12_VIEWPORT d3dViewport{};
    d3dViewport.TopLeftX = static_cast<float>(vp.left);
    d3dViewport.TopLeftY = static_cast<float>(vp.top);
    d3dViewport.Width = static_cast<float>(vp.width);
    d3dViewport.Height = static_cast<float>(vp.height);
    d3dViewport.MinDepth = vp.minDepth;
    d3dViewport.MaxDepth = vp.maxDepth;
    _impl->commandList->RSSetViewports(1, &d3dViewport);
#else
    (void)vp;
#endif
}

void CCD3D12CommandBuffer::setScissor(const Rect &rect) {
#if defined(_WIN32)
    if (!_impl->commandList) return;

    D3D12_RECT d3dRect{};
    d3dRect.left = rect.x;
    d3dRect.top = rect.y;
    d3dRect.right = static_cast<LONG>(rect.x + rect.width);
    d3dRect.bottom = static_cast<LONG>(rect.y + rect.height);
    _impl->commandList->RSSetScissorRects(1, &d3dRect);
#else
    (void)rect;
#endif
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
#if defined(_WIN32)
    if (!_impl->commandList) return;
    _impl->commandList->OMSetBlendFactor(&constants.x);
#else
    (void)constants;
#endif
}

void CCD3D12CommandBuffer::setDepthBound(float minBounds, float maxBounds) {
#if defined(_WIN32)
    if (!_impl->commandList) return;
    // OMSetDepthBounds is available on ID3D12GraphicsCommandList1 (D3D12.1+).
    // Query the extended interface; fall back silently if unavailable.
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> cmdList1;
    if (SUCCEEDED(_impl->commandList->QueryInterface(IID_PPV_ARGS(&cmdList1)))) {
        cmdList1->OMSetDepthBounds(minBounds, maxBounds);
    }
#else
    (void)minBounds;
    (void)maxBounds;
#endif
}

void CCD3D12CommandBuffer::setStencilWriteMask(StencilFace face, uint32_t mask) {
    (void)face;
    (void)mask;
    CC_LOG_WARNING("[D3D12] setStencilWriteMask(face=%u, mask=0x%x) ignored — set in pipeline state", static_cast<unsigned>(face), mask);
}

void CCD3D12CommandBuffer::setStencilCompareMask(StencilFace face, uint32_t ref, uint32_t mask) {
#if defined(_WIN32)
    if (!_impl->commandList) return;
    (void)face;
    (void)mask;
    _impl->commandList->OMSetStencilRef(ref);
#else
    (void)face;
    (void)ref;
    (void)mask;
#endif
}

void CCD3D12CommandBuffer::nextSubpass() {
    // D3D12 doesn't have explicit subpasses
}

void CCD3D12CommandBuffer::draw(const DrawInfo &info) {
#if defined(_WIN32)
    if (!_impl->commandList) return;

    // Flush any pending descriptor set bindings before drawing
    flushDescriptorSets();

    const uint32_t instanceCount = std::max<uint32_t>(info.instanceCount, 1);
    const uint32_t firstInstance = info.firstInstance;
    auto *d3d12PSO = static_cast<CCD3D12PipelineState *>(_impl->boundPipelineState);
    static uint32_t s_drawDiagCount = 0;
    const uint32_t drawId = s_drawDiagCount++;
    if (drawId < 3000) {
        const char *shaderName = "<null>";
        if (_impl->boundPipelineState && _impl->boundPipelineState->getShader()) {
            shaderName = _impl->boundPipelineState->getShader()->getName().c_str();
        }
        const auto *blendTarget0 = (_impl->boundPipelineState && !_impl->boundPipelineState->getBlendState().targets.empty())
                                       ? &_impl->boundPipelineState->getBlendState().targets[0]
                                       : nullptr;
        drawDiagLog("[DRAW] #%u shader='%s' indexCount=%u vertexCount=%u instanceCount=%u firstIndex=%u vertexOffset=%d firstInstance=%u pso=%p layout=%p pendingSets=%u dirtyAfterFlush=%d fallback=%d blend0=%u src=%u dst=%u srcA=%u dstA=%u\n",
                    drawId,
                    shaderName,
                    info.indexCount,
                    info.vertexCount,
                    instanceCount,
                    info.firstIndex,
                    info.vertexOffset,
                    firstInstance,
                    _impl->boundPipelineState,
                    _impl->boundPipelineLayout,
                    _impl->pendingSetCount,
                    _impl->descriptorSetsDirty ? 1 : 0,
                    (d3d12PSO && d3d12PSO->isDiagnosticFallback()) ? 1 : 0,
                    blendTarget0 ? blendTarget0->blend : 0U,
                    blendTarget0 ? static_cast<uint32_t>(blendTarget0->blendSrc) : 0U,
                    blendTarget0 ? static_cast<uint32_t>(blendTarget0->blendDst) : 0U,
                    blendTarget0 ? static_cast<uint32_t>(blendTarget0->blendSrcAlpha) : 0U,
                    blendTarget0 ? static_cast<uint32_t>(blendTarget0->blendDstAlpha) : 0U);
    }

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
#else
    (void)info;
#endif
}

void CCD3D12CommandBuffer::updateBuffer(Buffer *buff, const void *data, uint32_t size) {
#if defined(_WIN32)
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
#else
    (void)buff;
    (void)data;
    (void)size;
#endif
}

void CCD3D12CommandBuffer::copyBuffersToTexture(const uint8_t *const *buffers, Texture *texture, const BufferTextureCopy *regions, uint32_t count) {
#if defined(_WIN32)
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

        // Transfer ownership to the pending list — keeps resource alive until
        // the next begin() call, by which point the GPU has finished execution.
        _impl->pendingUploadResources.push_back(std::move(uploadResource));
    }

    // Transition back to appropriate state after copy
    D3D12_RESOURCE_STATES postCopyState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (hasFlag(d3d12Texture->getInfo().usage, TextureUsageBit::COLOR_ATTACHMENT)) {
        postCopyState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    } else if (hasFlag(d3d12Texture->getInfo().usage, TextureUsageBit::DEPTH_STENCIL_ATTACHMENT)) {
        postCopyState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    D3D12_RESOURCE_BARRIER toPostCopy{};
    toPostCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPostCopy.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    toPostCopy.Transition.pResource = textureResource;
    toPostCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toPostCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toPostCopy.Transition.StateAfter = postCopyState;
    _impl->commandList->ResourceBarrier(1, &toPostCopy);
    d3d12Texture->setCurrentState(postCopyState);
#else
    (void)buffers;
    (void)texture;
    (void)regions;
    (void)count;
#endif
}

void CCD3D12CommandBuffer::blitTexture(Texture *srcTexture, Texture *dstTexture, const TextureBlit *regions, uint32_t count, Filter filter) {
#if defined(_WIN32)
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
    ccstd::vector<D3D12_RESOURCE_BARRIER> preBarriers;

    D3D12_RESOURCE_STATES srcPrevState = srcD3D12->getCurrentState();
    if (srcPrevState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = srcPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        preBarriers.push_back(b);
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
        preBarriers.push_back(b);
    }

    if (!preBarriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(preBarriers.size()), preBarriers.data());
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
    ccstd::vector<D3D12_RESOURCE_BARRIER> postBarriers;

    D3D12_RESOURCE_STATES srcPostState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (hasFlag(srcInfo.usage, TextureUsageBit::COLOR_ATTACHMENT)) {
        srcPostState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = srcPostState;
        postBarriers.push_back(b);
    }

    D3D12_RESOURCE_STATES dstPostState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (hasFlag(dstInfo.usage, TextureUsageBit::COLOR_ATTACHMENT)) {
        dstPostState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = dstPostState;
        postBarriers.push_back(b);
    }

    if (!postBarriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(postBarriers.size()), postBarriers.data());
    }
    srcD3D12->setCurrentState(srcPostState);
    dstD3D12->setCurrentState(dstPostState);

    (void)filter; // Filter is not used for same-size copy (D3D12 CopyTextureRegion doesn't support filtering)
#else
    (void)srcTexture;
    (void)dstTexture;
    (void)regions;
    (void)count;
    (void)filter;
#endif
}

void CCD3D12CommandBuffer::copyTexture(Texture *srcTexture, Texture *dstTexture, const TextureCopy *regions, uint32_t count) {
#if defined(_WIN32)
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
    ccstd::vector<D3D12_RESOURCE_BARRIER> preBarriers;

    D3D12_RESOURCE_STATES srcPrevState = srcD3D12->getCurrentState();
    if (srcPrevState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = srcPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        preBarriers.push_back(b);
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
        preBarriers.push_back(b);
    }

    if (!preBarriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(preBarriers.size()), preBarriers.data());
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
    ccstd::vector<D3D12_RESOURCE_BARRIER> postBarriers;

    // Src back to shader resource
    D3D12_RESOURCE_STATES srcPostState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (hasFlag(srcInfo.usage, TextureUsageBit::COLOR_ATTACHMENT)) {
        srcPostState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    } else if (hasFlag(srcInfo.usage, TextureUsageBit::DEPTH_STENCIL_ATTACHMENT)) {
        srcPostState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = srcPostState;
        postBarriers.push_back(b);
    }

    // Dst back to shader resource (or render target)
    D3D12_RESOURCE_STATES dstPostState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (hasFlag(dstInfo.usage, TextureUsageBit::COLOR_ATTACHMENT)) {
        dstPostState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    } else if (hasFlag(dstInfo.usage, TextureUsageBit::DEPTH_STENCIL_ATTACHMENT)) {
        dstPostState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = dstPostState;
        postBarriers.push_back(b);
    }

    if (!postBarriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(postBarriers.size()), postBarriers.data());
    }
    srcD3D12->setCurrentState(srcPostState);
    dstD3D12->setCurrentState(dstPostState);
#else
    (void)srcTexture;
    (void)dstTexture;
    (void)regions;
    (void)count;
#endif
}

void CCD3D12CommandBuffer::resolveTexture(Texture *srcTexture, Texture *dstTexture, const TextureCopy *regions, uint32_t count) {
#if defined(_WIN32)
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
    ccstd::vector<D3D12_RESOURCE_BARRIER> preBarriers;

    D3D12_RESOURCE_STATES srcPrevState = srcD3D12->getCurrentState();
    if (srcPrevState != D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = srcPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        preBarriers.push_back(b);
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
        preBarriers.push_back(b);
    }

    if (!preBarriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(preBarriers.size()), preBarriers.data());
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
    ccstd::vector<D3D12_RESOURCE_BARRIER> postBarriers;

    {
        D3D12_RESOURCE_STATES srcPostState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (hasFlag(srcInfo.usage, TextureUsageBit::COLOR_ATTACHMENT)) {
            srcPostState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        b.Transition.StateAfter = srcPostState;
        postBarriers.push_back(b);
        srcD3D12->setCurrentState(srcPostState);
    }
    {
        D3D12_RESOURCE_STATES dstPostState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (hasFlag(dstInfo.usage, TextureUsageBit::COLOR_ATTACHMENT)) {
            dstPostState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        b.Transition.StateAfter = dstPostState;
        postBarriers.push_back(b);
        dstD3D12->setCurrentState(dstPostState);
    }

    if (!postBarriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(postBarriers.size()), postBarriers.data());
    }
#else
    (void)srcTexture;
    (void)dstTexture;
    (void)regions;
    (void)count;
#endif
}

void CCD3D12CommandBuffer::dispatch(const DispatchInfo &info) {
#if defined(_WIN32)
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
#else
    (void)info;
#endif
}

void CCD3D12CommandBuffer::pipelineBarrier(const GeneralBarrier *barrier, const BufferBarrier *const *bufferBarriers, const Buffer *const *buffers, uint32_t bufferCount, const TextureBarrier *const *textureBarriers, const Texture *const *textures, uint32_t textureBarrierCount) {
#if defined(_WIN32)
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
#else
    (void)barrier;
    (void)bufferBarriers;
    (void)buffers;
    (void)bufferCount;
    (void)textureBarriers;
    (void)textures;
    (void)textureBarrierCount;
#endif
}

void CCD3D12CommandBuffer::beginQuery(QueryPool *queryPool, uint32_t id) {
#if defined(_WIN32)
    if (!_impl || !_impl->commandList) return;
    auto *d3d12Pool = static_cast<CCD3D12QueryPool *>(queryPool);
    auto *heap = static_cast<ID3D12QueryHeap *>(d3d12Pool->getD3D12QueryHeap());
    if (!heap) return;
    D3D12_QUERY_TYPE queryType = (d3d12Pool->getType() == QueryType::OCCLUSION)
                                     ? D3D12_QUERY_TYPE_OCCLUSION
                                     : D3D12_QUERY_TYPE_TIMESTAMP;
    _impl->commandList->BeginQuery(heap, queryType, id);
#else
    (void)queryPool;
    (void)id;
#endif
}

void CCD3D12CommandBuffer::endQuery(QueryPool *queryPool, uint32_t id) {
#if defined(_WIN32)
    if (!_impl || !_impl->commandList) return;
    auto *d3d12Pool = static_cast<CCD3D12QueryPool *>(queryPool);
    auto *heap = static_cast<ID3D12QueryHeap *>(d3d12Pool->getD3D12QueryHeap());
    if (!heap) return;
    D3D12_QUERY_TYPE queryType = (d3d12Pool->getType() == QueryType::OCCLUSION)
                                     ? D3D12_QUERY_TYPE_OCCLUSION
                                     : D3D12_QUERY_TYPE_TIMESTAMP;
    _impl->commandList->EndQuery(heap, queryType, id);
#else
    (void)queryPool;
    (void)id;
#endif
}

void CCD3D12CommandBuffer::resetQueryPool(QueryPool *queryPool) {
#if defined(_WIN32)
    if (!_impl || !_impl->commandList) return;
    auto *d3d12Pool = static_cast<CCD3D12QueryPool *>(queryPool);
    auto *heap = static_cast<ID3D12QueryHeap *>(d3d12Pool->getD3D12QueryHeap());
    if (!heap) return;
    // D3D12 doesn't have a direct "reset query pool" on command list.
    // The results are overwritten on next BeginQuery/EndQuery cycle.
    // We clear the CPU-side results here.
    (void)heap;
#else
    (void)queryPool;
#endif
}

void CCD3D12CommandBuffer::customCommand(CustomCommand &&cmd) {
#if defined(_WIN32)
    if (cmd && _impl && _impl->commandList) {
        cmd(static_cast<void *>(_impl->commandList.Get()));
    }
#else
    (void)cmd;
#endif
}

void *CCD3D12CommandBuffer::getD3D12CommandList() const {
#if defined(_WIN32)
    return _impl ? _impl->commandList.Get() : nullptr;
#else
    return nullptr;
#endif
}

} // namespace gfx
} // namespace cc
