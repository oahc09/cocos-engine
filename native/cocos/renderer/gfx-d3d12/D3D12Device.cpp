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
#include <mutex>
#include <unordered_set>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace cc {
namespace gfx {

CCD3D12Device *CCD3D12Device::instance = nullptr;

namespace {
constexpr float D3D12_POC_CLEAR_COLOR[4] = {0.1F, 0.2F, 0.8F, 1.0F};

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

bool isD3D12PerfLoggingRequested() {
    // This is queried from several hot paths, so resolve it once per process.
    static const bool requested = isEnvironmentFlagEnabled("CC_D3D12_PERF_LOG");
    return requested;
}

#if defined(__ID3D12Device9_INTERFACE_DEFINED__) && defined(__ID3D12ShaderCacheSession_INTERFACE_DEFINED__)
constexpr GUID D3D12_COCOS_DXBC_SHADER_CACHE_GUID = {
    0x9f4e1b8d, 0x932a, 0x4a64, {0xa8, 0x4e, 0x31, 0x79, 0x50, 0x8f, 0xb8, 0xd2}};
constexpr uint64_t D3D12_COCOS_DXBC_SHADER_CACHE_VERSION = 1;
#endif

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
    static constexpr uint32_t MAX_FRAMES_IN_BATCH{2};
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
    uint64_t lastRetiredFenceValue{0};
    uint64_t bufferStateEpoch{1};
    uint64_t transientUniformUploadGeneration{1};
    uint32_t framesInCurrentBatch{0};

    // GPU-visible descriptor heap pools for shader access
    std::unique_ptr<D3D12DescriptorHeapPool> gpuDescriptorHeapPool;    // CBV_SRV_UAV, shaderVisible
    std::unique_ptr<D3D12DescriptorHeapPool> samplerDescriptorHeapPool; // SAMPLER, shaderVisible
    // Persistent CPU-only staging pools. Unlike GPU pools, these are not reset per frame.
    std::unique_ptr<D3D12DescriptorHeapPool> cpuDescriptorHeapPool;
    std::unique_ptr<D3D12DescriptorHeapPool> cpuSamplerDescriptorHeapPool;

    struct UploadPage {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint8_t *mappedData{nullptr};
        uint64_t size{0};
        uint64_t offset{0};
    };
    ccstd::vector<UploadPage> uploadPages;
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

#if defined(__ID3D12Device9_INTERFACE_DEFINED__) && defined(__ID3D12ShaderCacheSession_INTERFACE_DEFINED__)
    Microsoft::WRL::ComPtr<ID3D12ShaderCacheSession> shaderCacheSession;
    std::mutex shaderCacheMutex;
#endif

    struct FramePerfCounters {
        uint64_t descriptorFlushes{0};
        uint64_t copyDescriptorCalls{0};
        uint64_t copiedDescriptors{0};
        uint64_t dynamicOffsetRewrites{0};
        uint64_t dynamicOffsetDescriptors{0};
        uint64_t dynamicCbvTableDescriptors{0};
        uint64_t setDescriptorHeapCalls{0};
        uint64_t rootDescriptorTableBinds{0};
        uint64_t descriptorCacheHits{0};
        uint64_t descriptorCacheMisses{0};
        uint64_t descriptorRepackPasses{0};
        uint64_t descriptorRepackDescriptors{0};
        uint64_t descriptorCacheSlotLookups{0};
        uint64_t descriptorCacheSlotOwnerChanges{0};
        uint64_t descriptorFullFlushNs{0};
        uint64_t descriptorBindingBuildNs{0};
        uint64_t descriptorCacheProbeNs{0};
        uint64_t descriptorSetUpdateNs{0};
        uint64_t cbvCacheHits{0};
        uint64_t samplerCacheHits{0};
        uint64_t cbvCacheMisses{0};
        uint64_t samplerCacheMisses{0};
        uint64_t cbvCopiedDescriptors{0};
        uint64_t samplerCopiedDescriptors{0};
        uint64_t cbvRepackPasses{0};
        uint64_t samplerRepackPasses{0};
        uint64_t cbvHeapChanges{0};
        uint64_t samplerHeapChanges{0};
        uint64_t samplerTableLookups{0};
        uint64_t samplerUniqueTables{0};
        uint64_t samplerUniqueDescriptors{0};
        uint64_t samplerDuplicateTableHits{0};
        uint64_t samplerSignatureHashCollisions{0};
        uint64_t samplerConsecutiveExactHits{0};
        uint64_t resourceBarrierCalls{0};
        uint64_t resourceBarriers{0};
        uint64_t barrierTextureTransitions{0};
        uint64_t barrierBufferTransitions{0};
        uint64_t barrierUAV{0};
        uint64_t barrierTrackedAlreadyNext{0};
        uint64_t defaultBufferCopies{0};
        uint64_t defaultBufferBytes{0};
        uint64_t defaultUniformCopies{0};
        uint64_t defaultHostCopies{0};
        uint64_t defaultBufferViewCopies{0};
        uint64_t defaultDynamicOnlyCopies{0};
        uint64_t defaultRepeatedDestinations{0};
        uint64_t pendingBufferDrains{0};
        uint64_t pendingBuffersDrained{0};
        uint64_t maxPendingBufferBatch{0};
        uint64_t pendingBufferDrainNs{0};
        uint64_t pendingBufferEnqueues{0};
        uint64_t pendingBufferQueueGrowths{0};
        uint64_t pendingBufferQueueGrowthNs{0};
        uint64_t uniqueBatchRetainCalls{0};
        uint64_t uniqueBatchRetainInsertions{0};
        uint64_t uniqueBatchRetainNs{0};
        uint64_t uploadAllocateNs{0};
        uint64_t bufferBatchTotalNs{0};
        uint64_t bufferBatchBuildNs{0};
        uint64_t bufferBatchBarrierNs{0};
        uint64_t bufferBatchCopyNs{0};
        uint64_t bufferBatchStateNs{0};
        uint64_t bufferBatchFallbackNs{0};
        uint64_t descriptorFlushNs{0};
        uint64_t descriptorCopyNs{0};
        uint64_t defaultBufferUploadNs{0};
        uint64_t dynamicOffsetNs{0};
        uint64_t descriptorAllocateNs{0};
        uint64_t rootTableBindNs{0};
        uint64_t descriptorRangePrepareNs{0};
        uint64_t descriptorHeapNormalizeNs{0};
        uint64_t descriptorHeapRootNs{0};
        uint64_t descriptorCbvPrepareNs{0};
        uint64_t descriptorSamplerPrepareNs{0};
        uint64_t pipelineBindCalls{0};
        uint64_t pipelineSameLogical{0};
        uint64_t pipelineNativeChanges{0};
        uint64_t inputAssemblerBindCalls{0};
        uint64_t inputAssemblerSameLogical{0};
        uint64_t vertexBufferViewChanges{0};
        uint64_t indexBufferViewChanges{0};
        uint64_t pipelineBindNs{0};
        uint64_t inputAssemblerBindNs{0};
        uint64_t bindDescriptorSetCalls{0};
        uint64_t bindDescriptorSetNs{0};
        uint64_t drawCalls{0};
        uint64_t drawNs{0};
        uint64_t drawPendingBufferNs{0};
        uint64_t drawDescriptorFlushNs{0};
        uint64_t drawIssueNs{0};
        uint64_t commandReuseEvents{0};
        uint64_t commandReuseComparisons{0};
        uint64_t commandReuseExactMatches{0};
        uint32_t commandReuseFirstMismatch{std::numeric_limits<uint32_t>::max()};
        uint64_t commandBeginNs{0};
        uint64_t commandRecordingNs{0};
        uint64_t commandEndNs{0};
        uint64_t queueSubmitNs{0};
        uint64_t queueExecuteNs{0};
        uint64_t queueSignalNs{0};
        uint64_t acquireNs{0};
        uint64_t presentNs{0};
        uint64_t fenceWaits{0};
        uint64_t fenceWaitMicroseconds{0};
    };
    FramePerfCounters framePerfCounters;
    std::unordered_set<uint64_t> perfUpdatedBufferDestinations;
    uint64_t perfFrameIndex{0};
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
    initializeShaderCacheSession();

