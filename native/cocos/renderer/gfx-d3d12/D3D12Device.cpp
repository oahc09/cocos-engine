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
#include "D3D12DebugOptimization.h"
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
#include <array>
#include <atomic>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace cc {
namespace gfx {

CCD3D12Device *CCD3D12Device::instance = nullptr;
D3D12TransientUniformFrameState CCD3D12Device::activeTransientUniformFrameState{};

namespace {
constexpr uint32_t TRANSIENT_UNIFORM_SLOT_COUNT = 4096U;
std::atomic<uint64_t> NEXT_D3D12_DEVICE_EPOCH{1};

constexpr float D3D12_POC_CLEAR_COLOR[4] = {0.1F, 0.2F, 0.8F, 1.0F};
constexpr uint32_t D3D12_GPU_DESCRIPTORS_PER_FRAME = 65536;

bool isEnvironmentFlagEnabled(const char *name) {
    char value[16]{};
    const DWORD length = GetEnvironmentVariableA(
        name, value, static_cast<DWORD>(sizeof(value)));
    if (length == 0 || length >= sizeof(value)) {
        return false;
    }
    return std::strcmp(value, "1") == 0 ||
           _stricmp(value, "true") == 0 ||
           _stricmp(value, "on") == 0;
}

bool isD3D12DebugLayerRequested() {
    return isEnvironmentFlagEnabled("CC_D3D12_DEBUG_LAYER");
}

#if defined(__ID3D12Device9_INTERFACE_DEFINED__) && defined(__ID3D12ShaderCacheSession_INTERFACE_DEFINED__)
constexpr GUID D3D12_COCOS_DXBC_SHADER_CACHE_GUID = {
    0x9f4e1b8d, 0x932a, 0x4a64, {0xa8, 0x4e, 0x31, 0x79, 0x50, 0x8f, 0xb8, 0xd2}};
constexpr uint64_t D3D12_COCOS_DXBC_SHADER_CACHE_VERSION = 1;
#endif

void copyReadbackToBuffer(const uint8_t *mappedData, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint, uint32_t footprintRowCount,
                          uint8_t *buffer, const BufferTextureCopy &region, Format format) {
    const uint32_t rowStrideWidth = region.buffStride > 0 ? region.buffStride : region.texExtent.width;
    const uint32_t sliceStrideHeight = region.buffTexHeight > 0 ? region.buffTexHeight : region.texExtent.height;
    const uint32_t dstRowStride = formatSize(format, rowStrideWidth, 1, 1);
    const uint32_t dstSliceStride = formatSize(format, rowStrideWidth, sliceStrideHeight, 1);
    const uint32_t copyRowBytes = formatSize(format, region.texExtent.width, 1, 1);
    const auto blockAlignment = formatAlignment(format);
    const uint32_t blockHeight = std::max<uint32_t>(blockAlignment.second, 1);
    const uint32_t copyRows = (region.texExtent.height + blockHeight - 1) / blockHeight;
    const uint32_t copyDepth = std::max<uint32_t>(region.texExtent.depth, 1);

    const auto *srcBase = mappedData + footprint.Offset;
    auto *dstBase = buffer + region.buffOffset;
    for (uint32_t z = 0; z < copyDepth; ++z) {
        const auto *srcSlice = srcBase + static_cast<size_t>(z) * footprint.Footprint.RowPitch * footprintRowCount;
        auto *dstSlice = dstBase + static_cast<size_t>(z) * dstSliceStride;
        for (uint32_t row = 0; row < copyRows; ++row) {
            const auto *srcRow = srcSlice + static_cast<size_t>(row) * footprint.Footprint.RowPitch;
            auto *dstRow = dstSlice + static_cast<size_t>(row) * dstRowStride;
            std::memcpy(dstRow, srcRow, copyRowBytes);
        }
    }
}
} // namespace

struct CCD3D12Device::Impl {
    Microsoft::WRL::ComPtr<IDXGIFactory6> dxgiFactory;
    Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> graphicsQueue;
    // Device-level command allocator/list for legacy present and readback paths.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> frameFence;

    HANDLE fenceEvent{nullptr};
    uint64_t fenceValue{0};
    Microsoft::WRL::ComPtr<ID3D12Fence> lastSubmittedFence;
    uint64_t lastSubmittedFenceValue{0};
    uint64_t bufferStateEpoch{1};
    uint64_t deviceEpoch{0};
    uint64_t transientUniformUploadGeneration{1};

    struct UploadPage {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint8_t *mappedData{nullptr};
        uint64_t size{0};
        uint64_t offset{0};
    };

    struct FrameResources {
        std::unique_ptr<D3D12DescriptorHeapPool> samplerDescriptorHeapPool;
        ccstd::vector<UploadPage> uploadPages;
        UploadPage transientUniformSlotArena;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t fenceValue{0};
    };
    std::array<FrameResources, D3D12_MAX_FRAMES_IN_FLIGHT> frameResources;
    uint32_t activeFrameResource{D3D12_MAX_FRAMES_IN_FLIGHT - 1U};
    uint32_t nextTransientUniformSlotIndex{0};

    // GPU-visible descriptor heap pools for shader access.
    std::unique_ptr<D3D12DescriptorHeapPool> gpuDescriptorHeapPool;

    // Persistent CPU-only staging pools. Unlike GPU pools, these are not reset per frame.
    std::unique_ptr<D3D12DescriptorHeapPool> cpuDescriptorHeapPool;
    std::unique_ptr<D3D12DescriptorHeapPool> cpuSamplerDescriptorHeapPool;
    // Non-owning actors: Validator owns backend resources with raw pointers,
    // so intrusive ownership here would delete an actor when the queue drains.
    // CCD3D12Buffer::doDestroy unregisters itself before releasing resources.
    ccstd::vector<CCD3D12Buffer *> pendingBufferUpdates;

    struct PendingUploadCommandContext {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
        ccstd::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> referencedResources;
        ccstd::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> descriptorHeaps;
        uint64_t fenceValue{0};
    };
    ccstd::vector<PendingUploadCommandContext> pendingUploadCommandContexts;

    struct DeferredCubeUpload {
        Texture *texture{nullptr};
        ID3D12Resource *resource{nullptr};
        ccstd::vector<ccstd::vector<uint8_t>> layerData;
        ccstd::vector<BufferTextureCopy> layerRegions;
        uint32_t receivedMask{0};
        uint32_t receivedCount{0};
    };
    ccstd::vector<DeferredCubeUpload> deferredCubeUploads;

    // Dummy resources for safe null descriptor bindings
    IntrusivePtr<CCD3D12Texture> dummyTexture;
    IntrusivePtr<CCD3D12Buffer>  dummyBuffer;

    // Command signatures for ExecuteIndirect (indirect draw / dispatch)
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> drawIndirectSig;       // DrawInstanced
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> drawIndexedIndirectSig; // DrawIndexedInstanced
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatchIndirectSig;   // Dispatch
    struct LocalRootCbvIndirectSignature {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
        Microsoft::WRL::ComPtr<ID3D12CommandSignature> signature;
        uint32_t rootParameterIndices[D3D12_MAX_LOCAL_ROOT_CBVS]{};
        uint32_t rootParameterCount{0};
        uint32_t byteStride{0};
        bool indexed{false};
    };
    ccstd::vector<LocalRootCbvIndirectSignature> localRootCbvIndirectSignatures;

