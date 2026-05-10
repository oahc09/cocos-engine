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

#include "D3D12Device.h"
#include "D3D12Buffer.h"
#include "D3D12CommandBuffer.h"
#include "D3D12DescriptorHeapPool.h"
#include "D3D12DescriptorSet.h"
#include "D3D12DescriptorSetLayout.h"
#include "D3D12Framebuffer.h"
#include "D3D12InputAssembler.h"
#include "D3D12PipelineLayout.h"
#include "D3D12PipelineState.h"
#include "D3D12QueryPool.h"
#include "D3D12Queue.h"
#include "D3D12RenderPass.h"
#include "D3D12Shader.h"
#include "D3D12Swapchain.h"
#include "D3D12Texture.h"
#include "base/Log.h"


#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <algorithm>
#include <cstring>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace cc {
namespace gfx {

CCD3D12Device *CCD3D12Device::instance = nullptr;

namespace {
constexpr float D3D12_POC_CLEAR_COLOR[4] = {0.1F, 0.2F, 0.8F, 1.0F};

D3D12_RESOURCE_BARRIER textureTransition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}
}

struct CCD3D12Device::Impl {
    Microsoft::WRL::ComPtr<IDXGIFactory6> dxgiFactory;
    Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> graphicsQueue;
    // Device-level command allocator/list for legacy present and copyBuffersToTexture
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> frameFence;

    HANDLE fenceEvent{nullptr};
    uint64_t fenceValue{0};

    // GPU-visible descriptor heap pools for shader access
    std::unique_ptr<D3D12DescriptorHeapPool> gpuDescriptorHeapPool;    // CBV_SRV_UAV, shaderVisible
    std::unique_ptr<D3D12DescriptorHeapPool> samplerDescriptorHeapPool; // SAMPLER, shaderVisible

    // Dummy resources for safe null descriptor bindings
    IntrusivePtr<CCD3D12Texture> dummyTexture;
    IntrusivePtr<CCD3D12Buffer>  dummyBuffer;

    // Command signatures for ExecuteIndirect (indirect draw / dispatch)
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> drawIndirectSig;       // DrawInstanced
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> drawIndexedIndirectSig; // DrawIndexedInstanced
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatchIndirectSig;   // Dispatch
};

CCD3D12Device *CCD3D12Device::getInstance() {
    return CCD3D12Device::instance;
}

CCD3D12Device::CCD3D12Device()
: _impl(std::make_unique<Impl>()) {
    CCD3D12Device::instance = this;
    _api = API::D3D12;
    _deviceName = "D3D12";
    // D3D12 is single-threaded COM-based; the DeviceAgent's detached
    // thread model conflicts with D3D12's threading requirements and
    // causes DEVICE_HUNG (0x887a0006) on swapchain/texture creation.
    Device::isSupportDetachDeviceThread = false;
}

CCD3D12Device::~CCD3D12Device() {
    CCD3D12Device::instance = nullptr;
}