    // Initialize GPU-visible descriptor heap pools
    _impl->gpuDescriptorHeapPool = std::make_unique<D3D12DescriptorHeapPool>();
    _impl->gpuDescriptorHeapPool->initialize(
        // Keep one frame's descriptor tables in a single heap in normal scenes.
        // Switching shader-visible heaps invalidates every descriptor table and
        // defeats the command-buffer's per-set GPU descriptor cache.
        D3D12DescriptorHeapPool::HeapType::CBV_SRV_UAV, 65536, true);

    _impl->samplerDescriptorHeapPool = std::make_unique<D3D12DescriptorHeapPool>();
    _impl->samplerDescriptorHeapPool->initialize(
        D3D12DescriptorHeapPool::HeapType::SAMPLER, 2048, true);

    _impl->cpuDescriptorHeapPool = std::make_unique<D3D12DescriptorHeapPool>();
    _impl->cpuDescriptorHeapPool->initialize(
        D3D12DescriptorHeapPool::HeapType::CBV_SRV_UAV, 16384, false);

    _impl->cpuSamplerDescriptorHeapPool = std::make_unique<D3D12DescriptorHeapPool>();
    _impl->cpuSamplerDescriptorHeapPool->initialize(
        D3D12DescriptorHeapPool::HeapType::SAMPLER, 2048, false);

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
    drainD3D12ShaderCachePersistence();
    _d3d12Swapchains.clear();
    if (_impl) {
        _impl->deferredCubeUploads.clear();
    }
    waitForGpu();
    _impl->pendingBufferUpdates.clear();
    _impl->pendingUploadCommandContexts.clear();
    _impl->deferredCubeUploads.clear();

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
#if defined(__ID3D12Device9_INTERFACE_DEFINED__) && defined(__ID3D12ShaderCacheSession_INTERFACE_DEFINED__)
    {
        std::lock_guard<std::mutex> lock(_impl->shaderCacheMutex);
        _impl->shaderCacheSession.Reset();
    }
#endif
    _impl->d3dDevice.Reset();
    _impl->dxgiFactory.Reset();

    CC_SAFE_DESTROY_AND_DELETE(_cmdBuff);
    CC_SAFE_DESTROY_AND_DELETE(_queryPool);
    CC_SAFE_DESTROY_AND_DELETE(_queue);
}

void CCD3D12Device::initializeShaderCacheSession() {
#if defined(__ID3D12Device9_INTERFACE_DEFINED__) && defined(__ID3D12ShaderCacheSession_INTERFACE_DEFINED__)
    if (!_impl || !_impl->d3dDevice) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12Device9> device9;
    HRESULT hr = _impl->d3dDevice.As(&device9);
    if (FAILED(hr) || !device9) {
        CC_LOG_INFO("D3D12 Shader Cache Session unavailable: ID3D12Device9 not supported. HRESULT=0x%08x",
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
        CC_LOG_INFO("D3D12 Shader Cache Session initialized for DXBC cache.");
    } else {
        CC_LOG_WARNING("D3D12 Shader Cache Session initialization failed. HRESULT=0x%08x",
                       static_cast<unsigned>(hr));
    }
#else
    CC_LOG_INFO("D3D12 Shader Cache Session unavailable: SDK headers do not expose ID3D12Device9.");
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
#if CC_D3D12_PERF_COUNTERS
    const auto acquireStart = std::chrono::steady_clock::now();
#endif
    // The descriptor/upload pools are append-only within a bounded two-frame
    // batch. Reclaim the whole batch only after its latest fence completes, so
    // no descriptor or upload allocation referenced by the GPU is overwritten.
    if (_impl->framesInCurrentBatch >= Impl::MAX_FRAMES_IN_BATCH) {
        waitForSubmittedFence();
        retireFrameResources();
        _impl->framesInCurrentBatch = 0;
    }
    ++_impl->framesInCurrentBatch;

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
#if CC_D3D12_PERF_COUNTERS
    recordFramePhaseAnalysis(
        0, 0, 0, 0, 0, 0,
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - acquireStart).count()),
        0);
#endif
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

uint64_t CCD3D12Device::getTransientUniformUploadGeneration() const {
    return _impl ? _impl->transientUniformUploadGeneration : 0;
}

void CCD3D12Device::notifyTransientUniformUpload() {
    if (_impl) {
        ++_impl->transientUniformUploadGeneration;
    }
}

bool CCD3D12Device::isPerfLoggingEnabled() const {
    return isD3D12PerfLoggingRequested();
}

void CCD3D12Device::advanceBufferStateEpoch() {
    if (_impl) {
        ++_impl->bufferStateEpoch;
    }
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

    for (auto &page : _impl->uploadPages) {
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

    _impl->uploadPages.emplace_back(std::move(page));
    return allocation;
}

void CCD3D12Device::enqueueBufferUpdate(CCD3D12Buffer *buffer) {
    if (!_impl || !buffer) {
        return;
    }
#if CC_D3D12_PERF_COUNTERS
    const bool perfTimingEnabled = isD3D12PerfLoggingRequested();
    const bool queueWillGrow = _impl->pendingBufferUpdates.size() == _impl->pendingBufferUpdates.capacity();
    const auto growthStart = perfTimingEnabled && queueWillGrow
                                 ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
#endif
    _impl->pendingBufferUpdates.emplace_back(buffer);
#if CC_D3D12_PERF_COUNTERS
    if (perfTimingEnabled) {
        ++_impl->framePerfCounters.pendingBufferEnqueues;
        if (queueWillGrow) {
            ++_impl->framePerfCounters.pendingBufferQueueGrowths;
            _impl->framePerfCounters.pendingBufferQueueGrowthNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - growthStart).count());
        }
    }
#endif
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

#if CC_D3D12_PERF_COUNTERS
    const bool perfTimingEnabled = isD3D12PerfLoggingRequested();
    const auto drainStart = perfTimingEnabled
                                ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
#endif
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
#if CC_D3D12_PERF_COUNTERS
    if (isD3D12PerfLoggingRequested()) {
        auto &counters = _impl->framePerfCounters;
        const auto batchSize = static_cast<uint64_t>(pendingUpdates.size());
        ++counters.pendingBufferDrains;
        counters.pendingBuffersDrained += batchSize;
        counters.maxPendingBufferBatch = std::max(counters.maxPendingBufferBatch, batchSize);
    }
#endif
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
#if CC_D3D12_PERF_COUNTERS
    if (perfTimingEnabled) {
        const auto drainNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - drainStart).count());
        recordPendingBufferDrainTiming(drainNs);
    }