    std::mutex nativePipelineCacheMutex;
    std::unordered_map<uint32_t, std::shared_ptr<void>> blitPipelineCache;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> emptyRootSignature;

#if defined(__ID3D12Device9_INTERFACE_DEFINED__) && defined(__ID3D12ShaderCacheSession_INTERFACE_DEFINED__)
    Microsoft::WRL::ComPtr<ID3D12ShaderCacheSession> shaderCacheSession;
    std::mutex shaderCacheMutex;
#endif

};

CCD3D12Device *CCD3D12Device::getInstance() {
    return CCD3D12Device::instance;
}

CCD3D12Device::CCD3D12Device()
: _impl(std::make_unique<Impl>()) {
    _impl->deviceEpoch = NEXT_D3D12_DEVICE_EPOCH.fetch_add(1, std::memory_order_relaxed);
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
    initializeShaderCacheSession();

    _impl->gpuDescriptorHeapPool = std::make_unique<D3D12DescriptorHeapPool>();
    _impl->gpuDescriptorHeapPool->initialize(
        D3D12DescriptorHeapPool::HeapType::CBV_SRV_UAV,
        D3D12_GPU_DESCRIPTORS_PER_FRAME * D3D12_MAX_FRAMES_IN_FLIGHT, true);

    for (auto &frameResources : _impl->frameResources) {
        frameResources.samplerDescriptorHeapPool = std::make_unique<D3D12DescriptorHeapPool>();
        frameResources.samplerDescriptorHeapPool->initialize(
            D3D12DescriptorHeapPool::HeapType::SAMPLER, 2048, true);
    }

    _impl->cpuDescriptorHeapPool = std::make_unique<D3D12DescriptorHeapPool>();
    _impl->cpuDescriptorHeapPool->initialize(
        D3D12DescriptorHeapPool::HeapType::CBV_SRV_UAV, 16384, false);

    _impl->cpuSamplerDescriptorHeapPool = std::make_unique<D3D12DescriptorHeapPool>();
    _impl->cpuSamplerDescriptorHeapPool->initialize(
        D3D12DescriptorHeapPool::HeapType::SAMPLER, 2048, false);

    CC_D3D12_DIAGNOSTIC_LOG("D3D12 descriptor heap pools initialized.");

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
    _features[toNumber(Feature::COMPUTE_SHADER)] = false;
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
            CC_D3D12_DIAGNOSTIC_LOG("[D3D12] Dummy resources created for null descriptor bindings");
        } else {
            CC_LOG_WARNING("[D3D12] Failed to create dummy resources; null descriptors will be skipped");
        }
    }

    CC_LOG_INFO("D3D12 device initialized.");
    CC_LOG_INFO("RENDERER: %s", _renderer.c_str());
    CC_LOG_INFO("VENDOR: %s", _vendor.c_str());
    CC_LOG_INFO("CAPS: maxVertexUniformVectors=%u, maxFragmentUniformVectors=%u, maxTextureSize=%u",
                _caps.maxVertexUniformVectors, _caps.maxFragmentUniformVectors, _caps.maxTextureSize);

    reopenD3D12ShaderCachePersistence();

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
    activeTransientUniformFrameState = {};
    drainD3D12ShaderCachePersistence();
    _d3d12Swapchains.clear();
    if (_impl) {
        _impl->deferredCubeUploads.clear();
    }
    if (!waitIdle()) {
        CC_LOG_WARNING("D3D12 device teardown is continuing after waitIdle failed.");
    }
    _impl->pendingBufferUpdates.clear();
    _impl->pendingUploadCommandContexts.clear();
    _impl->deferredCubeUploads.clear();

    // Release objects that may retain command allocators, command lists, and
    // resources while the native device and queue are still available.
    CC_SAFE_DESTROY_AND_DELETE(_cmdBuff);
    CC_SAFE_DESTROY_AND_DELETE(_queryPool);
    CC_SAFE_DESTROY_AND_DELETE(_queue);

    {
        std::lock_guard<std::mutex> lock(_impl->nativePipelineCacheMutex);
        _impl->blitPipelineCache.clear();
        _impl->emptyRootSignature.Reset();
    }

    // Release dummy resources
    _impl->dummyTexture = nullptr;
    _impl->dummyBuffer = nullptr;

    // Shutdown descriptor heap pools first
    if (_impl->cpuSamplerDescriptorHeapPool) {
        _impl->cpuSamplerDescriptorHeapPool->shutdown();
        _impl->cpuSamplerDescriptorHeapPool.reset();
    }
    if (_impl->cpuDescriptorHeapPool) {
        _impl->cpuDescriptorHeapPool->shutdown();
        _impl->cpuDescriptorHeapPool.reset();
    }
    for (auto &frameResources : _impl->frameResources) {
        frameResources.uploadPages.clear();
        frameResources.fence.Reset();
        frameResources.fenceValue = 0;
        if (frameResources.samplerDescriptorHeapPool) {
            frameResources.samplerDescriptorHeapPool->shutdown();
            frameResources.samplerDescriptorHeapPool.reset();
        }
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
#if defined(__ID3D12Device9_INTERFACE_DEFINED__) && defined(__ID3D12ShaderCacheSession_INTERFACE_DEFINED__)
    {
        std::lock_guard<std::mutex> lock(_impl->shaderCacheMutex);
        _impl->shaderCacheSession.Reset();
    }
#endif
    _impl->d3dDevice.Reset();
    _impl->dxgiFactory.Reset();
}

void CCD3D12Device::initializeShaderCacheSession() {
#if defined(__ID3D12Device9_INTERFACE_DEFINED__) && defined(__ID3D12ShaderCacheSession_INTERFACE_DEFINED__)
    if (!_impl || !_impl->d3dDevice) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12Device9> device9;
    HRESULT hr = _impl->d3dDevice.As(&device9);
    if (FAILED(hr) || !device9) {
        CC_D3D12_DIAGNOSTIC_LOG("D3D12 Shader Cache Session unavailable: ID3D12Device9 not supported. HRESULT=0x%08x",
                    static_cast<unsigned>(hr));
        return;
    }

    D3D12_SHADER_CACHE_SESSION_DESC desc{};
    desc.Identifier = D3D12_COCOS_DXBC_SHADER_CACHE_GUID;
    desc.Mode = D3D12_SHADER_CACHE_MODE_DISK;
    desc.Flags = static_cast<D3D12_SHADER_CACHE_FLAGS>(0);
    desc.MaximumInMemoryCacheSizeBytes = 8U * 1024U * 1024U;
    desc.MaximumInMemoryCacheEntries = 4096U;
    desc.MaximumValueFileSizeBytes = 256U * 1024U * 1024U;
    desc.Version = D3D12_COCOS_DXBC_SHADER_CACHE_VERSION;

    hr = device9->CreateShaderCacheSession(&desc, IID_PPV_ARGS(&_impl->shaderCacheSession));
    if (SUCCEEDED(hr) && _impl->shaderCacheSession) {
        CC_D3D12_DIAGNOSTIC_LOG("D3D12 Shader Cache Session initialized for DXBC cache.");
    } else {
        CC_LOG_WARNING("D3D12 Shader Cache Session initialization failed. HRESULT=0x%08x",
                       static_cast<unsigned>(hr));
    }
#else
    CC_D3D12_DIAGNOSTIC_LOG("D3D12 Shader Cache Session unavailable: SDK headers do not expose ID3D12Device9.");
#endif
}

