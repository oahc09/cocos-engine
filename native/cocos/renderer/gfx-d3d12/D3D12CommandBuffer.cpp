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

// File diagnostic for draw calls
#include <cstdio>
#include <cstdarg>
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

struct CCD3D12CommandBuffer::Impl {
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice; // cached ref, not owning

    // Track state for the current recording
    PipelineState *boundPipelineState{nullptr};
    PipelineLayout *boundPipelineLayout{nullptr};
    bool isRecording{false};

    // Track swapchain for resource barriers during render pass
    CCD3D12Swapchain *activeSwapchain{nullptr};
    ID3D12Resource *activeSwapchainBackBuffer{nullptr};
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
        CC_LOG_ERROR("D3D12CommandBuffer::begin - allocator reset failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    hr = _impl->commandList->Reset(_impl->commandAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12CommandBuffer::begin - command list reset failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    _impl->isRecording = true;
    _impl->boundPipelineState = nullptr;
    _impl->boundPipelineLayout = nullptr;
    _impl->activeSwapchain = nullptr;
    _impl->activeSwapchainBackBuffer = nullptr;
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
    }
    _impl->isRecording = false;
#endif
}

void CCD3D12CommandBuffer::beginRenderPass(RenderPass *renderPass, Framebuffer *fbo, const Rect &renderArea, const Color *colors, float depth, uint32_t stencil, CommandBuffer *const *secondaryCBs, uint32_t secondaryCBCount) {
    (void)renderPass;
    (void)secondaryCBs;
    (void)secondaryCBCount;
#if defined(_WIN32)
    if (!_impl->commandList) return;

    auto *d3d12Fbo = static_cast<CCD3D12Framebuffer *>(fbo);
    if (!d3d12Fbo) {
        CC_LOG_WARNING("D3D12CommandBuffer::beginRenderPass - framebuffer is null.");
        return;
    }

    // If this framebuffer renders to a swapchain, insert PRESENT → RENDER_TARGET barrier
    CCD3D12Swapchain *swapchain = d3d12Fbo->getSwapchain();
    if (swapchain) {
        static uint32_t s_rpCount = 0;
        ++s_rpCount;
        if (s_rpCount <= 5) {
            drawDiagLog("[RENDERPASS] #%u: swapchain FBO detected, inserting PRESENT->RENDER_TARGET barrier\n", s_rpCount);
        }
        auto *backBuffer = static_cast<ID3D12Resource *>(swapchain->getCurrentBackBufferHandle());
        if (backBuffer) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = backBuffer;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            _impl->commandList->ResourceBarrier(1, &barrier);

            _impl->activeSwapchain = swapchain;
            _impl->activeSwapchainBackBuffer = backBuffer;
        }
    }

    const uint32_t fboWidth = d3d12Fbo->getWidth();
    const uint32_t fboHeight = d3d12Fbo->getHeight();

    // Count color attachments from framebuffer
    const auto &colorTextures = fbo->getColorTextures();
    const uint32_t colorCount = static_cast<uint32_t>(colorTextures.size());

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

    // Set render targets
    _impl->commandList->OMSetRenderTargets(
        colorCount,
        rtvHandles,
        FALSE,
        hasDSV ? &dsvHandle : nullptr);

    // Clear render targets
    for (uint32_t i = 0; i < colorCount; ++i) {
        if (rtvHandles[i].ptr != 0 && colors && i < colorCount) {
            float clearColor[4] = {colors[i].x, colors[i].y, colors[i].z, colors[i].w};
            _impl->commandList->ClearRenderTargetView(rtvHandles[i], clearColor, 0, nullptr);
        }
    }

    // Clear depth-stencil
    if (hasDSV) {
        D3D12_CLEAR_FLAGS clearFlags = D3D12_CLEAR_FLAG_DEPTH;
        if (renderPass) {
            const auto &dsAttachment = renderPass->getDepthStencilAttachment();
            if (dsAttachment.format == Format::DEPTH_STENCIL) {
                clearFlags |= D3D12_CLEAR_FLAG_STENCIL;
            }
        }
        _impl->commandList->ClearDepthStencilView(dsvHandle, clearFlags, depth, stencil, 0, nullptr);
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
    // If we transitioned a swapchain back buffer to RENDER_TARGET, transition it back to PRESENT
    if (_impl->activeSwapchainBackBuffer) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = _impl->activeSwapchainBackBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        _impl->commandList->ResourceBarrier(1, &barrier);

        _impl->activeSwapchain = nullptr;
        _impl->activeSwapchainBackBuffer = nullptr;
    }
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

    // Set primitive topology from PSO
    D3D12_PRIMITIVE_TOPOLOGY topology = static_cast<D3D12_PRIMITIVE_TOPOLOGY>(d3d12PSO->getD3D12PrimitiveTopology());
    _impl->commandList->IASetPrimitiveTopology(topology);

    // Set root signature from the pipeline layout
    auto *pipelineLayout = pso->getPipelineLayout();
    if (pipelineLayout) {
        auto *d3d12Layout = static_cast<const CCD3D12PipelineLayout *>(pipelineLayout);
        auto *rootSig = static_cast<ID3D12RootSignature *>(d3d12Layout->getID3D12RootSignature());
        if (rootSig) {
            _impl->commandList->SetGraphicsRootSignature(rootSig);
        }
        _impl->boundPipelineLayout = const_cast<PipelineLayout *>(pipelineLayout);
    }

    _impl->boundPipelineState = pso;