#endif
}

void CCD3D12Device::notifySubmittedFence(void *fence, uint64_t value) {
    if (!_impl) {
        return;
    }
    _impl->lastSubmittedFence = static_cast<ID3D12Fence *>(fence);
    _impl->lastSubmittedFenceValue = value;
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

#if CC_D3D12_PERF_COUNTERS
    const auto waitStart = std::chrono::steady_clock::now();
#endif
    WaitForSingleObject(_impl->fenceEvent, INFINITE);
#if CC_D3D12_PERF_COUNTERS
    const auto waitEnd = std::chrono::steady_clock::now();
    const auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(waitEnd - waitStart).count();
    recordFenceWait(static_cast<uint64_t>(std::max<int64_t>(waitUs, 0)));
#endif
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

    const bool hasPendingAsyncUploads = !_impl->pendingUploadCommandContexts.empty();

    if (!_impl->lastSubmittedFence || _impl->lastSubmittedFenceValue == 0) {
        return;
    }
    const uint64_t completedValue = _impl->lastSubmittedFence->GetCompletedValue();
    if (completedValue < _impl->lastSubmittedFenceValue ||
        _impl->lastRetiredFenceValue == _impl->lastSubmittedFenceValue) {
        return;
    }

    if (_impl->gpuDescriptorHeapPool) {
        _impl->gpuDescriptorHeapPool->reset();
    }
    if (_impl->samplerDescriptorHeapPool) {
        _impl->samplerDescriptorHeapPool->reset();
    }
    if (!hasPendingAsyncUploads) {
        if (!_impl->uploadPages.empty()) {
            notifyTransientUniformUpload();
        }
        for (auto &page : _impl->uploadPages) {
            page.offset = 0;
        }
    }
    _impl->lastRetiredFenceValue = _impl->lastSubmittedFenceValue;
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

void CCD3D12Device::recordDescriptorFlush(uint32_t copyCalls, uint32_t copiedDescriptors,
                                          uint32_t dynamicOffsetRewrites, uint32_t dynamicOffsetDescriptors,
                                          uint32_t dynamicCbvTableDescriptors,
                                          uint32_t setDescriptorHeapCalls, uint32_t rootDescriptorTableBinds) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    ++counters.descriptorFlushes;
    counters.copyDescriptorCalls += copyCalls;
    counters.copiedDescriptors += copiedDescriptors;
    counters.dynamicOffsetRewrites += dynamicOffsetRewrites;
    counters.dynamicOffsetDescriptors += dynamicOffsetDescriptors;
    counters.dynamicCbvTableDescriptors += dynamicCbvTableDescriptors;
    counters.setDescriptorHeapCalls += setDescriptorHeapCalls;
    counters.rootDescriptorTableBinds += rootDescriptorTableBinds;
#else
    (void)copyCalls;
    (void)copiedDescriptors;
    (void)dynamicOffsetRewrites;
    (void)dynamicOffsetDescriptors;
    (void)dynamicCbvTableDescriptors;
    (void)setDescriptorHeapCalls;
    (void)rootDescriptorTableBinds;
#endif
}

void CCD3D12Device::recordDescriptorStateBinds(uint32_t setDescriptorHeapCalls, uint32_t rootDescriptorTableBinds) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    _impl->framePerfCounters.setDescriptorHeapCalls += setDescriptorHeapCalls;
    _impl->framePerfCounters.rootDescriptorTableBinds += rootDescriptorTableBinds;
#else
    (void)setDescriptorHeapCalls;
    (void)rootDescriptorTableBinds;
#endif
}

void CCD3D12Device::recordDescriptorCacheAnalysis(uint32_t cacheHits, uint32_t cacheMisses,
                                                   uint32_t repackPasses, uint32_t repackDescriptors) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    counters.descriptorCacheHits += cacheHits;
    counters.descriptorCacheMisses += cacheMisses;
    counters.descriptorRepackPasses += repackPasses;
    counters.descriptorRepackDescriptors += repackDescriptors;
#else
    (void)cacheHits;
    (void)cacheMisses;
    (void)repackPasses;
    (void)repackDescriptors;
#endif
}

void CCD3D12Device::recordDescriptorBindingAnalysis(uint32_t slotLookups, uint32_t slotOwnerChanges,
                                                     uint64_t fullFlushNs, uint64_t bindingBuildNs,
                                                     uint64_t probeNs, uint64_t setUpdateNs) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    counters.descriptorCacheSlotLookups += slotLookups;
    counters.descriptorCacheSlotOwnerChanges += slotOwnerChanges;
    counters.descriptorFullFlushNs += fullFlushNs;
    counters.descriptorBindingBuildNs += bindingBuildNs;
    counters.descriptorCacheProbeNs += probeNs;
    counters.descriptorSetUpdateNs += setUpdateNs;
#else
    (void)slotLookups;
    (void)slotOwnerChanges;
    (void)fullFlushNs;
    (void)bindingBuildNs;
    (void)probeNs;
    (void)setUpdateNs;
#endif
}

void CCD3D12Device::recordDescriptorPostPhaseAnalysis(uint64_t descriptorRangePrepareNs,
                                                       uint64_t descriptorHeapNormalizeNs,
                                                       uint64_t descriptorHeapRootNs) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    counters.descriptorRangePrepareNs += descriptorRangePrepareNs;
    counters.descriptorHeapNormalizeNs += descriptorHeapNormalizeNs;
    counters.descriptorHeapRootNs += descriptorHeapRootNs;
#else
    (void)descriptorRangePrepareNs;
    (void)descriptorHeapNormalizeNs;
    (void)descriptorHeapRootNs;
#endif
}

void CCD3D12Device::recordDescriptorRangePhaseAnalysis(uint64_t descriptorCbvPrepareNs,
                                                        uint64_t descriptorSamplerPrepareNs) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    counters.descriptorCbvPrepareNs += descriptorCbvPrepareNs;
    counters.descriptorSamplerPrepareNs += descriptorSamplerPrepareNs;
#else
    (void)descriptorCbvPrepareNs;
    (void)descriptorSamplerPrepareNs;
#endif
}

void CCD3D12Device::recordResourceBarriers(uint32_t barrierCount) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || barrierCount == 0 || !isD3D12PerfLoggingRequested()) {
        return;
    }
    ++_impl->framePerfCounters.resourceBarrierCalls;
    _impl->framePerfCounters.resourceBarriers += barrierCount;
#else
    (void)barrierCount;
#endif
}