bool CCD3D12Device::loadShaderCacheValue(const void *key, uint32_t keySize, std::vector<uint8_t> &outValue) const {
#if defined(__ID3D12Device9_INTERFACE_DEFINED__) && defined(__ID3D12ShaderCacheSession_INTERFACE_DEFINED__)
    if (!_impl || !key || keySize == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(_impl->shaderCacheMutex);
    if (!_impl->shaderCacheSession) {
        return false;
    }

    UINT valueSize = 0;
    HRESULT hr = _impl->shaderCacheSession->FindValue(key, keySize, nullptr, &valueSize);
    if (FAILED(hr) || valueSize == 0) {
        return false;
    }

    outValue.resize(valueSize);
    hr = _impl->shaderCacheSession->FindValue(key, keySize, outValue.data(), &valueSize);
    if (FAILED(hr) || valueSize == 0) {
        outValue.clear();
        return false;
    }

    outValue.resize(valueSize);
    return true;
#else
    (void)key;
    (void)keySize;
    (void)outValue;
    return false;
#endif
}

bool CCD3D12Device::storeShaderCacheValue(const void *key, uint32_t keySize, const std::vector<uint8_t> &value) const {
#if defined(__ID3D12Device9_INTERFACE_DEFINED__) && defined(__ID3D12ShaderCacheSession_INTERFACE_DEFINED__)
    if (!_impl || !key || keySize == 0 || value.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(_impl->shaderCacheMutex);
    if (!_impl->shaderCacheSession) {
        return false;
    }

    // StoreValue reports an error when the key already exists. File-cache
    // hydration can race for identical fullSourceKeys, so make the operation
    // idempotent while holding the same session mutex.
    UINT existingValueSize = 0;
    const HRESULT findHr = _impl->shaderCacheSession->FindValue(key, keySize, nullptr, &existingValueSize);
    if (SUCCEEDED(findHr) && existingValueSize > 0) {
        return true;
    }
    HRESULT hr = _impl->shaderCacheSession->StoreValue(key, keySize, value.data(), static_cast<UINT>(value.size()));
    return SUCCEEDED(hr);
#else
    (void)key;
    (void)keySize;
    (void)value;
    return false;
#endif
}

void CCD3D12Device::acquire(Swapchain *const *swapchains, uint32_t count) {
    retireFrameResources();

    const uint32_t nextFrameResource =
        (_impl->activeFrameResource + 1U) % D3D12_MAX_FRAMES_IN_FLIGHT;
    auto &frameResources = _impl->frameResources[nextFrameResource];
    if (frameResources.fence && frameResources.fenceValue != 0 &&
        frameResources.fence->GetCompletedValue() < frameResources.fenceValue) {
        bool frameResourceReady = false;
        const HRESULT hr = frameResources.fence->SetEventOnCompletion(
            frameResources.fenceValue, _impl->fenceEvent);
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12 device failed to wait for frame-resource reuse. HRESULT=0x%08x",
                         static_cast<unsigned>(hr));
        } else {
            const DWORD waitResult = WaitForSingleObject(_impl->fenceEvent, INFINITE);
            if (waitResult != WAIT_OBJECT_0) {
                CC_LOG_ERROR("D3D12 frame-resource wait failed. result=%lu",
                             static_cast<unsigned long>(waitResult));
            } else {
                const uint64_t completedValue = frameResources.fence->GetCompletedValue();
                frameResourceReady =
                    completedValue != std::numeric_limits<UINT64>::max() &&
                    completedValue >= frameResources.fenceValue;
                if (!frameResourceReady) {
                    CC_LOG_ERROR(
                        "D3D12 frame-resource completion is unresolved. completed=%llu expected=%llu",
                        static_cast<unsigned long long>(completedValue),
                        static_cast<unsigned long long>(frameResources.fenceValue));
                }
            }
        }
        if (!frameResourceReady) {
            if (!waitIdle()) {
                CC_LOG_ERROR(
                    "D3D12 acquire refused to reuse unresolved frame resources.");
                return;
            }
        }
    }

    if (_impl->gpuDescriptorHeapPool) {
        _impl->gpuDescriptorHeapPool->beginFrameAllocationRange(
            nextFrameResource * D3D12_GPU_DESCRIPTORS_PER_FRAME,
            D3D12_GPU_DESCRIPTORS_PER_FRAME);
    }
    if (frameResources.samplerDescriptorHeapPool) {
        frameResources.samplerDescriptorHeapPool->reset();
    }
    if (!frameResources.uploadPages.empty()) {
        for (auto &page : frameResources.uploadPages) {
            page.offset = 0;
        }
    }
    frameResources.fence.Reset();
    frameResources.fenceValue = 0;
    _impl->activeFrameResource = nextFrameResource;
    activeTransientUniformFrameState = {};
    activeTransientUniformFrameState.epoch = _impl->bufferStateEpoch;
    const auto &activeArena = frameResources.transientUniformSlotArena;
    if (activeArena.resource && activeArena.mappedData) {
        activeTransientUniformFrameState.resource = activeArena.resource.Get();
        activeTransientUniformFrameState.mappedData = activeArena.mappedData;
        activeTransientUniformFrameState.gpuAddress =
            activeArena.resource->GetGPUVirtualAddress();
        activeTransientUniformFrameState.isValid = true;
    }
    // Ordinary uniform descriptors may reference a frame-local upload page or
    // stable slot. Switching frame resources invalidates those GPU addresses
    // even when the CPU-side uniform value itself did not change.
    notifyTransientUniformUpload();

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

uint64_t CCD3D12Device::getBufferStateEpoch() const {
    return _impl ? _impl->bufferStateEpoch : 0;
}

uint64_t CCD3D12Device::getDeviceEpoch() const {
    return _impl ? _impl->deviceEpoch : 0;
}

uint32_t CCD3D12Device::getActiveFrameResourceIndex() const {
    return _impl ? _impl->activeFrameResource : 0;
}

uint64_t CCD3D12Device::getTransientUniformUploadGeneration() const {
    return _impl ? _impl->transientUniformUploadGeneration : 0;
}

void CCD3D12Device::notifyTransientUniformUpload() {
    if (_impl) {
        ++_impl->transientUniformUploadGeneration;
    }
}

void CCD3D12Device::advanceBufferStateEpoch() {
    if (_impl) {
        ++_impl->bufferStateEpoch;
        activeTransientUniformFrameState.epoch = _impl->bufferStateEpoch;
    }
}

D3D12UploadAllocation CCD3D12Device::getOrCreateTransientUniformSlot(uint32_t &slotIndex) {
    D3D12UploadAllocation allocation;
    if (!_impl || !_impl->d3dDevice) {
        return allocation;
    }

    constexpr uint32_t INVALID_SLOT = std::numeric_limits<uint32_t>::max();
    if (slotIndex == INVALID_SLOT &&
        _impl->nextTransientUniformSlotIndex >= TRANSIENT_UNIFORM_SLOT_COUNT) {
        return allocation;
    }

    auto &arena = _impl->frameResources[_impl->activeFrameResource].transientUniformSlotArena;
    if (!arena.resource) {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProperties.CreationNodeMask = 1;
        heapProperties.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = static_cast<uint64_t>(TRANSIENT_UNIFORM_SLOT_COUNT) *
                             D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = _impl->d3dDevice->CreateCommittedResource(
            &heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&arena.resource));
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12 transient uniform slot arena creation failed. HRESULT=0x%08x",
                         static_cast<unsigned>(hr));
            return allocation;
        }

        D3D12_RANGE readRange{};
        void *mappedData = nullptr;
        hr = arena.resource->Map(0, &readRange, &mappedData);
        if (FAILED(hr) || !mappedData) {
            CC_LOG_ERROR("D3D12 transient uniform slot arena Map failed. HRESULT=0x%08x",
                         static_cast<unsigned>(hr));
            arena.resource.Reset();
            return allocation;
        }
        arena.mappedData = static_cast<uint8_t *>(mappedData);
        arena.size = resourceDesc.Width;
    }

    if (slotIndex == INVALID_SLOT) {
        slotIndex = _impl->nextTransientUniformSlotIndex++;
    }
    if (slotIndex >= TRANSIENT_UNIFORM_SLOT_COUNT) {
        return allocation;
    }

    activeTransientUniformFrameState.resource = arena.resource.Get();
    activeTransientUniformFrameState.mappedData = arena.mappedData;
    activeTransientUniformFrameState.gpuAddress = arena.resource->GetGPUVirtualAddress();
    activeTransientUniformFrameState.epoch = _impl->bufferStateEpoch;
    activeTransientUniformFrameState.isValid = true;

    const uint64_t offset = static_cast<uint64_t>(slotIndex) *
                            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    allocation.resource = arena.resource.Get();
    allocation.mappedData = arena.mappedData + offset;
    allocation.offset = offset;
    allocation.gpuAddress = arena.resource->GetGPUVirtualAddress() + offset;
    allocation.size = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    allocation.isValid = true;
    return allocation;
}

D3D12UploadAllocation CCD3D12Device::allocateUploadBuffer(uint64_t size, uint64_t alignment) {
    D3D12UploadAllocation allocation;
    if (!_impl || !_impl->d3dDevice || size == 0) {
        return allocation;
    }

    alignment = std::max<uint64_t>(alignment, 1);
    auto alignUp = [](uint64_t value, uint64_t align) {
        return ((value + align - 1) / align) * align;
    };

    constexpr uint64_t DEFAULT_UPLOAD_PAGE_SIZE = 1024ULL * 1024ULL;
    const uint64_t requiredSize = alignUp(size, alignment);

    auto &uploadPages = _impl->frameResources[_impl->activeFrameResource].uploadPages;
    for (auto &page : uploadPages) {
        const uint64_t alignedOffset = alignUp(page.offset, alignment);
        if (alignedOffset + requiredSize <= page.size) {
            page.offset = alignedOffset + requiredSize;
            allocation.resource = page.resource.Get();
            allocation.mappedData = page.mappedData + alignedOffset;
            allocation.offset = alignedOffset;
            allocation.gpuAddress = page.resource->GetGPUVirtualAddress() + alignedOffset;
            allocation.size = requiredSize;
            allocation.isValid = true;
            return allocation;
        }
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = std::max<uint64_t>(DEFAULT_UPLOAD_PAGE_SIZE, requiredSize);
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Impl::UploadPage page;
    page.size = resourceDesc.Width;
    HRESULT hr = _impl->d3dDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&page.resource));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 upload page creation failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return allocation;
    }

    D3D12_RANGE readRange{};
    void *mappedData = nullptr;
    hr = page.resource->Map(0, &readRange, &mappedData);
    if (FAILED(hr) || !mappedData) {
        CC_LOG_ERROR("D3D12 upload page Map failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return allocation;
    }
    page.mappedData = static_cast<uint8_t *>(mappedData);
    page.offset = requiredSize;

    allocation.resource = page.resource.Get();
    allocation.mappedData = page.mappedData;
    allocation.offset = 0;
    allocation.gpuAddress = page.resource->GetGPUVirtualAddress();
    allocation.size = requiredSize;
    allocation.isValid = true;

    uploadPages.emplace_back(std::move(page));
    return allocation;
}