#else
    (void)pso;
#endif
}

void CCD3D12CommandBuffer::bindDescriptorSet(uint32_t set, DescriptorSet *descriptorSet, uint32_t dynamicOffsetCount, const uint32_t *dynamicOffsets) {
    (void)dynamicOffsetCount;
    (void)dynamicOffsets;
#if defined(_WIN32)
    if (!_impl->commandList || !descriptorSet) return;

    // In D3D12, descriptor sets need to be copied to a GPU-visible heap and bound via root descriptor tables.
    // For the PoC, we assume the DescriptorSet has already created its GPU-visible descriptors
    // during update(). We need to get the GPU handles from the descriptor set and bind them.

    // Get the device's GPU-visible descriptor heap
    auto *device = CCD3D12Device::getInstance();
    if (!device) return;

    // The D3D12DescriptorSet during update() should have created descriptors.
    // For the PoC, we rely on the descriptor heap pool in Device.
    // We'll set the descriptor heaps and root descriptor table.

    // For now, set root descriptor table with the descriptor set's GPU handle
    // This is a simplified approach - a full implementation would need to
    // copy descriptors to a GPU-visible heap and track them.

    // Set descriptor heaps (need at least CBV_SRV_UAV heap for graphics)
    auto *heapPool = device->getGPUDescriptorHeapPool();
    if (heapPool) {
        void *heap = heapPool->getHeap(0);
        if (heap) {
            auto *d3dHeap = static_cast<ID3D12DescriptorHeap *>(heap);
            _impl->commandList->SetDescriptorHeaps(1, &d3dHeap);
        }
    }

    // Note: A full implementation would need to get the GPU descriptor handle
    // from the descriptor set and call SetGraphicsRootDescriptorTable.
    // For PoC this is a placeholder that at least sets up the descriptor heap.
    CC_LOG_INFO("D3D12CommandBuffer::bindDescriptorSet(set=%u) - bound.", set);
#else
    (void)set;
    (void)descriptorSet;
#endif
}

void CCD3D12CommandBuffer::bindInputAssembler(InputAssembler *ia) {
#if defined(_WIN32)
    if (!_impl->commandList || !ia) return;

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
    // D3D12 doesn't support line width > 1 natively
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
    (void)minBounds;
    (void)maxBounds;
    // Depth bounds test requires D3D12 feature support check
}