bool CCD3D12Device::doInit(const DeviceInfo &info) {
    (void)info;

    if (!initializeD3D12Context()) {
        CC_LOG_ERROR("Failed to initialize D3D12 context.");
        return false;
    }

    // Initialize GPU-visible descriptor heap pools
    _impl->gpuDescriptorHeapPool = std::make_unique<D3D12DescriptorHeapPool>();
    _impl->gpuDescriptorHeapPool->initialize(
        D3D12DescriptorHeapPool::HeapType::CBV_SRV_UAV, 4096, true);

    _impl->samplerDescriptorHeapPool = std::make_unique<D3D12DescriptorHeapPool>();
    _impl->samplerDescriptorHeapPool->initialize(
        D3D12DescriptorHeapPool::HeapType::SAMPLER, 2048, true);

    CC_LOG_INFO("D3D12 descriptor heap pools initialized.");

    QueueInfo queueInfo;
    queueInfo.type = QueueType::GRAPHICS;
    _queue = createQueue(queueInfo);

    QueryPoolInfo queryPoolInfo{QueryType::OCCLUSION, DEFAULT_MAX_QUERY_OBJECTS, true};
    _queryPool = createQueryPool(queryPoolInfo);

    CommandBufferInfo cmdBuffInfo;
    cmdBuffInfo.type = CommandBufferType::PRIMARY;
    cmdBuffInfo.queue = _queue;
    _cmdBuff = createCommandBuffer(cmdBuffInfo);

    // Initialize format feature support table via D3D12 API
    initFormatFeatures();

    _features[toNumber(Feature::ELEMENT_INDEX_UINT)] = true;
    _features[toNumber(Feature::INSTANCED_ARRAYS)] = true;
    _features[toNumber(Feature::MULTIPLE_RENDER_TARGETS)] = true;
    _features[toNumber(Feature::BLEND_MINMAX)] = true;
    _features[toNumber(Feature::COMPUTE_SHADER)] = true;
    _features[toNumber(Feature::INPUT_ATTACHMENT_BENEFIT)] = false;
    _features[toNumber(Feature::SUBPASS_COLOR_INPUT)] = false;
    _features[toNumber(Feature::SUBPASS_DEPTH_STENCIL_INPUT)] = false;
    _features[toNumber(Feature::RASTERIZATION_ORDER_NOCOHERENT)] = false;
    _features[toNumber(Feature::MULTI_SAMPLE_RESOLVE_DEPTH_STENCIL)] = false;

    // Initialize device capabilities
    initCapabilities();

    _renderer = "D3D12";
    _vendor = "Unknown";

    // Create dummy resources for safe null descriptor bindings
    {
        gfx::TextureInfo texInfo{};
        texInfo.usage = TextureUsageBit::SAMPLED | TextureUsageBit::STORAGE;
        texInfo.format = Format::RGBA8;
        texInfo.width = 1;
        texInfo.height = 1;
        texInfo.levelCount = 1;
        texInfo.layerCount = 1;
        texInfo.samples = SampleCount::X1;
        texInfo.flags = TextureFlagBit::NONE;
        _impl->dummyTexture = static_cast<CCD3D12Texture *>(createTexture(texInfo));

        gfx::BufferInfo bufInfo{};
        bufInfo.usage = BufferUsageBit::UNIFORM | BufferUsageBit::STORAGE;
        bufInfo.memUsage = MemoryUsageBit::HOST | MemoryUsageBit::DEVICE;
        bufInfo.size = 256;
        bufInfo.flags = BufferFlagBit::NONE;
        _impl->dummyBuffer = static_cast<CCD3D12Buffer *>(createBuffer(bufInfo));

        if (_impl->dummyTexture && _impl->dummyBuffer) {
            CC_LOG_INFO("[D3D12] Dummy resources created for null descriptor bindings");
        } else {
            CC_LOG_WARNING("[D3D12] Failed to create dummy resources; null descriptors will be skipped");
        }
    }

    CC_LOG_INFO("D3D12 device initialized.");
    CC_LOG_INFO("RENDERER: %s", _renderer.c_str());
    CC_LOG_INFO("VENDOR: %s", _vendor.c_str());
    CC_LOG_INFO("CAPS: maxVertexUniformVectors=%u, maxFragmentUniformVectors=%u, maxTextureSize=%u",
                _caps.maxVertexUniformVectors, _caps.maxFragmentUniformVectors, _caps.maxTextureSize);

    // Create command signatures for indirect draw / dispatch
    {
        // DrawIndirect: matches D3D12_DRAW_ARGUMENTS { VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation }
        D3D12_COMMAND_SIGNATURE_DESC sigDesc{};
        D3D12_INDIRECT_ARGUMENT_DESC argDesc{};
        argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        sigDesc.pArgumentDescs = &argDesc;
        sigDesc.NumArgumentDescs = 1;
        sigDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        _impl->d3dDevice->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&_impl->drawIndirectSig));

        // DrawIndexedIndirect: matches D3D12_DRAW_INDEXED_ARGUMENTS { IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation }
        argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        sigDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        _impl->d3dDevice->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&_impl->drawIndexedIndirectSig));

        // DispatchIndirect: matches D3D12_DISPATCH_ARGUMENTS { ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ }
        argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        sigDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        _impl->d3dDevice->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&_impl->dispatchIndirectSig));
    }

    return true;
}

void CCD3D12Device::doDestroy() {
    waitForGpu();

    // Release dummy resources
    _impl->dummyTexture = nullptr;
    _impl->dummyBuffer = nullptr;

    // Shutdown descriptor heap pools first
    if (_impl->samplerDescriptorHeapPool) {
        _impl->samplerDescriptorHeapPool->shutdown();
        _impl->samplerDescriptorHeapPool.reset();
    }
    if (_impl->gpuDescriptorHeapPool) {
        _impl->gpuDescriptorHeapPool->shutdown();
        _impl->gpuDescriptorHeapPool.reset();
    }

    if (_impl->fenceEvent) {
        CloseHandle(_impl->fenceEvent);
        _impl->fenceEvent = nullptr;
    }
    _impl->commandList.Reset();
    _impl->commandAllocator.Reset();
    _impl->frameFence.Reset();
    _impl->graphicsQueue.Reset();
    _impl->d3dDevice.Reset();
    _impl->dxgiFactory.Reset();

    CC_SAFE_DESTROY_AND_DELETE(_cmdBuff);
    CC_SAFE_DESTROY_AND_DELETE(_queryPool);
    CC_SAFE_DESTROY_AND_DELETE(_queue);
}

void CCD3D12Device::acquire(Swapchain *const *swapchains, uint32_t count) {
    // The DeviceAgent and DeviceValidator layers unwrap their wrappers before
    // passing swapchains down to us, so the pointers here are raw CCD3D12Swapchain*.
    _d3d12Swapchains.clear();
    for (uint32_t i = 0; i < count; ++i) {
        if (swapchains[i]) {
            _d3d12Swapchains.push_back(static_cast<CCD3D12Swapchain *>(swapchains[i]));
        }
    }

    if (_onAcquire) {
        _onAcquire->execute();
    }
}

CCD3D12Texture *CCD3D12Device::getDummyTexture() const {
    return _impl ? _impl->dummyTexture.get() : nullptr;
}