void CCD3D12Device::enqueueBufferUpdate(CCD3D12Buffer *buffer) {
    if (!_impl || !buffer) {
        return;
    }
    _impl->pendingBufferUpdates.emplace_back(buffer);
}

void CCD3D12Device::discardPendingBufferUpdate(CCD3D12Buffer *buffer) {
    if (!_impl || !buffer) {
        return;
    }
    if (!_impl->pendingBufferUpdates.empty()) {
        _impl->pendingBufferUpdates.erase(
            std::remove(_impl->pendingBufferUpdates.begin(), _impl->pendingBufferUpdates.end(), buffer),
            _impl->pendingBufferUpdates.end());
    }
}

void CCD3D12Device::flushPendingBufferUpdates(CCD3D12CommandBuffer *commandBuffer) {
    if (!_impl || !commandBuffer || _impl->pendingBufferUpdates.empty()) {
        return;
    }

    // Move the queue out first so an update produced while draining is kept
    // for the next safe point instead of being invalidated by vector growth.
    ccstd::vector<CCD3D12Buffer *> pendingUpdates;
    pendingUpdates.swap(_impl->pendingBufferUpdates);

    uint64_t transientUniformBytes = 0;
    for (size_t i = 0; i < pendingUpdates.size(); ++i) {
        auto *buffer = pendingUpdates[i];
        transientUniformBytes += buffer ? buffer->getPendingTransientUniformUploadSize() : 0;
    }
    const auto transientUniformAllocation = transientUniformBytes > 0
                                                ? allocateUploadBuffer(
                                                      transientUniformBytes,
                                                      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)
                                                : D3D12UploadAllocation{};
    uint64_t transientUniformOffset = 0;
    // updateQueued admits each Buffer actor only once. Non-view buffers own
    // distinct D3D12 resources, so this batch cannot contain aliased targets.
    const bool destinationsAreUnique =
        commandBuffer->getType() == CommandBufferType::PRIMARY &&
        std::all_of(
            pendingUpdates.begin(), pendingUpdates.end(),
            [](const CCD3D12Buffer *buffer) {
                return buffer && !buffer->isBufferView();
            });
    commandBuffer->startBufferUpdateBatch(destinationsAreUnique);
    for (size_t i = 0; i < pendingUpdates.size(); ++i) {
        auto *buffer = pendingUpdates[i];
        if (buffer) {
            const uint32_t transientSize = buffer->getPendingTransientUniformUploadSize();
            if (transientSize > 0 && transientUniformAllocation.isValid &&
                buffer->flushTransientUniformUpload(
                    transientUniformAllocation.resource,
                    static_cast<uint8_t *>(transientUniformAllocation.mappedData) + transientUniformOffset,
                    transientUniformAllocation.gpuAddress + transientUniformOffset,
                    _impl->bufferStateEpoch)) {
                transientUniformOffset += transientSize;
                continue;
            }
            buffer->flushPendingUpdate(commandBuffer);
        }
    }
    if (transientUniformOffset > 0) {
        notifyTransientUniformUpload();
    }
    commandBuffer->finishBufferUpdateBatch();

}

void CCD3D12Device::notifySubmittedFence(void *fence, uint64_t value) {
    if (!_impl || !fence) {
        return;
    }
    auto *d3d12Fence = static_cast<ID3D12Fence *>(fence);
    _impl->lastSubmittedFence = d3d12Fence;
    _impl->lastSubmittedFenceValue = value;
    auto &frameResources = _impl->frameResources[_impl->activeFrameResource];
    frameResources.fence = d3d12Fence;
    frameResources.fenceValue = value;
}

void CCD3D12Device::waitForSubmittedFence() {
    if (!_impl || !_impl->lastSubmittedFence || _impl->lastSubmittedFenceValue == 0 || !_impl->fenceEvent) {
        return;
    }

    if (_impl->lastSubmittedFence->GetCompletedValue() >= _impl->lastSubmittedFenceValue) {
        return;
    }

    const HRESULT hr = _impl->lastSubmittedFence->SetEventOnCompletion(
        _impl->lastSubmittedFenceValue, _impl->fenceEvent);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 device failed to wait for submitted frame. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    WaitForSingleObject(_impl->fenceEvent, INFINITE);
}

void CCD3D12Device::retireFrameResources() {
    if (!_impl) {
        return;
    }

    if (_impl->frameFence) {
        const uint64_t completedUploadFence = _impl->frameFence->GetCompletedValue();
        _impl->pendingUploadCommandContexts.erase(
            std::remove_if(_impl->pendingUploadCommandContexts.begin(),
                           _impl->pendingUploadCommandContexts.end(),
                           [completedUploadFence](const Impl::PendingUploadCommandContext &context) {
                               return context.fenceValue <= completedUploadFence;
                           }),
            _impl->pendingUploadCommandContexts.end());
    }

    // Descriptor ranges and upload pages are reset by acquire() only after
    // the fence of their owning frame slot has completed. Async uploads above
    // are retained independently by their own device fence.
}

void CCD3D12Device::unregisterSwapchain(CCD3D12Swapchain *swapchain) {
    if (!swapchain) {
        return;
    }
    _d3d12Swapchains.erase(
        std::remove(_d3d12Swapchains.begin(), _d3d12Swapchains.end(), swapchain),
        _d3d12Swapchains.end());
}

bool CCD3D12Device::isSwapchainBackBuffer(void *resource) const {
    if (!resource) {
        return false;
    }

    for (auto *swapchain : _d3d12Swapchains) {
        if (swapchain && swapchain->containsBackBuffer(resource)) {
            return true;
        }
    }
    return false;
}

void *CCD3D12Device::getDrawIndirectSignature() const {
    return _impl ? _impl->drawIndirectSig.Get() : nullptr;
}

void *CCD3D12Device::getDrawIndexedIndirectSignature() const {
    return _impl ? _impl->drawIndexedIndirectSig.Get() : nullptr;
}

void *CCD3D12Device::getOrCreateLocalRootCbvIndirectSignature(void *rootSignature,
                                                               const uint32_t *rootParameterIndices,
                                                               uint32_t rootParameterCount,
                                                               bool indexed,
                                                               uint32_t byteStride) {
    if (!_impl || !_impl->d3dDevice || !rootSignature || !rootParameterIndices ||
        rootParameterCount == 0 || rootParameterCount > D3D12_MAX_LOCAL_ROOT_CBVS ||
        byteStride == 0) {
        return nullptr;
    }

    auto *d3dRootSignature = static_cast<ID3D12RootSignature *>(rootSignature);
    for (const auto &entry : _impl->localRootCbvIndirectSignatures) {
        if (entry.rootSignature.Get() == d3dRootSignature &&
            entry.rootParameterCount == rootParameterCount && entry.byteStride == byteStride &&
            entry.indexed == indexed &&
            std::memcmp(entry.rootParameterIndices, rootParameterIndices,
                        rootParameterCount * sizeof(uint32_t)) == 0) {
            return entry.signature.Get();
        }
    }

    D3D12_INDIRECT_ARGUMENT_DESC arguments[D3D12_MAX_LOCAL_ROOT_CBVS + 1]{};
    for (uint32_t i = 0; i < rootParameterCount; ++i) {
        arguments[i].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
        arguments[i].ConstantBufferView.RootParameterIndex = rootParameterIndices[i];
    }
    arguments[rootParameterCount].Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED
                                                 : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
    signatureDesc.pArgumentDescs = arguments;
    signatureDesc.NumArgumentDescs = rootParameterCount + 1;
    signatureDesc.ByteStride = byteStride;

    Impl::LocalRootCbvIndirectSignature entry;
    entry.rootSignature = d3dRootSignature;
    std::memcpy(entry.rootParameterIndices, rootParameterIndices,
                rootParameterCount * sizeof(uint32_t));
    entry.rootParameterCount = rootParameterCount;
    entry.byteStride = byteStride;
    entry.indexed = indexed;
    const HRESULT hr = _impl->d3dDevice->CreateCommandSignature(
        &signatureDesc, d3dRootSignature, IID_PPV_ARGS(&entry.signature));
    if (FAILED(hr)) {
        CC_LOG_WARNING("D3D12: CreateCommandSignature for local Root-CBV batching failed "
                       "(roots=%u, indexed=%s, stride=%u, HRESULT=0x%08x).",
                       rootParameterCount, indexed ? "true" : "false", byteStride,
                       static_cast<unsigned>(hr));
        return nullptr;
    }

    _impl->localRootCbvIndirectSignatures.emplace_back(std::move(entry));
    return _impl->localRootCbvIndirectSignatures.back().signature.Get();
}