void CCD3D12Device::recordBarrierAnalysis(uint32_t textureTransitions, uint32_t bufferTransitions,
                                          uint32_t uavBarriers, uint32_t trackedAlreadyNext) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    _impl->framePerfCounters.barrierTextureTransitions += textureTransitions;
    _impl->framePerfCounters.barrierBufferTransitions += bufferTransitions;
    _impl->framePerfCounters.barrierUAV += uavBarriers;
    _impl->framePerfCounters.barrierTrackedAlreadyNext += trackedAlreadyNext;
#else
    (void)textureTransitions;
    (void)bufferTransitions;
    (void)uavBarriers;
    (void)trackedAlreadyNext;
#endif
}

void CCD3D12Device::recordDefaultBufferUpload(void *resource, uint32_t offset, uint32_t size,
                                               bool uniform, bool hostVisible, bool bufferView, bool dynamicOnly) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !resource || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    ++counters.defaultBufferCopies;
    counters.defaultBufferBytes += size;
    counters.defaultUniformCopies += uniform ? 1U : 0U;
    counters.defaultHostCopies += hostVisible ? 1U : 0U;
    counters.defaultBufferViewCopies += bufferView ? 1U : 0U;
    counters.defaultDynamicOnlyCopies += dynamicOnly ? 1U : 0U;
    const uint64_t key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(resource)) ^
                         (static_cast<uint64_t>(offset) * 0x9E3779B185EBCA87ULL);
    if (!_impl->perfUpdatedBufferDestinations.emplace(key).second) {
        ++counters.defaultRepeatedDestinations;
    }
#else
    (void)resource;
    (void)offset;
    (void)size;
    (void)uniform;
    (void)hostVisible;
    (void)bufferView;
    (void)dynamicOnly;
#endif
}

void CCD3D12Device::recordHotPathTimings(uint64_t descriptorFlushNs, uint64_t descriptorCopyNs,
                                         uint64_t defaultBufferUploadNs, uint64_t dynamicOffsetNs,
                                         uint64_t descriptorAllocateNs, uint64_t rootTableBindNs) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    _impl->framePerfCounters.descriptorFlushNs += descriptorFlushNs;
    _impl->framePerfCounters.descriptorCopyNs += descriptorCopyNs;
    _impl->framePerfCounters.defaultBufferUploadNs += defaultBufferUploadNs;
    _impl->framePerfCounters.dynamicOffsetNs += dynamicOffsetNs;
    _impl->framePerfCounters.descriptorAllocateNs += descriptorAllocateNs;
    _impl->framePerfCounters.rootTableBindNs += rootTableBindNs;
#else
    (void)descriptorFlushNs;
    (void)descriptorCopyNs;
    (void)defaultBufferUploadNs;
    (void)dynamicOffsetNs;
    (void)descriptorAllocateNs;
    (void)rootTableBindNs;
#endif
}

void CCD3D12Device::recordPendingBufferDrainTiming(uint64_t drainNs) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    _impl->framePerfCounters.pendingBufferDrainNs += drainNs;
#else
    (void)drainNs;
#endif
}

void CCD3D12Device::recordUniqueBatchRetentionTiming(uint64_t calls, uint64_t insertions, uint64_t retentionNs) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    _impl->framePerfCounters.uniqueBatchRetainCalls += calls;
    _impl->framePerfCounters.uniqueBatchRetainInsertions += insertions;
    _impl->framePerfCounters.uniqueBatchRetainNs += retentionNs;
#else
    (void)calls;
    (void)insertions;
    (void)retentionNs;
#endif
}

void CCD3D12Device::recordUploadAllocationTiming(uint64_t allocationNs) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    _impl->framePerfCounters.uploadAllocateNs += allocationNs;
#else
    (void)allocationNs;
#endif
}

void CCD3D12Device::recordBufferBatchPhaseTimings(uint64_t totalNs, uint64_t buildNs, uint64_t barrierNs,
                                                   uint64_t copyNs, uint64_t stateNs,
                                                   uint64_t fallbackNs) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    counters.bufferBatchTotalNs += totalNs;
    counters.bufferBatchBuildNs += buildNs;
    counters.bufferBatchBarrierNs += barrierNs;
    counters.bufferBatchCopyNs += copyNs;
    counters.bufferBatchStateNs += stateNs;
    counters.bufferBatchFallbackNs += fallbackNs;
#else
    (void)totalNs;
    (void)buildNs;
    (void)barrierNs;
    (void)copyNs;
    (void)stateNs;
    (void)fallbackNs;
#endif
}

void CCD3D12Device::recordDescriptorTypeAnalysis(uint32_t cbvCacheHits, uint32_t samplerCacheHits,
                                                  uint32_t cbvCacheMisses, uint32_t samplerCacheMisses,
                                                  uint32_t cbvCopiedDescriptors, uint32_t samplerCopiedDescriptors,
                                                  uint32_t cbvRepackPasses, uint32_t samplerRepackPasses,
                                                  uint32_t cbvHeapChanges, uint32_t samplerHeapChanges) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    counters.cbvCacheHits += cbvCacheHits;
    counters.samplerCacheHits += samplerCacheHits;
    counters.cbvCacheMisses += cbvCacheMisses;
    counters.samplerCacheMisses += samplerCacheMisses;
    counters.cbvCopiedDescriptors += cbvCopiedDescriptors;
    counters.samplerCopiedDescriptors += samplerCopiedDescriptors;
    counters.cbvRepackPasses += cbvRepackPasses;
    counters.samplerRepackPasses += samplerRepackPasses;
    counters.cbvHeapChanges += cbvHeapChanges;
    counters.samplerHeapChanges += samplerHeapChanges;
#else
    (void)cbvCacheHits;
    (void)samplerCacheHits;
    (void)cbvCacheMisses;
    (void)samplerCacheMisses;
    (void)cbvCopiedDescriptors;
    (void)samplerCopiedDescriptors;
    (void)cbvRepackPasses;
    (void)samplerRepackPasses;
    (void)cbvHeapChanges;
    (void)samplerHeapChanges;
#endif
}

void CCD3D12Device::recordSamplerTableAnalysis(uint32_t lookups, uint32_t uniqueTables,
                                                uint32_t uniqueDescriptors, uint32_t duplicateTableHits,
                                                uint32_t signatureHashCollisions,
                                                uint32_t consecutiveExactHits) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    counters.samplerTableLookups += lookups;
    counters.samplerUniqueTables += uniqueTables;
    counters.samplerUniqueDescriptors += uniqueDescriptors;
    counters.samplerDuplicateTableHits += duplicateTableHits;
    counters.samplerSignatureHashCollisions += signatureHashCollisions;
    counters.samplerConsecutiveExactHits += consecutiveExactHits;
#else
    (void)lookups;
    (void)uniqueTables;
    (void)uniqueDescriptors;
    (void)duplicateTableHits;
    (void)signatureHashCollisions;
    (void)consecutiveExactHits;
#endif
}