CCD3D12Buffer *CCD3D12Device::getDummyBuffer() const {
    return _impl ? _impl->dummyBuffer.get() : nullptr;
}

void *CCD3D12Device::getDrawIndirectSignature() const {
    return _impl ? _impl->drawIndirectSig.Get() : nullptr;
}

void *CCD3D12Device::getDrawIndexedIndirectSignature() const {
    return _impl ? _impl->drawIndexedIndirectSig.Get() : nullptr;
}

void *CCD3D12Device::getDispatchIndirectSignature() const {
    return _impl ? _impl->dispatchIndirectSig.Get() : nullptr;
}

void CCD3D12Device::present() {
    if (!_impl->graphicsQueue || !_impl->frameFence || !_impl->fenceEvent) {
        return;
    }

    // Use _d3d12Swapchains instead of getSwapchains() — the DeviceAgent/DeviceValidator
    // layers wrap swapchains and store them in THEIR _swapchains (private to Device base).
    // Our _d3d12Swapchains tracks the raw CCD3D12Swapchain objects created by this device.
    for (auto *d3d12Swapchain : _d3d12Swapchains) {
        if (!d3d12Swapchain || !d3d12Swapchain->isReady()) {
            continue;
        }

        // The engine's rendering pipeline already handles resource barriers
        // (PRESENT ↔ RENDER_TARGET) and render target clears via CommandBuffer.
        // We only need to present the swapchain here.
        if (!d3d12Swapchain->present()) {
            continue;
        }

        ++_impl->fenceValue;
        HRESULT hr = _impl->graphicsQueue->Signal(_impl->frameFence.Get(), _impl->fenceValue);
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12 queue signal failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            continue;
        }

        if (_impl->frameFence->GetCompletedValue() < _impl->fenceValue) {
            hr = _impl->frameFence->SetEventOnCompletion(_impl->fenceValue, _impl->fenceEvent);
            if (SUCCEEDED(hr)) {
                DWORD waitResult = WaitForSingleObject(_impl->fenceEvent, 5000);
                if (waitResult == WAIT_TIMEOUT) {
                    CC_LOG_ERROR("D3D12 present fence wait timed out (5s). GPU may be hung.");
                } else if (waitResult == WAIT_FAILED) {
                    CC_LOG_ERROR("D3D12 present WaitForSingleObject failed. errno=%u", static_cast<unsigned>(GetLastError()));
                }
            } else {
                CC_LOG_ERROR("D3D12 fence wait setup failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            }
        }
    }

    // Note: GPU descriptor heap pool reset is now handled exclusively in
    // CommandBuffer::begin() to avoid double-reset if multiple command buffers
    // exist. Previously this was redundantly called here AND in begin().
}

CommandBuffer *CCD3D12Device::createCommandBuffer(const CommandBufferInfo &info, bool hasAgent) {
    (void)info;
    (void)hasAgent;
    return ccnew CCD3D12CommandBuffer;
}

Queue *CCD3D12Device::createQueue() {
    return ccnew CCD3D12Queue;
}

QueryPool *CCD3D12Device::createQueryPool() {
    return ccnew CCD3D12QueryPool;
}

Swapchain *CCD3D12Device::createSwapchain() {
    return ccnew CCD3D12Swapchain;
}

Buffer *CCD3D12Device::createBuffer() {
    return ccnew CCD3D12Buffer;
}

Texture *CCD3D12Device::createTexture() {
    return ccnew CCD3D12Texture;
}

Shader *CCD3D12Device::createShader() {
    return ccnew CCD3D12Shader;
}

InputAssembler *CCD3D12Device::createInputAssembler() {
    return ccnew CCD3D12InputAssembler;
}

RenderPass *CCD3D12Device::createRenderPass() {
    return ccnew CCD3D12RenderPass;
}

Framebuffer *CCD3D12Device::createFramebuffer() {
    return ccnew CCD3D12Framebuffer;
}

DescriptorSet *CCD3D12Device::createDescriptorSet() {
    return ccnew CCD3D12DescriptorSet;
}

DescriptorSetLayout *CCD3D12Device::createDescriptorSetLayout() {
    return ccnew CCD3D12DescriptorSetLayout;
}

PipelineLayout *CCD3D12Device::createPipelineLayout() {
    return ccnew CCD3D12PipelineLayout;
}

PipelineState *CCD3D12Device::createPipelineState() {
    return ccnew CCD3D12PipelineState;
}

void CCD3D12Device::copyBuffersToTexture(const uint8_t *const *buffers, Texture *dst, const BufferTextureCopy *regions, uint32_t count) {
    if (!buffers || !dst || !regions || count == 0 || !_impl->d3dDevice || !_impl->graphicsQueue || !_impl->commandAllocator || !_impl->commandList) {
        return;
    }

    auto *d3d12Texture = static_cast<CCD3D12Texture *>(dst);
    auto *textureResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
    if (!textureResource) {
        return;
    }

    const auto &textureInfo = dst->getInfo();
    const uint32_t bytesPerTexel = GFX_FORMAT_INFOS[toNumber(textureInfo.format)].size;
    if (bytesPerTexel == 0) {
        CC_LOG_WARNING("D3D12 texture upload skipped for unsupported texel size.");
        return;
    }

    waitForGpu();

    HRESULT hr = _impl->commandAllocator->Reset();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 upload command allocator reset failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    hr = _impl->commandList->Reset(_impl->commandAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 upload command list reset failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    auto toCopyDest = textureTransition(textureResource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    _impl->commandList->ResourceBarrier(1, &toCopyDest);

    // Keep all upload resources alive until after GPU execution completes.
    // Previously, uploadResource was a local inside the for-loop body and was
    // destroyed before CommandList::Close(), which violates D3D12 resource
    // lifetime rules and triggers DEVICE_HUNG / TDR.
    ccstd::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploadResources;
    uploadResources.reserve(count);

    for (uint32_t regionIndex = 0; regionIndex < count; ++regionIndex) {
        if (!buffers[regionIndex]) {
            continue;
        }

        const auto &region = regions[regionIndex];
        const uint32_t mipLevel = region.texSubres.mipLevel;
        const uint32_t arrayLayer = textureInfo.type == TextureType::TEX3D ? 0 : region.texSubres.baseArrayLayer;
        const uint32_t subresource = mipLevel + arrayLayer * textureInfo.levelCount;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 uploadSize = 0;
        D3D12_RESOURCE_DESC textureDesc = textureResource->GetDesc();
        _impl->d3dDevice->GetCopyableFootprints(&textureDesc, subresource, 1, 0, &footprint, &rowCount, &rowSizeInBytes, &uploadSize);
        if (uploadSize == 0 || rowCount == 0) {
            continue;
        }

        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProperties.CreationNodeMask = 1;
        heapProperties.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Alignment = 0;
        uploadDesc.Width = uploadSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.SampleDesc.Quality = 0;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3D12Resource> uploadResource;
        hr = _impl->d3dDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&uploadResource));
        if (FAILED(hr)) {
            CC_LOG_ERROR("CreateCommittedResource(texture upload) failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            continue;
        }

        void *mappedData = nullptr;
        D3D12_RANGE readRange{};
        hr = uploadResource->Map(0, &readRange, &mappedData);
        if (FAILED(hr) || !mappedData) {
            CC_LOG_ERROR("D3D12 texture upload Map failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            continue;
        }

        const uint32_t sourceRowTexels = region.buffStride > 0 ? region.buffStride : region.texExtent.width;
        const uint32_t sourceRows = region.buffTexHeight > 0 ? region.buffTexHeight : region.texExtent.height;
        const uint32_t sourceRowPitch = sourceRowTexels * bytesPerTexel;
        const uint32_t sourceSlicePitch = sourceRowPitch * sourceRows;
        const uint32_t copyRowBytes = region.texExtent.width * bytesPerTexel;
        const uint32_t copyRows = std::min<uint32_t>(region.texExtent.height, rowCount);
        const uint32_t copyDepth = std::max<uint32_t>(region.texExtent.depth, 1);
        const auto *src = buffers[regionIndex] + region.buffOffset;
        auto *dstBytes = static_cast<uint8_t *>(mappedData) + footprint.Offset;

        for (uint32_t z = 0; z < copyDepth; ++z) {
            for (uint32_t row = 0; row < copyRows; ++row) {
                const uint8_t *srcRow = src + z * sourceSlicePitch + row * sourceRowPitch;
                uint8_t *dstRow = dstBytes + z * footprint.Footprint.RowPitch * rowCount + row * footprint.Footprint.RowPitch;
                std::memcpy(dstRow, srcRow, std::min<uint32_t>(copyRowBytes, static_cast<uint32_t>(rowSizeInBytes)));
            }
        }

        D3D12_RANGE writeRange{0, static_cast<SIZE_T>(uploadSize)};
        uploadResource->Unmap(0, &writeRange);

        D3D12_TEXTURE_COPY_LOCATION srcLocation{};
        srcLocation.pResource = uploadResource.Get();
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLocation.PlacedFootprint = footprint;

        D3D12_TEXTURE_COPY_LOCATION dstLocation{};
        dstLocation.pResource = textureResource;
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLocation.SubresourceIndex = subresource;

        D3D12_BOX srcBox{};
        srcBox.left = 0;
        srcBox.top = 0;
        srcBox.front = 0;
        srcBox.right = region.texExtent.width;
        srcBox.bottom = region.texExtent.height;
        srcBox.back = copyDepth;

        _impl->commandList->CopyTextureRegion(
            &dstLocation,
            region.texOffset.x,
            region.texOffset.y,
            region.texOffset.z,
            &srcLocation,
            &srcBox);

        // Transfer ownership to the vector — keeps resource alive until
        // after waitForGpu() below.
        uploadResources.push_back(std::move(uploadResource));
    }

    auto toCommon = textureTransition(textureResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    _impl->commandList->ResourceBarrier(1, &toCommon);

    hr = _impl->commandList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 upload command list close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    ID3D12CommandList *commandLists[] = {_impl->commandList.Get()};
    _impl->graphicsQueue->ExecuteCommandLists(1, commandLists);
    waitForGpu();

    // Now safe to release upload resources — GPU has finished.
    uploadResources.clear();
}

void CCD3D12Device::copyTextureToBuffers(Texture *src, uint8_t *const *buffers, const BufferTextureCopy *region, uint32_t count) {
    (void)src;
    (void)buffers;
    (void)region;
    (void)count;
}

void CCD3D12Device::getQueryPoolResults(QueryPool *queryPool) {
    if (!queryPool) return;
    auto *d3d12Pool = static_cast<CCD3D12QueryPool *>(queryPool);
    d3d12Pool->fetchResults();
}

SampleCount CCD3D12Device::getMaxSampleCount(Format format, TextureUsage usage, TextureFlags flags) const {
    if (!_impl || !_impl->d3dDevice) return SampleCount::X1;

    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
    // Map common Cocos formats to DXGI formats — only need to handle renderable formats
    switch (format) {
        case Format::RGBA8:      dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case Format::BGRA8:      dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case Format::R8:         dxgiFormat = DXGI_FORMAT_R8_UNORM; break;
        case Format::RG8:        dxgiFormat = DXGI_FORMAT_R8G8_UNORM; break;
        case Format::RGBA4:      dxgiFormat = DXGI_FORMAT_B4G4R4A4_UNORM; break;
        case Format::DEPTH:      dxgiFormat = DXGI_FORMAT_D24_UNORM_S8_UINT; break;
        case Format::DEPTH_STENCIL: dxgiFormat = DXGI_FORMAT_D24_UNORM_S8_UINT; break;
        case Format::R16F:       dxgiFormat = DXGI_FORMAT_R16_FLOAT; break;
        case Format::RG16F:      dxgiFormat = DXGI_FORMAT_R16G16_FLOAT; break;
        case Format::RGBA16F:    dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case Format::R32F:       dxgiFormat = DXGI_FORMAT_R32_FLOAT; break;
        case Format::RG32F:      dxgiFormat = DXGI_FORMAT_R32G32_FLOAT; break;
        case Format::RGBA32F:    dxgiFormat = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        case Format::R11G11B10F: dxgiFormat = DXGI_FORMAT_R11G11B10_FLOAT; break;
        case Format::RGB10A2:    dxgiFormat = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        case Format::RGB9E5:     dxgiFormat = DXGI_FORMAT_R9G9B9E5_SHAREDEXP; break;
        default:                 dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM; break; // fallback
    }

    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qualityLevels{};
    qualityLevels.Format = dxgiFormat;
    qualityLevels.SampleCount = 1; // start checking
    qualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;

    // Check sample counts from highest to lowest
    for (uint32_t sampleCount = 32; sampleCount >= 2; sampleCount /= 2) {
        qualityLevels.SampleCount = sampleCount;
        if (SUCCEEDED(_impl->d3dDevice->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qualityLevels, sizeof(qualityLevels)))) {
            if (qualityLevels.NumQualityLevels > 0) {
                return static_cast<SampleCount>(sampleCount);
            }
        }
    }
    return SampleCount::X1;
}

void CCD3D12Device::initFormatFeatures() {
    // Hardcoded format feature table for D3D12 FL11_0+ hardware.
    // Same approach as GLES3Device::initFormatFeature() — no runtime API calls needed.
    // D3D12 FL11_0 guarantees support for all these formats.

    // Full set: SAMPLED_TEXTURE | RENDER_TARGET | LINEAR_FILTER | STORAGE_TEXTURE | VERTEX_ATTRIBUTE
    const auto F_FULL = FormatFeature::SAMPLED_TEXTURE | FormatFeature::RENDER_TARGET |
                         FormatFeature::LINEAR_FILTER | FormatFeature::STORAGE_TEXTURE |
                         FormatFeature::VERTEX_ATTRIBUTE;
    // No render target.
    const auto F_NO_RT = FormatFeature::SAMPLED_TEXTURE | FormatFeature::STORAGE_TEXTURE |
                          FormatFeature::VERTEX_ATTRIBUTE;
    // Single-channel 32-bit float is renderable and is used by shadow maps when available.
    const auto F_R32F = FormatFeature::SAMPLED_TEXTURE | FormatFeature::RENDER_TARGET |
                         FormatFeature::STORAGE_TEXTURE | FormatFeature::VERTEX_ATTRIBUTE;
    // Standard: SAMPLED_TEXTURE | RENDER_TARGET | LINEAR_FILTER | STORAGE_TEXTURE (no vertex)
    const auto F_STD = FormatFeature::SAMPLED_TEXTURE | FormatFeature::RENDER_TARGET |
                        FormatFeature::LINEAR_FILTER | FormatFeature::STORAGE_TEXTURE;
    // Depth: SAMPLED_TEXTURE | RENDER_TARGET only
    const auto F_DEPTH = FormatFeature::SAMPLED_TEXTURE | FormatFeature::RENDER_TARGET;

    // --- 8-bit normalized ---
    _formatFeatures[toNumber(Format::R8)]          = F_FULL;
    _formatFeatures[toNumber(Format::R8SN)]        = F_STD;
    _formatFeatures[toNumber(Format::RG8)]         = F_FULL;
    _formatFeatures[toNumber(Format::RG8SN)]       = F_STD;
    _formatFeatures[toNumber(Format::RGB8)]        = F_FULL;
    _formatFeatures[toNumber(Format::RGB8SN)]      = F_STD;
    _formatFeatures[toNumber(Format::RGBA8)]       = F_FULL;
    _formatFeatures[toNumber(Format::RGBA8SN)]     = F_STD;
    _formatFeatures[toNumber(Format::BGRA8)]       = F_FULL;
    _formatFeatures[toNumber(Format::SRGB8)]       = F_STD;
    _formatFeatures[toNumber(Format::SRGB8_A8)]    = F_STD;

    // --- 8-bit integer ---
    _formatFeatures[toNumber(Format::R8I)]         = F_FULL;
    _formatFeatures[toNumber(Format::R8UI)]        = F_FULL;
    _formatFeatures[toNumber(Format::RG8I)]        = F_FULL;
    _formatFeatures[toNumber(Format::RG8UI)]       = F_FULL;
    _formatFeatures[toNumber(Format::RGB8I)]       = F_FULL;
    _formatFeatures[toNumber(Format::RGB8UI)]      = F_FULL;
    _formatFeatures[toNumber(Format::RGBA8I)]      = F_FULL;
    _formatFeatures[toNumber(Format::RGBA8UI)]     = F_FULL;

    // --- 16-bit float ---
    _formatFeatures[toNumber(Format::R16F)]        = F_FULL;
    _formatFeatures[toNumber(Format::RG16F)]       = F_FULL;
    _formatFeatures[toNumber(Format::RGB16F)]      = F_FULL;
    _formatFeatures[toNumber(Format::RGBA16F)]     = F_FULL;

    // --- 16-bit integer ---
    _formatFeatures[toNumber(Format::R16I)]        = F_FULL;
    _formatFeatures[toNumber(Format::R16UI)]       = F_FULL;
    _formatFeatures[toNumber(Format::RG16I)]       = F_FULL;
    _formatFeatures[toNumber(Format::RG16UI)]      = F_FULL;
    _formatFeatures[toNumber(Format::RGB16I)]      = F_FULL;
    _formatFeatures[toNumber(Format::RGB16UI)]     = F_FULL;
    _formatFeatures[toNumber(Format::RGBA16I)]     = F_FULL;
    _formatFeatures[toNumber(Format::RGBA16UI)]    = F_FULL;

    // --- 32-bit float ---
    _formatFeatures[toNumber(Format::R32F)]        = F_R32F;
    _formatFeatures[toNumber(Format::RG32F)]       = F_NO_RT;
    _formatFeatures[toNumber(Format::RGB32F)]      = F_NO_RT;
    _formatFeatures[toNumber(Format::RGBA32F)]     = F_NO_RT;

    // --- 32-bit integer ---
    _formatFeatures[toNumber(Format::R32I)]        = F_FULL;
    _formatFeatures[toNumber(Format::R32UI)]       = F_FULL;
    _formatFeatures[toNumber(Format::RG32I)]       = F_FULL;
    _formatFeatures[toNumber(Format::RG32UI)]      = F_FULL;
    _formatFeatures[toNumber(Format::RGB32I)]      = F_FULL;
    _formatFeatures[toNumber(Format::RGB32UI)]     = F_FULL;
    _formatFeatures[toNumber(Format::RGBA32I)]     = F_FULL;
    _formatFeatures[toNumber(Format::RGBA32UI)]    = F_FULL;

    // --- Packed / special ---
    _formatFeatures[toNumber(Format::RGB10A2)]     = F_STD;
    _formatFeatures[toNumber(Format::RGB10A2UI)]   = F_STD;
    _formatFeatures[toNumber(Format::R11G11B10F)]  = F_STD;
    _formatFeatures[toNumber(Format::RGB9E5)]      = F_STD;
    _formatFeatures[toNumber(Format::R5G6B5)]      = F_STD;
    _formatFeatures[toNumber(Format::RGBA4)]       = F_STD;
    _formatFeatures[toNumber(Format::RGB5A1)]      = F_STD;

    // --- Depth ---
    _formatFeatures[toNumber(Format::DEPTH)]       = F_DEPTH;
    _formatFeatures[toNumber(Format::DEPTH_STENCIL)] = F_DEPTH;

    // --- BC compressed (SAMPLED_TEXTURE only, no render target) ---
    const auto F_BC = FormatFeature::SAMPLED_TEXTURE | FormatFeature::LINEAR_FILTER;
    _formatFeatures[toNumber(Format::BC1)]           = F_BC;
    _formatFeatures[toNumber(Format::BC1_ALPHA)]     = F_BC;
    _formatFeatures[toNumber(Format::BC1_SRGB)]      = F_BC;
    _formatFeatures[toNumber(Format::BC1_SRGB_ALPHA)]= F_BC;
    _formatFeatures[toNumber(Format::BC2)]           = F_BC;
    _formatFeatures[toNumber(Format::BC2_SRGB)]      = F_BC;
    _formatFeatures[toNumber(Format::BC3)]           = F_BC;
    _formatFeatures[toNumber(Format::BC3_SRGB)]      = F_BC;
    _formatFeatures[toNumber(Format::BC4)]           = FormatFeature::SAMPLED_TEXTURE;
    _formatFeatures[toNumber(Format::BC4_SNORM)]     = FormatFeature::SAMPLED_TEXTURE;
    _formatFeatures[toNumber(Format::BC5)]           = FormatFeature::SAMPLED_TEXTURE;
    _formatFeatures[toNumber(Format::BC5_SNORM)]     = FormatFeature::SAMPLED_TEXTURE;
    _formatFeatures[toNumber(Format::BC6H_UF16)]     = FormatFeature::SAMPLED_TEXTURE;
    _formatFeatures[toNumber(Format::BC6H_SF16)]     = FormatFeature::SAMPLED_TEXTURE;
    _formatFeatures[toNumber(Format::BC7)]           = F_BC;
    _formatFeatures[toNumber(Format::BC7_SRGB)]      = F_BC;

    // ETC / ASTC — D3D12 has no native support, leave as NONE

    CC_LOG_INFO("[D3D12] Format features initialized (hardcoded table).");
}

void CCD3D12Device::initCapabilities() {
    // D3D12 with FL11_0+ guarantees these minimums.
    // Values are conservative but sufficient for Cocos Creator's pipeline.
    _caps.maxVertexAttributes = 16;
    _caps.maxVertexUniformVectors = 4096;   // D3D12: 64KB constant buffer / 16 bytes per vec4
    _caps.maxFragmentUniformVectors = 4096;  // Same as vertex
    _caps.maxTextureUnits = 32;              // D3D12 FL11_0: 128 texture slots, but engine uses fewer
    _caps.maxImageUnits = 8;                 // UAV bindless
    _caps.maxVertexTextureUnits = 32;
    _caps.maxColorRenderTargets = 8;         // D3D12 FL11_0: 8 render targets
    _caps.maxShaderStorageBufferBindings = 64;
    _caps.maxShaderStorageBlockSize = 1 << 27; // 128MB
    _caps.maxUniformBufferBindings = 14;     // Standard D3D12 CBV slots per shader stage
    _caps.maxUniformBlockSize = 65536;       // 64KB constant buffer
    _caps.maxTextureSize = 16384;            // D3D12 FL11_0 minimum
    _caps.maxCubeMapTextureSize = 16384;
    _caps.maxArrayTextureLayers = 2048;      // D3D12 FL11_0 minimum
    _caps.max3DTextureSize = 2048;           // D3D12 FL11_0 minimum
    _caps.uboOffsetAlignment = 256;          // D3D12 constant buffer offset alignment (16-byte CBV, 256 for HLSL)

    _caps.maxComputeSharedMemorySize = 32768;  // D3D12 FL11_0 minimum
    _caps.maxComputeWorkGroupInvocations = 1024;
    _caps.maxComputeWorkGroupSize = {1024, 1024, 64};
    _caps.maxComputeWorkGroupCount = {65535, 65535, 65535};

    _caps.supportQuery = true;
    _caps.supportVariableRateShading = false;
    _caps.supportSubPassShading = false;

    // D3D12 uses the same render-target sampling orientation as Metal/WGPU while
    // keeping D3D-style clip space. This keeps CSM atlas writes and shadow-map
    // sampling on the same Y convention.
    _caps.clipSpaceMinZ = 0.F;
    _caps.screenSpaceSignY = -1.F;
    _caps.clipSpaceSignY = 1.F;
}

bool CCD3D12Device::initializeD3D12Context() {
    UINT dxgiFactoryFlags = 0;

    // Enable debug layer only in Debug builds; too heavy for Release and
    // can cause stability issues on some AMD drivers.
#if !defined(NDEBUG)
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        } else {
            CC_LOG_WARNING("Could not enable D3D12 debug layer.");
        }
    }
#endif

    HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&_impl->dxgiFactory));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateDXGIFactory2 failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;

    // Enumerate all adapters and pick the best one:
    // 1. Skip software adapters (WARP)
    // 2. Prefer discrete GPU (highest DedicatedVideoMemory)
    // 3. Fall back to integrated GPU if no discrete GPU available
    // 4. Last resort: WARP software adapter
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> bestAdapter;
        DXGI_ADAPTER_DESC1 bestDesc{};
        bool foundDiscrete = false;

        for (UINT adapterIndex = 0; _impl->dxgiFactory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex) {
            DXGI_ADAPTER_DESC1 adapterDesc{};
            adapter->GetDesc1(&adapterDesc);

            // Skip software adapters
            if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                adapter.Reset();
                continue;
            }

            // Check D3D12 support before considering this adapter
            Microsoft::WRL::ComPtr<ID3D12Device> testDevice;
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice)))) {
                adapter.Reset();
                continue;
            }

            char adapterName[128] = {};
            wcstombs(adapterName, adapterDesc.Description, sizeof(adapterName) - 1);
            bool isDiscrete = (adapterDesc.DedicatedVideoMemory > 0 &&
                              adapterDesc.SharedSystemMemory > 0) ||
                             (adapterDesc.VendorId == 0x10DE) ||  // NVIDIA
                             (adapterDesc.VendorId == 0x1002);    // AMD
            CC_LOG_INFO("Adapter[%u]: %s (VRAM=%llu MB, Shared=%llu MB, VendorID=0x%04x, Discrete=%s)",
                         adapterIndex, adapterName,
                         static_cast<unsigned long long>(adapterDesc.DedicatedVideoMemory / (1024 * 1024)),
                         static_cast<unsigned long long>(adapterDesc.SharedSystemMemory / (1024 * 1024)),
                         static_cast<unsigned>(adapterDesc.VendorId),
                         isDiscrete ? "yes" : "no");

            // Selection priority: discrete > integrated (by VRAM size within category)
            if (!foundDiscrete && isDiscrete) {
                // First discrete GPU found — always pick it over any integrated
                bestAdapter = adapter;
                bestDesc = adapterDesc;
                foundDiscrete = true;
            } else if (foundDiscrete && isDiscrete &&
                       adapterDesc.DedicatedVideoMemory > bestDesc.DedicatedVideoMemory) {
                // Better discrete GPU found
                bestAdapter = adapter;
                bestDesc = adapterDesc;
            } else if (!foundDiscrete &&
                       (!bestAdapter || adapterDesc.DedicatedVideoMemory > bestDesc.DedicatedVideoMemory)) {
                // No discrete found yet, pick the one with most VRAM
                bestAdapter = adapter;
                bestDesc = adapterDesc;
            }

            adapter.Reset();
        }

        if (bestAdapter) {
            hr = D3D12CreateDevice(bestAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_impl->d3dDevice));
            if (SUCCEEDED(hr)) {
                char name[128] = {};
                wcstombs(name, bestDesc.Description, sizeof(name) - 1);
                CC_LOG_INFO("D3D12: selected adapter '%s' (VRAM=%llu MB, VendorID=0x%04x)",
                            name,
                            static_cast<unsigned long long>(bestDesc.DedicatedVideoMemory / (1024 * 1024)),
                            static_cast<unsigned>(bestDesc.VendorId));
            }
        }
    }

    if (!_impl->d3dDevice) {
        CC_LOG_WARNING("No adapter worked, trying WARP (default adapter)...");
        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_impl->d3dDevice));
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12CreateDevice failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return false;
        }
    }

    // Setup ID3D12InfoQueue to capture actionable D3D12 debug layer messages.
    {
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(_impl->d3dDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
            D3D12_MESSAGE_SEVERITY denySeverities[] = {
                D3D12_MESSAGE_SEVERITY_INFO,
                D3D12_MESSAGE_SEVERITY_MESSAGE,
            };
            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumSeverities = _countof(denySeverities);
            filter.DenyList.pSeverityList = denySeverities;
            infoQueue->PushStorageFilter(&filter);
            infoQueue->SetMessageCountLimit(4096);
        }
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    hr = _impl->d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_impl->graphicsQueue));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateCommandQueue failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    hr = _impl->d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_impl->commandAllocator));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateCommandAllocator failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    hr = _impl->d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _impl->commandAllocator.Get(), nullptr, IID_PPV_ARGS(&_impl->commandList));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateCommandList failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    hr = _impl->commandList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("Initial command list close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    hr = _impl->d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_impl->frameFence));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateFence failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    _impl->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!_impl->fenceEvent) {
        CC_LOG_ERROR("CreateEvent for D3D12 fence failed.");
        return false;
    }

    _impl->fenceValue = 0;

    return true;
}