void *CCD3D12Device::getDispatchIndirectSignature() const {
    return _impl ? _impl->dispatchIndirectSig.Get() : nullptr;
}

void CCD3D12Device::present() {
    if (!_impl->graphicsQueue || !_impl->frameFence || !_impl->fenceEvent) {
        return;
    }

    flushDeferredCubeUploads();

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
    }

    // Transient descriptor/upload pages are reclaimed lazily in acquire()
    // after the queue fence proves the previous submission has completed.
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

void CCD3D12Device::flushDeferredCubeUploads() {
    if (!_impl || _impl->deferredCubeUploads.empty()) {
        return;
    }

    auto pendingUploads = std::move(_impl->deferredCubeUploads);
    _impl->deferredCubeUploads.clear();

    for (auto &pending : pendingUploads) {
        if (!pending.texture || pending.receivedCount == 0) {
            continue;
        }

        ccstd::vector<const uint8_t *> buffers;
        ccstd::vector<BufferTextureCopy> regions;
        buffers.reserve(pending.receivedCount);
        regions.reserve(pending.receivedCount);
        for (size_t layer = 0; layer < pending.layerData.size(); ++layer) {
            if (pending.layerData[layer].empty()) {
                continue;
            }
            buffers.push_back(pending.layerData[layer].data());
            regions.push_back(pending.layerRegions[layer]);
        }
        if (buffers.empty()) {
            continue;
        }

#ifndef NDEBUG
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-MIP-DIAG] flush deferred cube upload resource=%p regions=%u complete=%s",
                    pending.resource,
                    static_cast<uint32_t>(regions.size()),
                    pending.receivedCount == pending.layerData.size() ? "true" : "false");
#endif
        copyBuffersToTextureImmediate(buffers.data(), pending.texture, regions.data(), static_cast<uint32_t>(regions.size()));
    }
}

void CCD3D12Device::flushDeferredCubeUploadsForTexture(Texture *texture) {
    if (!_impl || !texture || _impl->deferredCubeUploads.empty()) {
        return;
    }

    auto *d3d12Texture = static_cast<CCD3D12Texture *>(texture);
    auto *textureResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
    ccstd::vector<Impl::DeferredCubeUpload> retainedUploads;
    ccstd::vector<Impl::DeferredCubeUpload> matchingUploads;
    retainedUploads.reserve(_impl->deferredCubeUploads.size());
    matchingUploads.reserve(_impl->deferredCubeUploads.size());

    for (auto &pending : _impl->deferredCubeUploads) {
        if (pending.texture == texture || (textureResource && pending.resource == textureResource)) {
            matchingUploads.emplace_back(std::move(pending));
        } else {
            retainedUploads.emplace_back(std::move(pending));
        }
    }

    if (matchingUploads.empty()) {
        _impl->deferredCubeUploads = std::move(retainedUploads);
        return;
    }

    _impl->deferredCubeUploads = std::move(matchingUploads);
    flushDeferredCubeUploads();
    _impl->deferredCubeUploads = std::move(retainedUploads);
}

void CCD3D12Device::discardDeferredCubeUploadsForTexture(Texture *texture) {
    if (!_impl || !texture || _impl->deferredCubeUploads.empty()) {
        return;
    }

    const auto beforeCount = _impl->deferredCubeUploads.size();
    _impl->deferredCubeUploads.erase(
        std::remove_if(_impl->deferredCubeUploads.begin(), _impl->deferredCubeUploads.end(),
                       [texture](const Impl::DeferredCubeUpload &pending) {
                           return pending.texture == texture;
                       }),
        _impl->deferredCubeUploads.end());
#ifndef NDEBUG
    if (_impl->deferredCubeUploads.size() != beforeCount) {
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-MIP-DIAG] discard deferred cube upload texture=%p removed=%zu",
                    texture,
                    beforeCount - _impl->deferredCubeUploads.size());
    }
#endif
}

bool CCD3D12Device::tryDeferCubeFaceUpload(const uint8_t *const *buffers, Texture *dst, const BufferTextureCopy *regions, uint32_t count) {
    if (!_impl || !buffers || !buffers[0] || !dst || !regions || count != 1) {
        return false;
    }

    const auto &textureInfo = dst->getInfo();
    const auto &region = regions[0];
    if (textureInfo.type != TextureType::CUBE ||
        !hasFlag(textureInfo.flags, TextureFlagBit::GEN_MIPMAP) ||
        textureInfo.levelCount <= 1 ||
        textureInfo.layerCount == 0 ||
        textureInfo.layerCount > 32 ||
        region.texSubres.mipLevel != 0 ||
        region.texSubres.layerCount > 1 ||
        region.texSubres.baseArrayLayer >= textureInfo.layerCount ||
        region.texOffset.x != 0 ||
        region.texOffset.y != 0 ||
        region.texOffset.z != 0 ||
        region.texExtent.width != textureInfo.width ||
        region.texExtent.height != textureInfo.height ||
        std::max<uint32_t>(region.texExtent.depth, 1) != 1) {
        return false;
    }

    const uint32_t sourceRowTexels = region.buffStride > 0 ? region.buffStride : region.texExtent.width;
    const uint32_t sourceRows = region.buffTexHeight > 0 ? region.buffTexHeight : region.texExtent.height;
    const uint32_t sourceBytes = formatSize(textureInfo.format, sourceRowTexels, sourceRows, 1);
    if (sourceBytes == 0) {
        return false;
    }

    auto *d3d12Texture = static_cast<CCD3D12Texture *>(dst);
    auto *textureResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
    if (!textureResource) {
        return false;
    }

    auto found = std::find_if(_impl->deferredCubeUploads.begin(), _impl->deferredCubeUploads.end(),
                              [textureResource](const Impl::DeferredCubeUpload &pending) {
                                  return pending.resource == textureResource;
                              });
    if (found == _impl->deferredCubeUploads.end()) {
        Impl::DeferredCubeUpload pending;
        pending.texture = dst;
        pending.resource = textureResource;
        pending.layerData.resize(textureInfo.layerCount);
        pending.layerRegions.resize(textureInfo.layerCount);
        _impl->deferredCubeUploads.emplace_back(std::move(pending));
        found = std::prev(_impl->deferredCubeUploads.end());
    }

    const uint32_t layer = region.texSubres.baseArrayLayer;
    const uint32_t layerBit = 1U << layer;
    if ((found->receivedMask & layerBit) != 0) {
        flushDeferredCubeUploadsForTexture(dst);
        return false;
    }

    auto storedRegion = region;
    storedRegion.buffOffset = 0;
    found->layerData[layer].resize(sourceBytes);
    std::memcpy(found->layerData[layer].data(), buffers[0] + region.buffOffset, sourceBytes);
    found->layerRegions[layer] = storedRegion;
    found->receivedMask |= layerBit;
    ++found->receivedCount;

    if (found->receivedCount < textureInfo.layerCount) {
        return true;
    }

#ifndef NDEBUG
    CC_D3D12_DIAGNOSTIC_LOG("[D3D12-MIP-DIAG] defer complete cube upload resource=%p size=%ux%u layers=%u regions=%u",
                textureResource,
                textureInfo.width,
                textureInfo.height,
                textureInfo.layerCount,
                found->receivedCount);
#endif
    return true;
}

void CCD3D12Device::copyBuffersToTexture(const uint8_t *const *buffers, Texture *dst, const BufferTextureCopy *regions, uint32_t count) {
    if (tryDeferCubeFaceUpload(buffers, dst, regions, count)) {
        return;
    }

    flushDeferredCubeUploadsForTexture(dst);
    copyBuffersToTextureImmediate(buffers, dst, regions, count);
}