void CCD3D12Device::recordGraphicsBindAnalysis(uint32_t pipelineBindCalls, uint32_t pipelineSameLogical,
                                                uint32_t pipelineNativeChanges, uint32_t inputAssemblerBindCalls,
                                                uint32_t inputAssemblerSameLogical, uint32_t vertexBufferViewChanges,
                                                uint32_t indexBufferViewChanges, uint64_t pipelineBindNs,
                                                uint64_t inputAssemblerBindNs) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    counters.pipelineBindCalls += pipelineBindCalls;
    counters.pipelineSameLogical += pipelineSameLogical;
    counters.pipelineNativeChanges += pipelineNativeChanges;
    counters.inputAssemblerBindCalls += inputAssemblerBindCalls;
    counters.inputAssemblerSameLogical += inputAssemblerSameLogical;
    counters.vertexBufferViewChanges += vertexBufferViewChanges;
    counters.indexBufferViewChanges += indexBufferViewChanges;
    counters.pipelineBindNs += pipelineBindNs;
    counters.inputAssemblerBindNs += inputAssemblerBindNs;
#else
    (void)pipelineBindCalls;
    (void)pipelineSameLogical;
    (void)pipelineNativeChanges;
    (void)inputAssemblerBindCalls;
    (void)inputAssemblerSameLogical;
    (void)vertexBufferViewChanges;
    (void)indexBufferViewChanges;
    (void)pipelineBindNs;
    (void)inputAssemblerBindNs;
#endif
}

void CCD3D12Device::recordCommandHotPathAnalysis(uint32_t bindDescriptorSetCalls, uint64_t bindDescriptorSetNs,
                                                  uint32_t drawCalls, uint64_t drawNs) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    counters.bindDescriptorSetCalls += bindDescriptorSetCalls;
    counters.bindDescriptorSetNs += bindDescriptorSetNs;
    counters.drawCalls += drawCalls;
    counters.drawNs += drawNs;
#else
    (void)bindDescriptorSetCalls;
    (void)bindDescriptorSetNs;
    (void)drawCalls;
    (void)drawNs;
#endif
}

void CCD3D12Device::recordDrawPhaseAnalysis(uint64_t drawPendingBufferNs, uint64_t drawDescriptorFlushNs,
                                             uint64_t drawIssueNs) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    counters.drawPendingBufferNs += drawPendingBufferNs;
    counters.drawDescriptorFlushNs += drawDescriptorFlushNs;
    counters.drawIssueNs += drawIssueNs;
#else
    (void)drawPendingBufferNs;
    (void)drawDescriptorFlushNs;
    (void)drawIssueNs;
#endif
}

void CCD3D12Device::recordCommandReuseAnalysis(uint32_t eventCount, bool hadPrevious,
                                                bool exactMatch, uint32_t firstMismatchIndex) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    counters.commandReuseEvents += eventCount;
    counters.commandReuseComparisons += hadPrevious ? 1U : 0U;
    counters.commandReuseExactMatches += exactMatch ? 1U : 0U;
    if (hadPrevious && !exactMatch) {
        counters.commandReuseFirstMismatch = std::min(counters.commandReuseFirstMismatch, firstMismatchIndex);
    }
#else
    (void)eventCount;
    (void)hadPrevious;
    (void)exactMatch;
    (void)firstMismatchIndex;
#endif
}

void CCD3D12Device::recordFramePhaseAnalysis(uint64_t commandBeginNs, uint64_t commandRecordingNs,
                                             uint64_t commandEndNs, uint64_t queueSubmitNs,
                                             uint64_t queueExecuteNs, uint64_t queueSignalNs,
                                             uint64_t acquireNs, uint64_t presentNs) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    auto &counters = _impl->framePerfCounters;
    counters.commandBeginNs += commandBeginNs;
    counters.commandRecordingNs += commandRecordingNs;
    counters.commandEndNs += commandEndNs;
    counters.queueSubmitNs += queueSubmitNs;
    counters.queueExecuteNs += queueExecuteNs;
    counters.queueSignalNs += queueSignalNs;
    counters.acquireNs += acquireNs;
    counters.presentNs += presentNs;
#else
    (void)commandBeginNs;
    (void)commandRecordingNs;
    (void)commandEndNs;
    (void)queueSubmitNs;
    (void)queueExecuteNs;
    (void)queueSignalNs;
    (void)acquireNs;
    (void)presentNs;
#endif
}

void CCD3D12Device::recordFenceWait(uint64_t waitMicroseconds) {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    ++_impl->framePerfCounters.fenceWaits;
    _impl->framePerfCounters.fenceWaitMicroseconds += waitMicroseconds;
#else
    (void)waitMicroseconds;
#endif
}