void CCD3D12Device::waitForGpu() {
    if (!_impl->graphicsQueue || !_impl->frameFence || !_impl->fenceEvent) {
        return;
    }

    ++_impl->fenceValue;
    HRESULT hr = _impl->graphicsQueue->Signal(_impl->frameFence.Get(), _impl->fenceValue);
    if (FAILED(hr)) {
        CC_LOG_ERROR("waitForGpu signal failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    if (_impl->frameFence->GetCompletedValue() < _impl->fenceValue) {
        hr = _impl->frameFence->SetEventOnCompletion(_impl->fenceValue, _impl->fenceEvent);
        if (FAILED(hr)) {
            CC_LOG_ERROR("waitForGpu SetEventOnCompletion failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return;
        }
        WaitForSingleObject(_impl->fenceEvent, INFINITE);
    }
}

void *CCD3D12Device::getD3D12DeviceHandle() const {
    return _impl ? _impl->d3dDevice.Get() : nullptr;
}

void *CCD3D12Device::getGraphicsQueueHandle() const {
    return _impl ? _impl->graphicsQueue.Get() : nullptr;
}

void *CCD3D12Device::getDXGIFactoryHandle() const {
    return _impl ? _impl->dxgiFactory.Get() : nullptr;
}

D3D12DescriptorHeapPool *CCD3D12Device::getGPUDescriptorHeapPool() const {
    return _impl ? _impl->gpuDescriptorHeapPool.get() : nullptr;
}

D3D12DescriptorHeapPool *CCD3D12Device::getSamplerDescriptorHeapPool() const {
    return _impl ? _impl->samplerDescriptorHeapPool.get() : nullptr;
}

} // namespace gfx
} // namespace cc