void CCD3D12Device::copyBuffersToTextureImmediate(const uint8_t *const *buffers, Texture *dst, const BufferTextureCopy *regions, uint32_t count) {
    if (!buffers || !dst || !regions || count == 0 || !_impl->d3dDevice || !_impl->graphicsQueue || !_impl->frameFence) {
        return;
    }

    auto *d3d12Texture = static_cast<CCD3D12Texture *>(dst);
    auto *textureResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
    const auto backing = d3d12Texture->getD3D12ResourceBacking();
    if (!textureResource || !backing) {
        return;
    }

    const auto &textureInfo = dst->getInfo();
    const auto &textureView = dst->getViewInfo();
    const uint32_t baseMip = dst->isTextureView() ? textureView.baseLevel : 0;
    const uint32_t baseLayer = dst->isTextureView() ? textureView.baseLayer : 0;
    if (formatSize(textureInfo.format, 1, 1, 1) == 0) {
        CC_LOG_WARNING("D3D12 texture upload skipped for unsupported texel size.");
        return;
    }
#ifndef NDEBUG
    const bool diagnoseMipUpload =
        hasFlag(textureInfo.flags, TextureFlagBit::GEN_MIPMAP) && textureInfo.levelCount > 1;
#else
    constexpr bool diagnoseMipUpload = false;
#endif
    if (diagnoseMipUpload) {
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-MIP-DIAG] device upload resource=%p size=%ux%u levels=%u layers=%u format=%u regions=%u",
                    textureResource, textureInfo.width, textureInfo.height, textureInfo.levelCount,
                    textureInfo.layerCount, static_cast<unsigned>(textureInfo.format), count);
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> uploadCommandAllocator;
    HRESULT hr = _impl->d3dDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&uploadCommandAllocator));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 upload command allocator creation failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> uploadCommandList;
    hr = _impl->d3dDevice->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        uploadCommandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&uploadCommandList));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 upload command list creation failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    ccstd::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploadSourceResources;
    std::unordered_set<ID3D12Resource *> retainedUploadResources;
    ccstd::vector<D3D12_RESOURCE_STATES> previousStates(backing->subresourceCount());
    ccstd::vector<D3D12_RESOURCE_BARRIER> stateBarriers;
    stateBarriers.reserve(backing->subresourceCount());
    for (uint32_t subresource = 0; subresource < backing->subresourceCount(); ++subresource) {
        previousStates[subresource] = backing->states.get(subresource);
        D3D12_RESOURCE_BARRIER barrier{};
        if (makeD3D12TransitionBarrier(
                *backing, textureResource, subresource,
                D3D12_RESOURCE_STATE_COPY_DEST, barrier)) {
            stateBarriers.push_back(barrier);
        }
    }
    if (!stateBarriers.empty()) {
        uploadCommandList->ResourceBarrier(
            static_cast<UINT>(stateBarriers.size()), stateBarriers.data());
    }

    for (uint32_t regionIndex = 0; regionIndex < count; ++regionIndex) {
        if (!buffers[regionIndex]) {
            continue;
        }

        const auto &region = regions[regionIndex];
        const uint32_t mipLevel = baseMip + region.texSubres.mipLevel;
        const uint32_t arrayLayer = textureInfo.type == TextureType::TEX3D
                                        ? 0
                                        : baseLayer + region.texSubres.baseArrayLayer;
        const uint32_t subresource = backing->subresourceIndex(mipLevel, arrayLayer, 0);
        if (diagnoseMipUpload) {
            CC_D3D12_DIAGNOSTIC_LOG("[D3D12-MIP-DIAG] region=%u mip=%u layer=%u extent=%ux%ux%u offset=%d,%d,%d",
                        regionIndex, mipLevel, arrayLayer,
                        region.texExtent.width, region.texExtent.height, region.texExtent.depth,
                        region.texOffset.x, region.texOffset.y, region.texOffset.z);
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 uploadSize = 0;
        D3D12_RESOURCE_DESC textureDesc = textureResource->GetDesc();
        if (!getD3D12TextureUploadFootprint(
                _impl->d3dDevice.Get(), textureDesc, region,
                footprint, rowCount, rowSizeInBytes, uploadSize)) {
            continue;
        }

        auto upload = allocateUploadBuffer(uploadSize + footprint.Offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!upload.isValid || !upload.mappedData || !upload.resource) {
            CC_LOG_ERROR("D3D12 texture upload allocation failed. size=%llu",
                         static_cast<unsigned long long>(uploadSize + footprint.Offset));
            continue;
        }
        auto *uploadResource = static_cast<ID3D12Resource *>(upload.resource);
        if (retainedUploadResources.emplace(uploadResource).second) {
            uploadSourceResources.emplace_back(uploadResource);
        }

        const uint32_t sourceRowTexels = region.buffStride > 0 ? region.buffStride : region.texExtent.width;
        const uint32_t sourceRows = region.buffTexHeight > 0 ? region.buffTexHeight : region.texExtent.height;
        const uint32_t sourceRowPitch = formatSize(textureInfo.format, sourceRowTexels, 1, 1);
        const uint32_t sourceSlicePitch = formatSize(textureInfo.format, sourceRowTexels, sourceRows, 1);
        const uint32_t copyRowBytes = formatSize(textureInfo.format, region.texExtent.width, 1, 1);
        const auto blockAlignment = formatAlignment(textureInfo.format);
        const uint32_t blockHeight = std::max<uint32_t>(blockAlignment.second, 1);
        const uint32_t copyRows = std::min<uint32_t>(
            (region.texExtent.height + blockHeight - 1) / blockHeight, rowCount);
        const uint32_t copyDepth = std::max<uint32_t>(region.texExtent.depth, 1);
        const auto *src = buffers[regionIndex] + region.buffOffset;
        auto *dstBytes = static_cast<uint8_t *>(upload.mappedData) + footprint.Offset;

        for (uint32_t z = 0; z < copyDepth; ++z) {
            for (uint32_t row = 0; row < copyRows; ++row) {
                const uint8_t *srcRow = src + z * sourceSlicePitch + row * sourceRowPitch;
                uint8_t *dstRow = dstBytes + z * footprint.Footprint.RowPitch * rowCount + row * footprint.Footprint.RowPitch;
                std::memcpy(dstRow, srcRow, std::min<uint32_t>(copyRowBytes, static_cast<uint32_t>(rowSizeInBytes)));
            }
        }

        D3D12_TEXTURE_COPY_LOCATION srcLocation{};
        srcLocation.pResource = static_cast<ID3D12Resource *>(upload.resource);
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLocation.PlacedFootprint = footprint;
        srcLocation.PlacedFootprint.Offset = upload.offset + footprint.Offset;

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

        uploadCommandList->CopyTextureRegion(
            &dstLocation,
            region.texOffset.x,
            region.texOffset.y,
            region.texOffset.z,
            &srcLocation,
            &srcBox);

        d3d12Texture->markBaseMipLayerUploaded(
            region.texSubres.mipLevel,
            region.texSubres.baseArrayLayer,
            textureInfo.type == TextureType::TEX3D ? 1 : region.texSubres.layerCount);
    }

    ccstd::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> mipDescriptorHeaps;
    const bool shouldGenerateMipmaps = d3d12Texture->shouldGenerateMipmapsAfterUpload();
    const bool generatedMipmaps =
        !dst->isTextureView() && shouldGenerateMipmaps &&
        generateD3D12Mipmaps(_impl->d3dDevice.Get(), uploadCommandList.Get(), textureResource, backing,
                             textureInfo, mipDescriptorHeaps);
    if (generatedMipmaps) {
        d3d12Texture->markMipmapsGenerated();
    }
    if (diagnoseMipUpload) {
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-MIP-DIAG] generation resource=%p result=%s descriptorHeaps=%zu",
                    textureResource,
                    shouldGenerateMipmaps ? (generatedMipmaps ? "success" : "failed") : "deferred",
                    mipDescriptorHeaps.size());
    }
    stateBarriers.clear();
    for (uint32_t subresource = 0; subresource < backing->subresourceCount(); ++subresource) {
        D3D12_RESOURCE_BARRIER barrier{};
        if (makeD3D12TransitionBarrier(
                *backing, textureResource, subresource,
                previousStates[subresource], barrier)) {
            stateBarriers.push_back(barrier);
        }
    }
    if (!stateBarriers.empty()) {
        uploadCommandList->ResourceBarrier(
            static_cast<UINT>(stateBarriers.size()), stateBarriers.data());
    }

    hr = uploadCommandList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 upload command list close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    ID3D12CommandList *commandLists[] = {uploadCommandList.Get()};
    _impl->graphicsQueue->ExecuteCommandLists(1, commandLists);

    Impl::PendingUploadCommandContext pendingContext;
    pendingContext.commandAllocator = std::move(uploadCommandAllocator);
    pendingContext.commandList = std::move(uploadCommandList);
    pendingContext.referencedResources.emplace_back(textureResource);
    pendingContext.referencedResources.insert(
        pendingContext.referencedResources.end(),
        std::make_move_iterator(uploadSourceResources.begin()),
        std::make_move_iterator(uploadSourceResources.end()));
    pendingContext.descriptorHeaps = std::move(mipDescriptorHeaps);

    ++_impl->fenceValue;
    hr = _impl->graphicsQueue->Signal(_impl->frameFence.Get(), _impl->fenceValue);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 upload fence signal failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        if (!waitIdle()) {
            pendingContext.fenceValue = std::numeric_limits<uint64_t>::max();
            auto &frameResources = _impl->frameResources[_impl->activeFrameResource];
            frameResources.fence = _impl->frameFence;
            frameResources.fenceValue = pendingContext.fenceValue;
            _impl->pendingUploadCommandContexts.emplace_back(std::move(pendingContext));
        }
        return;
    }

    pendingContext.fenceValue = _impl->fenceValue;
    auto &frameResources = _impl->frameResources[_impl->activeFrameResource];
    frameResources.fence = _impl->frameFence;
    frameResources.fenceValue = _impl->fenceValue;
    _impl->pendingUploadCommandContexts.emplace_back(std::move(pendingContext));
    if (diagnoseMipUpload) {
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-MIP-DIAG] upload submitted async resource=%p fence=%llu pendingContexts=%zu",
                    textureResource,
                    static_cast<unsigned long long>(_impl->fenceValue),
                    _impl->pendingUploadCommandContexts.size());
    }
}