void CCD3D12Device::reportAndResetFramePerfCounters() {
#if CC_D3D12_PERF_COUNTERS
    if (!_impl || !isD3D12PerfLoggingRequested()) {
        return;
    }
    const auto &counters = _impl->framePerfCounters;
    CC_LOG_INFO("[D3D12-PERF] frame=%llu flushDescriptorSets=%llu CopyDescriptorsSimple=%llu copiedDescriptors=%llu "
                "dynamicOffsetRewrites=%llu dynamicOffsetDescriptors=%llu dynamicCbvTableDescriptors=%llu "
                "SetDescriptorHeaps=%llu rootTableBinds=%llu descriptorCacheHits=%llu "
                "descriptorCacheMisses=%llu descriptorRepackPasses=%llu descriptorRepackDescriptors=%llu "
                "ResourceBarrierCalls=%llu ResourceBarriers=%llu barrierTextureTransitions=%llu "
                "barrierBufferTransitions=%llu barrierUAV=%llu barrierTrackedAlreadyNext=%llu "
                "defaultBufferCopies=%llu defaultBufferBytes=%llu defaultUniformCopies=%llu "
                "defaultHostCopies=%llu defaultBufferViewCopies=%llu defaultDynamicOnlyCopies=%llu "
                "defaultRepeatedDestinations=%llu pendingBufferDrains=%llu "
                "pendingBuffersDrained=%llu maxPendingBufferBatch=%llu pendingBufferDrainUs=%llu "
                "pendingBufferEnqueues=%llu pendingBufferQueueGrowths=%llu pendingBufferQueueGrowthUs=%llu "
                "uniqueBatchRetainCalls=%llu uniqueBatchRetainInsertions=%llu "
                "uniqueBatchRetainDuplicates=%llu uniqueBatchRetainUs=%llu "
                "uploadAllocateUs=%llu bufferBatchTotalUs=%llu bufferBatchBuildUs=%llu bufferBatchBarrierUs=%llu "
                "bufferBatchCopyUs=%llu bufferBatchStateUs=%llu bufferBatchFallbackUs=%llu "
                "descriptorFlushUs=%llu descriptorCopyUs=%llu defaultBufferUploadUs=%llu dynamicOffsetUs=%llu "
                "descriptorAllocateUs=%llu rootTableBindUs=%llu "
                "fenceWaits=%llu fenceWaitUs=%llu",
                static_cast<unsigned long long>(++_impl->perfFrameIndex),
                static_cast<unsigned long long>(counters.descriptorFlushes),
                static_cast<unsigned long long>(counters.copyDescriptorCalls),
                static_cast<unsigned long long>(counters.copiedDescriptors),
                static_cast<unsigned long long>(counters.dynamicOffsetRewrites),
                static_cast<unsigned long long>(counters.dynamicOffsetDescriptors),
                static_cast<unsigned long long>(counters.dynamicCbvTableDescriptors),
                static_cast<unsigned long long>(counters.setDescriptorHeapCalls),
                static_cast<unsigned long long>(counters.rootDescriptorTableBinds),
                static_cast<unsigned long long>(counters.descriptorCacheHits),
                static_cast<unsigned long long>(counters.descriptorCacheMisses),
                static_cast<unsigned long long>(counters.descriptorRepackPasses),
                static_cast<unsigned long long>(counters.descriptorRepackDescriptors),
                static_cast<unsigned long long>(counters.resourceBarrierCalls),
                static_cast<unsigned long long>(counters.resourceBarriers),
                static_cast<unsigned long long>(counters.barrierTextureTransitions),
                static_cast<unsigned long long>(counters.barrierBufferTransitions),
                static_cast<unsigned long long>(counters.barrierUAV),
                static_cast<unsigned long long>(counters.barrierTrackedAlreadyNext),
                static_cast<unsigned long long>(counters.defaultBufferCopies),
                static_cast<unsigned long long>(counters.defaultBufferBytes),
                static_cast<unsigned long long>(counters.defaultUniformCopies),
                static_cast<unsigned long long>(counters.defaultHostCopies),
                static_cast<unsigned long long>(counters.defaultBufferViewCopies),
                static_cast<unsigned long long>(counters.defaultDynamicOnlyCopies),
                static_cast<unsigned long long>(counters.defaultRepeatedDestinations),
                static_cast<unsigned long long>(counters.pendingBufferDrains),
                static_cast<unsigned long long>(counters.pendingBuffersDrained),
                static_cast<unsigned long long>(counters.maxPendingBufferBatch),
                static_cast<unsigned long long>(counters.pendingBufferDrainNs / 1000ULL),
                static_cast<unsigned long long>(counters.pendingBufferEnqueues),
                static_cast<unsigned long long>(counters.pendingBufferQueueGrowths),
                static_cast<unsigned long long>(counters.pendingBufferQueueGrowthNs / 1000ULL),
                static_cast<unsigned long long>(counters.uniqueBatchRetainCalls),
                static_cast<unsigned long long>(counters.uniqueBatchRetainInsertions),
                static_cast<unsigned long long>(counters.uniqueBatchRetainCalls - counters.uniqueBatchRetainInsertions),
                static_cast<unsigned long long>(counters.uniqueBatchRetainNs / 1000ULL),
                static_cast<unsigned long long>(counters.uploadAllocateNs / 1000ULL),
                static_cast<unsigned long long>(counters.bufferBatchTotalNs / 1000ULL),
                static_cast<unsigned long long>(counters.bufferBatchBuildNs / 1000ULL),
                static_cast<unsigned long long>(counters.bufferBatchBarrierNs / 1000ULL),
                static_cast<unsigned long long>(counters.bufferBatchCopyNs / 1000ULL),
                static_cast<unsigned long long>(counters.bufferBatchStateNs / 1000ULL),
                static_cast<unsigned long long>(counters.bufferBatchFallbackNs / 1000ULL),
                static_cast<unsigned long long>(counters.descriptorFlushNs / 1000ULL),
                static_cast<unsigned long long>(counters.descriptorCopyNs / 1000ULL),
                static_cast<unsigned long long>(counters.defaultBufferUploadNs / 1000ULL),
                static_cast<unsigned long long>(counters.dynamicOffsetNs / 1000ULL),
                static_cast<unsigned long long>(counters.descriptorAllocateNs / 1000ULL),
                static_cast<unsigned long long>(counters.rootTableBindNs / 1000ULL),
                static_cast<unsigned long long>(counters.fenceWaits),
                static_cast<unsigned long long>(counters.fenceWaitMicroseconds));
    const uint64_t measuredDescriptorBindingNs = counters.descriptorSetUpdateNs +
                                                 counters.descriptorCacheProbeNs;
    CC_LOG_INFO("[D3D12-PERF-DESCRIPTOR-BINDING] frame=%llu descriptorFullFlushUs=%llu "
                "descriptorBindingBuildUs=%llu descriptorCacheProbeUs=%llu descriptorSetUpdateUs=%llu "
                "descriptorBindingResidualUs=%llu "
                "descriptorCacheSlotLookups=%llu descriptorCacheSlotOwnerChanges=%llu "
                "descriptorCacheSlotOwnerHits=%llu",
                static_cast<unsigned long long>(_impl->perfFrameIndex),
                static_cast<unsigned long long>(counters.descriptorFullFlushNs / 1000ULL),
                static_cast<unsigned long long>(counters.descriptorBindingBuildNs / 1000ULL),
                static_cast<unsigned long long>(counters.descriptorCacheProbeNs / 1000ULL),
                static_cast<unsigned long long>(counters.descriptorSetUpdateNs / 1000ULL),
                static_cast<unsigned long long>((counters.descriptorBindingBuildNs -
                                                 std::min(counters.descriptorBindingBuildNs,
                                                          measuredDescriptorBindingNs)) /
                                                1000ULL),
                static_cast<unsigned long long>(counters.descriptorCacheSlotLookups),
                static_cast<unsigned long long>(counters.descriptorCacheSlotOwnerChanges),
                static_cast<unsigned long long>(counters.descriptorCacheSlotLookups - counters.descriptorCacheSlotOwnerChanges));
    const uint64_t measuredDescriptorPostNs = counters.descriptorRangePrepareNs +
                                              counters.descriptorHeapNormalizeNs +
                                              counters.descriptorHeapRootNs;
    CC_LOG_INFO("[D3D12-PERF-DESCRIPTOR-POST-PHASES] frame=%llu descriptorRangePrepareUs=%llu "
                "descriptorHeapNormalizeUs=%llu descriptorHeapRootUs=%llu descriptorPostResidualUs=%llu",
                static_cast<unsigned long long>(_impl->perfFrameIndex),
                static_cast<unsigned long long>(counters.descriptorRangePrepareNs / 1000ULL),
                static_cast<unsigned long long>(counters.descriptorHeapNormalizeNs / 1000ULL),
                static_cast<unsigned long long>(counters.descriptorHeapRootNs / 1000ULL),
                static_cast<unsigned long long>((counters.descriptorFlushNs -
                                                 std::min(counters.descriptorFlushNs, measuredDescriptorPostNs)) /
                                                1000ULL));
    const uint64_t measuredDescriptorRangeNs = counters.descriptorCbvPrepareNs +
                                               counters.descriptorSamplerPrepareNs;
    CC_LOG_INFO("[D3D12-PERF-DESCRIPTOR-RANGE-PHASES] frame=%llu descriptorCbvPrepareUs=%llu "
                "descriptorSamplerPrepareUs=%llu descriptorRangeResidualUs=%llu",
                static_cast<unsigned long long>(_impl->perfFrameIndex),
                static_cast<unsigned long long>(counters.descriptorCbvPrepareNs / 1000ULL),
                static_cast<unsigned long long>(counters.descriptorSamplerPrepareNs / 1000ULL),
                static_cast<unsigned long long>((counters.descriptorRangePrepareNs -
                                                 std::min(counters.descriptorRangePrepareNs, measuredDescriptorRangeNs)) /
                                                1000ULL));
    CC_LOG_INFO("[D3D12-PERF-TYPES] frame=%llu cbvCacheHits=%llu samplerCacheHits=%llu "
                "cbvCacheMisses=%llu samplerCacheMisses=%llu cbvCopiedDescriptors=%llu "
                "samplerCopiedDescriptors=%llu cbvRepackPasses=%llu samplerRepackPasses=%llu "
                "cbvHeapChanges=%llu samplerHeapChanges=%llu",
                static_cast<unsigned long long>(_impl->perfFrameIndex),
                static_cast<unsigned long long>(counters.cbvCacheHits),
                static_cast<unsigned long long>(counters.samplerCacheHits),
                static_cast<unsigned long long>(counters.cbvCacheMisses),
                static_cast<unsigned long long>(counters.samplerCacheMisses),
                static_cast<unsigned long long>(counters.cbvCopiedDescriptors),
                static_cast<unsigned long long>(counters.samplerCopiedDescriptors),
                static_cast<unsigned long long>(counters.cbvRepackPasses),
                static_cast<unsigned long long>(counters.samplerRepackPasses),
                static_cast<unsigned long long>(counters.cbvHeapChanges),
                static_cast<unsigned long long>(counters.samplerHeapChanges));
    CC_LOG_INFO("[D3D12-PERF-SAMPLERS] frame=%llu samplerTableLookups=%llu "
                "samplerUniqueTables=%llu samplerUniqueDescriptors=%llu "
                "samplerDuplicateTableHits=%llu samplerSignatureHashCollisions=%llu "
                "samplerConsecutiveExactHits=%llu",
                static_cast<unsigned long long>(_impl->perfFrameIndex),
                static_cast<unsigned long long>(counters.samplerTableLookups),
                static_cast<unsigned long long>(counters.samplerUniqueTables),
                static_cast<unsigned long long>(counters.samplerUniqueDescriptors),
                static_cast<unsigned long long>(counters.samplerDuplicateTableHits),
                static_cast<unsigned long long>(counters.samplerSignatureHashCollisions),
                static_cast<unsigned long long>(counters.samplerConsecutiveExactHits));
    CC_LOG_INFO("[D3D12-PERF-BINDS] frame=%llu pipelineBindCalls=%llu pipelineSameLogical=%llu "
                "pipelineNativeChanges=%llu pipelineBindUs=%llu inputAssemblerBindCalls=%llu "
                "inputAssemblerSameLogical=%llu vertexBufferViewChanges=%llu "
                "indexBufferViewChanges=%llu inputAssemblerBindUs=%llu",
                static_cast<unsigned long long>(_impl->perfFrameIndex),
                static_cast<unsigned long long>(counters.pipelineBindCalls),
                static_cast<unsigned long long>(counters.pipelineSameLogical),
                static_cast<unsigned long long>(counters.pipelineNativeChanges),
                static_cast<unsigned long long>(counters.pipelineBindNs / 1000ULL),
                static_cast<unsigned long long>(counters.inputAssemblerBindCalls),
                static_cast<unsigned long long>(counters.inputAssemblerSameLogical),
                static_cast<unsigned long long>(counters.vertexBufferViewChanges),
                static_cast<unsigned long long>(counters.indexBufferViewChanges),
                static_cast<unsigned long long>(counters.inputAssemblerBindNs / 1000ULL));
    CC_LOG_INFO("[D3D12-PERF-HOT-CALLS] frame=%llu bindDescriptorSetCalls=%llu "
                "bindDescriptorSetUs=%llu drawCalls=%llu drawUs=%llu",
                static_cast<unsigned long long>(_impl->perfFrameIndex),
                static_cast<unsigned long long>(counters.bindDescriptorSetCalls),
                static_cast<unsigned long long>(counters.bindDescriptorSetNs / 1000ULL),
                static_cast<unsigned long long>(counters.drawCalls),
                static_cast<unsigned long long>(counters.drawNs / 1000ULL));
    CC_LOG_INFO("[D3D12-PERF-REUSE-GATE] frame=%llu events=%llu comparisons=%llu "
                "exactMatches=%llu firstMismatchIndex=%u",
                static_cast<unsigned long long>(_impl->perfFrameIndex),
                static_cast<unsigned long long>(counters.commandReuseEvents),
                static_cast<unsigned long long>(counters.commandReuseComparisons),
                static_cast<unsigned long long>(counters.commandReuseExactMatches),
                counters.commandReuseFirstMismatch);
    const uint64_t measuredDrawNs = counters.drawPendingBufferNs + counters.drawDescriptorFlushNs + counters.drawIssueNs;
    CC_LOG_INFO("[D3D12-PERF-DRAW-PHASES] frame=%llu drawCalls=%llu drawPendingBufferUs=%llu "
                "drawDescriptorFlushUs=%llu drawIssueUs=%llu drawResidualUs=%llu",
                static_cast<unsigned long long>(_impl->perfFrameIndex),
                static_cast<unsigned long long>(counters.drawCalls),
                static_cast<unsigned long long>(counters.drawPendingBufferNs / 1000ULL),
                static_cast<unsigned long long>(counters.drawDescriptorFlushNs / 1000ULL),
                static_cast<unsigned long long>(counters.drawIssueNs / 1000ULL),
                static_cast<unsigned long long>((counters.drawNs - std::min(counters.drawNs, measuredDrawNs)) / 1000ULL));
    CC_LOG_INFO("[D3D12-PERF-FRAME-PHASES] frame=%llu commandBeginUs=%llu "
                "commandRecordingUs=%llu commandEndUs=%llu queueSubmitUs=%llu "
                "queueExecuteUs=%llu queueSignalUs=%llu acquireUs=%llu presentUs=%llu",
                static_cast<unsigned long long>(_impl->perfFrameIndex),
                static_cast<unsigned long long>(counters.commandBeginNs / 1000ULL),
                static_cast<unsigned long long>(counters.commandRecordingNs / 1000ULL),
                static_cast<unsigned long long>(counters.commandEndNs / 1000ULL),
                static_cast<unsigned long long>(counters.queueSubmitNs / 1000ULL),
                static_cast<unsigned long long>(counters.queueExecuteNs / 1000ULL),
                static_cast<unsigned long long>(counters.queueSignalNs / 1000ULL),
                static_cast<unsigned long long>(counters.acquireNs / 1000ULL),
                static_cast<unsigned long long>(counters.presentNs / 1000ULL));
    _impl->framePerfCounters = {};
    _impl->perfUpdatedBufferDestinations.clear();
#endif
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
    reportAndResetFramePerfCounters();
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
        CC_LOG_INFO("[D3D12-MIP-DIAG] flush deferred cube upload resource=%p regions=%u complete=%s",
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
        CC_LOG_INFO("[D3D12-MIP-DIAG] discard deferred cube upload texture=%p removed=%zu",
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
    CC_LOG_INFO("[D3D12-MIP-DIAG] defer complete cube upload resource=%p size=%ux%u layers=%u regions=%u",
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
    if (!textureResource) {
        return;
    }

    const auto &textureInfo = dst->getInfo();
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
        CC_LOG_INFO("[D3D12-MIP-DIAG] device upload resource=%p size=%ux%u levels=%u layers=%u format=%u regions=%u",
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

    const D3D12_RESOURCE_STATES previousState = d3d12Texture->getCurrentState();
    if (previousState != D3D12_RESOURCE_STATE_COPY_DEST) {
        auto toCopyDest = textureTransition(textureResource, previousState, D3D12_RESOURCE_STATE_COPY_DEST);
        uploadCommandList->ResourceBarrier(1, &toCopyDest);
        d3d12Texture->setCurrentState(D3D12_RESOURCE_STATE_COPY_DEST);
    }

    for (uint32_t regionIndex = 0; regionIndex < count; ++regionIndex) {
        if (!buffers[regionIndex]) {
            continue;
        }

        const auto &region = regions[regionIndex];
        const uint32_t mipLevel = region.texSubres.mipLevel;
        const uint32_t arrayLayer = textureInfo.type == TextureType::TEX3D ? 0 : region.texSubres.baseArrayLayer;
        const uint32_t subresource = mipLevel + arrayLayer * textureInfo.levelCount;
        if (diagnoseMipUpload) {
            CC_LOG_INFO("[D3D12-MIP-DIAG] region=%u mip=%u layer=%u extent=%ux%ux%u offset=%d,%d,%d",
                        regionIndex, mipLevel, arrayLayer,
                        region.texExtent.width, region.texExtent.height, region.texExtent.depth,
                        region.texOffset.x, region.texOffset.y, region.texOffset.z);
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 uploadSize = 0;
        D3D12_RESOURCE_DESC textureDesc = textureResource->GetDesc();
        _impl->d3dDevice->GetCopyableFootprints(&textureDesc, subresource, 1, 0, &footprint, &rowCount, &rowSizeInBytes, &uploadSize);
        if (uploadSize == 0 || rowCount == 0) {
            continue;
        }

        auto upload = allocateUploadBuffer(uploadSize + footprint.Offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!upload.isValid || !upload.mappedData || !upload.resource) {
            CC_LOG_ERROR("D3D12 texture upload allocation failed. size=%llu",
                         static_cast<unsigned long long>(uploadSize + footprint.Offset));
            continue;
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
            mipLevel,
            arrayLayer,
            textureInfo.type == TextureType::TEX3D ? 1 : region.texSubres.layerCount);
    }

    ccstd::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> mipDescriptorHeaps;
    const bool shouldGenerateMipmaps = d3d12Texture->shouldGenerateMipmapsAfterUpload();
    const bool generatedMipmaps =
        shouldGenerateMipmaps &&
        generateD3D12Mipmaps(_impl->d3dDevice.Get(), uploadCommandList.Get(), textureResource,
                             textureInfo, mipDescriptorHeaps);
    if (generatedMipmaps) {
        d3d12Texture->markMipmapsGenerated();
    }
    if (diagnoseMipUpload) {
        CC_LOG_INFO("[D3D12-MIP-DIAG] generation resource=%p result=%s descriptorHeaps=%zu",
                    textureResource,
                    shouldGenerateMipmaps ? (generatedMipmaps ? "success" : "failed") : "deferred",
                    mipDescriptorHeaps.size());
    }
    const D3D12_RESOURCE_STATES postUploadState =
        generatedMipmaps ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    if (postUploadState != previousState) {
        auto restoreState = textureTransition(
        textureResource,
            postUploadState,
            previousState);
        uploadCommandList->ResourceBarrier(1, &restoreState);
    }
    d3d12Texture->setCurrentState(previousState);

    hr = uploadCommandList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 upload command list close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    ID3D12CommandList *commandLists[] = {uploadCommandList.Get()};
    _impl->graphicsQueue->ExecuteCommandLists(1, commandLists);

    ++_impl->fenceValue;
    hr = _impl->graphicsQueue->Signal(_impl->frameFence.Get(), _impl->fenceValue);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 upload fence signal failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        waitForGpu();
        return;
    }

    Impl::PendingUploadCommandContext pendingContext;
    pendingContext.commandAllocator = std::move(uploadCommandAllocator);
    pendingContext.commandList = std::move(uploadCommandList);
    pendingContext.referencedResources.emplace_back(textureResource);
    pendingContext.descriptorHeaps = std::move(mipDescriptorHeaps);
    pendingContext.fenceValue = _impl->fenceValue;
    _impl->pendingUploadCommandContexts.emplace_back(std::move(pendingContext));
    if (diagnoseMipUpload) {
        CC_LOG_INFO("[D3D12-MIP-DIAG] upload submitted async resource=%p fence=%llu pendingContexts=%zu",
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
    if (!textureResource) {
        return;
    }

    const auto &textureInfo = src->getInfo();
    if (textureInfo.samples != SampleCount::X1) {
        CC_LOG_WARNING("D3D12 texture readback skipped for multisampled texture. Resolve before readback.");
        return;
    }

    waitForGpu();

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

    const D3D12_RESOURCE_STATES previousState = d3d12Texture->getCurrentState();
    if (previousState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        auto toCopySource = textureTransition(textureResource, previousState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        _impl->commandList->ResourceBarrier(1, &toCopySource);
        d3d12Texture->setCurrentState(D3D12_RESOURCE_STATE_COPY_SOURCE);
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
        const uint32_t mipLevel = copyRegion.texSubres.mipLevel;
        const uint32_t arrayLayer = textureInfo.type == TextureType::TEX3D ? 0 : copyRegion.texSubres.baseArrayLayer;
        const uint32_t subresource = mipLevel + arrayLayer * textureInfo.levelCount;

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

    if (previousState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        auto restoreState = textureTransition(textureResource, D3D12_RESOURCE_STATE_COPY_SOURCE, previousState);
        _impl->commandList->ResourceBarrier(1, &restoreState);
        d3d12Texture->setCurrentState(previousState);
    }

    hr = _impl->commandList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 readback command list close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    if (!readbackRegions.empty()) {
        ID3D12CommandList *commandLists[] = {_impl->commandList.Get()};
        _impl->graphicsQueue->ExecuteCommandLists(1, commandLists);
        waitForGpu();

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
        CC_LOG_INFO("D3D12 debug layer disabled; set CC_D3D12_DEBUG_LAYER=1 for API validation.");
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

D3D12DescriptorHeapPool *CCD3D12Device::getCPUDescriptorHeapPool() const {
    return _impl ? _impl->cpuDescriptorHeapPool.get() : nullptr;
}

D3D12DescriptorHeapPool *CCD3D12Device::getCPUSamplerDescriptorHeapPool() const {
    return _impl ? _impl->cpuSamplerDescriptorHeapPool.get() : nullptr;
}

} // namespace gfx
} // namespace cc