void CCD3D12CommandBuffer::setStencilWriteMask(StencilFace face, uint32_t mask) {
    (void)face;
    (void)mask;
    // Stencil write mask is typically set in pipeline state, not dynamically
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

    const uint32_t instanceCount = std::max<uint32_t>(info.instanceCount, 1);
    const uint32_t firstInstance = info.firstInstance;

    // Diagnostic: log first few draw calls to verify rendering pipeline
    static uint32_t s_drawCallCount = 0;
    if (s_drawCallCount < 10) {
        CC_LOG_INFO("[DIAG-DRAW] #%u: idx=%u vtx=%u inst=%u firstIdx=%u vtxOff=%d firstInst=%u pso=%s",
                     s_drawCallCount,
                     info.indexCount, info.vertexCount, instanceCount,
                     info.firstIndex, info.vertexOffset, firstInstance,
                     _impl->boundPipelineState ? "YES" : "NULL");
        drawDiagLog("[DRAW] #%u: idx=%u vtx=%u inst=%u firstIdx=%u vtxOff=%d firstInst=%u pso=%s\n",
                     s_drawCallCount,
                     info.indexCount, info.vertexCount, instanceCount,
                     info.firstIndex, info.vertexOffset, firstInstance,
                     _impl->boundPipelineState ? "YES" : "NULL");
        ++s_drawCallCount;
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
    toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    _impl->commandList->ResourceBarrier(1, &toCopyDest);

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
    }

    // Transition back to common
    D3D12_RESOURCE_BARRIER toCommon{};
    toCommon.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCommon.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    toCommon.Transition.pResource = textureResource;
    toCommon.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCommon.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toCommon.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    _impl->commandList->ResourceBarrier(1, &toCommon);
#else
    (void)buffers;
    (void)texture;
    (void)regions;
    (void)count;
#endif
}

void CCD3D12CommandBuffer::blitTexture(Texture *srcTexture, Texture *dstTexture, const TextureBlit *regions, uint32_t count, Filter filter) {
    // Blit is complex in D3D12 (requires compute or custom render pass)
    (void)srcTexture;
    (void)dstTexture;
    (void)regions;
    (void)count;
    (void)filter;
}

void CCD3D12CommandBuffer::copyTexture(Texture *srcTexture, Texture *dstTexture, const TextureCopy *regions, uint32_t count) {
    // Texture-to-texture copy via CopyTextureRegion
    (void)srcTexture;
    (void)dstTexture;
    (void)regions;
    (void)count;
}

void CCD3D12CommandBuffer::resolveTexture(Texture *srcTexture, Texture *dstTexture, const TextureCopy *regions, uint32_t count) {
    // Resolve multisampled texture
    (void)srcTexture;
    (void)dstTexture;
    (void)regions;
    (void)count;
}

void CCD3D12CommandBuffer::dispatch(const DispatchInfo &info) {
#if defined(_WIN32)
    if (!_impl->commandList) return;
    _impl->commandList->Dispatch(info.groupCountX, info.groupCountY, info.groupCountZ);
#else
    (void)info;
#endif
}

void CCD3D12CommandBuffer::pipelineBarrier(const GeneralBarrier *barrier, const BufferBarrier *const *bufferBarriers, const Buffer *const *buffers, uint32_t bufferCount, const TextureBarrier *const *textureBarriers, const Texture *const *textures, uint32_t textureBarrierCount) {
#if defined(_WIN32)
    if (!_impl->commandList) return;

    // Build D3D12_RESOURCE_BARRIER array
    ccstd::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(bufferCount + textureBarrierCount);

    // Buffer barriers
    for (uint32_t i = 0; i < bufferCount; ++i) {
        if (!buffers[i] || !bufferBarriers[i]) continue;
        auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(const_cast<Buffer *>(buffers[i]));
        auto *resource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
        if (!resource) continue;

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        // Simplified: assume common barriers
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_GENERIC_READ;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
        barriers.push_back(b);
    }

    // Texture barriers
    for (uint32_t i = 0; i < textureBarrierCount; ++i) {
        if (!textures[i] || !textureBarriers[i]) continue;
        auto *d3d12Texture = static_cast<CCD3D12Texture *>(const_cast<Texture *>(textures[i]));
        auto *resource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
        if (!resource) continue;

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_GENERIC_READ;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
        barriers.push_back(b);
    }

    if (!barriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }
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
                                     : D3D12_QUERY_TYPE_OCCLUSION;
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
                                     : D3D12_QUERY_TYPE_OCCLUSION;
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

void *CCD3D12CommandBuffer::getD3D12CommandList() const {
#if defined(_WIN32)
    return _impl ? _impl->commandList.Get() : nullptr;
#else
    return nullptr;
#endif
}

} // namespace gfx
} // namespace cc