void CCD3D12Device::copyTextureToBuffers(Texture *src, uint8_t *const *buffers, const BufferTextureCopy *regions, uint32_t count) {
    if (!src || !buffers || !regions || count == 0 || !_impl->d3dDevice || !_impl->graphicsQueue || !_impl->commandAllocator || !_impl->commandList) {
        return;
    }

    auto *d3d12Texture = static_cast<CCD3D12Texture *>(src);
    auto *textureResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
    const auto backing = d3d12Texture->getD3D12ResourceBacking();
    if (!textureResource || !backing) {
        return;
    }

    const auto &textureInfo = src->getInfo();
    const auto &textureView = src->getViewInfo();
    const uint32_t baseMip = src->isTextureView() ? textureView.baseLevel : 0;
    const uint32_t baseLayer = src->isTextureView() ? textureView.baseLayer : 0;
    if (textureInfo.samples != SampleCount::X1) {
        CC_LOG_WARNING("D3D12 texture readback skipped for multisampled texture. Resolve before readback.");
        return;
    }

    if (!waitForGpu()) {
        return;
    }

    HRESULT hr = _impl->commandAllocator->Reset();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 readback command allocator reset failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    hr = _impl->commandList->Reset(_impl->commandAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 readback command list reset failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    ccstd::vector<D3D12_RESOURCE_STATES> previousStates(backing->subresourceCount());
    ccstd::vector<D3D12_RESOURCE_BARRIER> stateBarriers;
    stateBarriers.reserve(backing->subresourceCount());
    for (uint32_t subresource = 0; subresource < backing->subresourceCount(); ++subresource) {
        previousStates[subresource] = backing->states.get(subresource);
        D3D12_RESOURCE_BARRIER barrier{};
        if (makeD3D12TransitionBarrier(
                *backing, textureResource, subresource,
                D3D12_RESOURCE_STATE_COPY_SOURCE, barrier)) {
            stateBarriers.push_back(barrier);
        }
    }
    if (!stateBarriers.empty()) {
        _impl->commandList->ResourceBarrier(
            static_cast<UINT>(stateBarriers.size()), stateBarriers.data());
    }

    struct ReadbackRegion {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        uint32_t regionIndex{0};
        UINT rowCount{0};
        UINT64 uploadSize{0};
    };
    ccstd::vector<ReadbackRegion> readbackRegions;
    readbackRegions.reserve(count);

    D3D12_RESOURCE_DESC textureDesc = textureResource->GetDesc();
    for (uint32_t regionIndex = 0; regionIndex < count; ++regionIndex) {
        if (!buffers[regionIndex]) {
            continue;
        }

        const auto &copyRegion = regions[regionIndex];
        const uint32_t mipLevel = baseMip + copyRegion.texSubres.mipLevel;
        const uint32_t arrayLayer = textureInfo.type == TextureType::TEX3D
                                        ? 0
                                        : baseLayer + copyRegion.texSubres.baseArrayLayer;
        const uint32_t subresource = backing->subresourceIndex(mipLevel, arrayLayer, 0);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 readbackSize = 0;
        _impl->d3dDevice->GetCopyableFootprints(&textureDesc, subresource, 1, 0, &footprint, &rowCount, &rowSizeInBytes, &readbackSize);
        if (readbackSize == 0 || rowCount == 0) {
            continue;
        }

        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
        heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProperties.CreationNodeMask = 1;
        heapProperties.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC readbackDesc{};
        readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readbackDesc.Alignment = 0;
        readbackDesc.Width = readbackSize;
        readbackDesc.Height = 1;
        readbackDesc.DepthOrArraySize = 1;
        readbackDesc.MipLevels = 1;
        readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
        readbackDesc.SampleDesc.Count = 1;
        readbackDesc.SampleDesc.Quality = 0;
        readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        readbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3D12Resource> readbackResource;
        hr = _impl->d3dDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&readbackResource));
        if (FAILED(hr)) {
            CC_LOG_ERROR("CreateCommittedResource(texture readback) failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            continue;
        }

        D3D12_TEXTURE_COPY_LOCATION srcLocation{};
        srcLocation.pResource = textureResource;
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLocation.SubresourceIndex = subresource;

        D3D12_TEXTURE_COPY_LOCATION dstLocation{};
        dstLocation.pResource = readbackResource.Get();
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLocation.PlacedFootprint = footprint;

        D3D12_BOX srcBox{};
        srcBox.left = static_cast<UINT>(copyRegion.texOffset.x);
        srcBox.top = static_cast<UINT>(copyRegion.texOffset.y);
        srcBox.front = static_cast<UINT>(copyRegion.texOffset.z);
        srcBox.right = srcBox.left + copyRegion.texExtent.width;
        srcBox.bottom = srcBox.top + copyRegion.texExtent.height;
        srcBox.back = srcBox.front + std::max<uint32_t>(copyRegion.texExtent.depth, 1);

        _impl->commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, &srcBox);

        ReadbackRegion readbackRegion;
        readbackRegion.resource = std::move(readbackResource);
        readbackRegion.footprint = footprint;
        readbackRegion.regionIndex = regionIndex;
        readbackRegion.rowCount = rowCount;
        readbackRegion.uploadSize = readbackSize;
        readbackRegions.push_back(std::move(readbackRegion));
    }

    stateBarriers.clear();
    for (uint32_t subresource = 0; subresource < backing->subresourceCount(); ++subresource) {
        D3D12_RESOURCE_BARRIER barrier{};
        if (makeD3D12TransitionBarrier(
                *backing, textureResource, subresource,
                previousStates[subresource], barrier)) {
            stateBarriers.push_back(barrier);
        }
    }
    if (!stateBarriers.empty()) {
        _impl->commandList->ResourceBarrier(
            static_cast<UINT>(stateBarriers.size()), stateBarriers.data());
    }

    hr = _impl->commandList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 readback command list close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    if (!readbackRegions.empty()) {
        ID3D12CommandList *commandLists[] = {_impl->commandList.Get()};
        _impl->graphicsQueue->ExecuteCommandLists(1, commandLists);
        if (!waitForGpu()) {
            return;
        }

        for (const auto &readbackRegion : readbackRegions) {
            void *mappedData = nullptr;
            D3D12_RANGE readRange{0, static_cast<SIZE_T>(readbackRegion.uploadSize)};
            hr = readbackRegion.resource->Map(0, &readRange, &mappedData);
            if (FAILED(hr) || !mappedData) {
                CC_LOG_ERROR("D3D12 texture readback Map failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
                continue;
            }

            const uint32_t regionIndex = readbackRegion.regionIndex;
            copyReadbackToBuffer(static_cast<const uint8_t *>(mappedData), readbackRegion.footprint, readbackRegion.rowCount,
                                 buffers[regionIndex], regions[regionIndex], textureInfo.format);
            D3D12_RANGE writtenRange{0, 0};
            readbackRegion.resource->Unmap(0, &writtenRange);
        }
    }
}

void CCD3D12Device::getQueryPoolResults(QueryPool *queryPool) {
    if (!queryPool) return;
    auto *d3d12Pool = static_cast<CCD3D12QueryPool *>(queryPool);
    d3d12Pool->fetchResults();
}

SampleCount CCD3D12Device::getMaxSampleCount(Format format, TextureUsage usage, TextureFlags flags) const {
    if (!_impl || !_impl->d3dDevice) return SampleCount::X1;

    const DXGI_FORMAT dxgiFormat = toD3D12Format(format);
    if (dxgiFormat == DXGI_FORMAT_UNKNOWN) return SampleCount::X1;

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
    if (!_impl || !_impl->d3dDevice) return;

    auto queryFormatSupport = [this](DXGI_FORMAT format) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
        support.Format = format;
        if (format == DXGI_FORMAT_UNKNOWN ||
            FAILED(_impl->d3dDevice->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) {
            support.Support1 = D3D12_FORMAT_SUPPORT1_NONE;
            support.Support2 = D3D12_FORMAT_SUPPORT2_NONE;
        }
        return support;
    };

    const auto formatCount = static_cast<uint32_t>(Format::COUNT);
    for (uint32_t i = toNumber(Format::R8); i < formatCount; ++i) {
        const auto format = static_cast<Format>(i);
        const DXGI_FORMAT dxgiFormat = toD3D12Format(format);
        if (dxgiFormat == DXGI_FORMAT_UNKNOWN) continue;

        const auto nativeSupport = queryFormatSupport(dxgiFormat);
        auto sampledSupport = nativeSupport;
        if (format == Format::DEPTH) {
            sampledSupport = queryFormatSupport(DXGI_FORMAT_R32_FLOAT);
        } else if (format == Format::DEPTH_STENCIL) {
            sampledSupport = queryFormatSupport(DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
        }

        const auto support1 = nativeSupport.Support1;
        if (support1 & (D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)) {
            _formatFeatures[i] |= FormatFeature::RENDER_TARGET;
        }
        if (sampledSupport.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) {
            _formatFeatures[i] |= FormatFeature::SAMPLED_TEXTURE;
            const auto type = GFX_FORMAT_INFOS[i].type;
            if (type != FormatType::UINT && type != FormatType::INT) {
                _formatFeatures[i] |= FormatFeature::LINEAR_FILTER;
            }
        }
        if (support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) {
            _formatFeatures[i] |= FormatFeature::STORAGE_TEXTURE;
        }
        if (support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER) {
            _formatFeatures[i] |= FormatFeature::VERTEX_ATTRIBUTE;
        }
    }
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

    // SDK Layers validate every D3D12 call and are prohibitively expensive in
    // draw-call-heavy scenes. Keep them explicitly opt-in for focused API
    // validation instead of coupling them to the application's Debug config.
    if (isD3D12DebugLayerRequested()) {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            CC_LOG_INFO("D3D12 debug layer enabled by CC_D3D12_DEBUG_LAYER.");
        } else {
            CC_LOG_WARNING("Could not enable D3D12 debug layer.");
        }
    } else {
        CC_D3D12_DIAGNOSTIC_LOG("D3D12 debug layer disabled; set CC_D3D12_DEBUG_LAYER=1 for API validation.");
    }

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
            CC_D3D12_DIAGNOSTIC_LOG("Adapter[%u]: %s (VRAM=%llu MB, Shared=%llu MB, VendorID=0x%04x, Discrete=%s)",
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

bool CCD3D12Device::waitForGpu() {
    return waitIdle();
}

bool CCD3D12Device::waitIdle() {
    if (!_impl->graphicsQueue || !_impl->frameFence || !_impl->fenceEvent) {
        CC_LOG_ERROR("D3D12 waitIdle failed: queue or fence synchronization object is unavailable.");
        return false;
    }

    auto logDeviceFailure = [this](const char *stage, HRESULT hr) {
        const HRESULT removedReason = _impl->d3dDevice
                                          ? _impl->d3dDevice->GetDeviceRemovedReason()
                                          : E_POINTER;
        CC_LOG_ERROR("D3D12 waitIdle %s failed. HRESULT=0x%08x DeviceRemovedReason=0x%08x",
                     stage, static_cast<unsigned>(hr), static_cast<unsigned>(removedReason));
    };

    ++_impl->fenceValue;
    HRESULT hr = _impl->graphicsQueue->Signal(_impl->frameFence.Get(), _impl->fenceValue);
    if (FAILED(hr)) {
        logDeviceFailure("signal", hr);
        return false;
    }

    constexpr UINT64 DEVICE_REMOVED_FENCE_VALUE = std::numeric_limits<UINT64>::max();
    UINT64 completedValue = _impl->frameFence->GetCompletedValue();
    if (completedValue == DEVICE_REMOVED_FENCE_VALUE) {
        logDeviceFailure("completion query", DXGI_ERROR_DEVICE_REMOVED);
        return false;
    }
    if (completedValue < _impl->fenceValue) {
        hr = _impl->frameFence->SetEventOnCompletion(_impl->fenceValue, _impl->fenceEvent);
        if (FAILED(hr)) {
            logDeviceFailure("SetEventOnCompletion", hr);
            return false;
        }
        const DWORD waitResult = WaitForSingleObject(_impl->fenceEvent, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            CC_LOG_ERROR("D3D12 waitIdle fence wait failed. result=%lu",
                         static_cast<unsigned long>(waitResult));
            logDeviceFailure(
                "fence wait",
                waitResult == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL);
            return false;
        }
        completedValue = _impl->frameFence->GetCompletedValue();
        if (completedValue == DEVICE_REMOVED_FENCE_VALUE ||
            completedValue < _impl->fenceValue) {
            logDeviceFailure("post-wait completion query",
                             completedValue == DEVICE_REMOVED_FENCE_VALUE
                                 ? DXGI_ERROR_DEVICE_REMOVED
                                 : E_FAIL);
            return false;
        }
    }
    if (_queue) {
        static_cast<CCD3D12Queue *>(_queue)->retireSubmittedContexts(true);
    }
    retireFrameResources();
    return true;
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

std::shared_ptr<void> CCD3D12Device::getBlitPipelineCacheEntry(uint32_t key) const {
    if (!_impl) {
        return {};
    }
    std::lock_guard<std::mutex> lock(_impl->nativePipelineCacheMutex);
    const auto iter = _impl->blitPipelineCache.find(key);
    return iter == _impl->blitPipelineCache.end() ? std::shared_ptr<void>{} : iter->second;
}

std::shared_ptr<void> CCD3D12Device::cacheBlitPipeline(uint32_t key, std::shared_ptr<void> pipeline) {
    if (!_impl || !pipeline) {
        return {};
    }
    std::lock_guard<std::mutex> lock(_impl->nativePipelineCacheMutex);
    const auto inserted = _impl->blitPipelineCache.emplace(key, std::move(pipeline));
    return inserted.first->second;
}

void *CCD3D12Device::getOrCreateEmptyRootSignature() {
    if (!_impl || !_impl->d3dDevice) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(_impl->nativePipelineCacheMutex);
    if (_impl->emptyRootSignature) {
        return _impl->emptyRootSignature.Get();
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 empty root signature serialization failed. HRESULT=0x%08x",
                     static_cast<unsigned>(hr));
        return nullptr;
    }

    hr = _impl->d3dDevice->CreateRootSignature(
        0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
        IID_PPV_ARGS(&_impl->emptyRootSignature));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 empty root signature creation failed. HRESULT=0x%08x",
                     static_cast<unsigned>(hr));
        return nullptr;
    }
    return _impl->emptyRootSignature.Get();
}

D3D12DescriptorHeapPool *CCD3D12Device::getGPUDescriptorHeapPool() const {
    return _impl ? _impl->gpuDescriptorHeapPool.get() : nullptr;
}

D3D12DescriptorHeapPool *CCD3D12Device::getSamplerDescriptorHeapPool() const {
    return _impl ? _impl->frameResources[_impl->activeFrameResource].samplerDescriptorHeapPool.get() : nullptr;
}

D3D12DescriptorHeapPool *CCD3D12Device::getCPUDescriptorHeapPool() const {
    return _impl ? _impl->cpuDescriptorHeapPool.get() : nullptr;
}

D3D12DescriptorHeapPool *CCD3D12Device::getCPUSamplerDescriptorHeapPool() const {
    return _impl ? _impl->cpuSamplerDescriptorHeapPool.get() : nullptr;
}

} // namespace gfx
} // namespace cc
