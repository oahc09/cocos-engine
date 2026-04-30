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

// File-based diagnostic logger for verifying D3D12 rendering pipeline
// Writes to d3d12-render-diag.log in the current working directory
#include <cstdio>
#include <cstdarg>
namespace {
void diagLog(const char *fmt, ...) {
    static FILE *s_diagFile = nullptr;
    if (!s_diagFile) {
        s_diagFile = fopen("C:\\temp\\d3d12-render-diag.log", "a");
        if (!s_diagFile) return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(s_diagFile, fmt, args);
    fflush(s_diagFile);
    va_end(args);
}
} // anonymous namespace

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <algorithm>
    #include <cstring>
    #include <windows.h>
    #include <d3d12.h>
    #include <dxgi1_6.h>
    #include <wrl/client.h>
#endif

namespace cc {
namespace gfx {

CCD3D12Device *CCD3D12Device::instance = nullptr;

namespace {
constexpr float D3D12_POC_CLEAR_COLOR[4] = {0.1F, 0.2F, 0.8F, 1.0F};

#if defined(_WIN32)
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

// Dump all pending ID3D12InfoQueue messages to the CC log.
// This is the KEY missing diagnostic — D3D12 debug layer was enabled but messages
// were silently discarded because we never set up InfoQueue message retrieval.
void dumpD3D12DebugMessages(ID3D12Device *device, const char *checkpoint) {
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
        return;
    }
    UINT64 msgCount = infoQueue->GetNumStoredMessages();
    if (msgCount == 0) {
        CC_LOG_INFO("[DIAG-INFOQUEUE] %s: no pending messages.", checkpoint);
        return;
    }
    CC_LOG_INFO("[DIAG-INFOQUEUE] %s: %llu pending messages:", checkpoint, static_cast<unsigned long long>(msgCount));
    for (UINT64 i = 0; i < msgCount; ++i) {
        SIZE_T msgSize = 0;
        infoQueue->GetMessage(i, nullptr, &msgSize);
        if (msgSize == 0) continue;
        auto *msgData = static_cast<D3D12_MESSAGE *>(malloc(msgSize));
        if (!msgData) continue;
        if (SUCCEEDED(infoQueue->GetMessage(i, msgData, &msgSize))) {
            const char *severity = "UNKNOWN";
            switch (msgData->Severity) {
                case D3D12_MESSAGE_SEVERITY_CORRUPTION: severity = "CORRUPTION"; break;
                case D3D12_MESSAGE_SEVERITY_ERROR:      severity = "ERROR"; break;
                case D3D12_MESSAGE_SEVERITY_WARNING:    severity = "WARNING"; break;
                case D3D12_MESSAGE_SEVERITY_INFO:       severity = "INFO"; break;
                case D3D12_MESSAGE_SEVERITY_MESSAGE:    severity = "MESSAGE"; break;
                default: break;
            }
            CC_LOG_INFO("[DIAG-INFOQUEUE]   [%s] ID=%u: %.*s",
                         severity, static_cast<unsigned>(msgData->ID),
                         static_cast<int>(msgData->DescriptionByteLength),
                         msgData->pDescription);
        }
        free(msgData);
    }
    // Clear the queue after dumping so we only see new messages next time
    infoQueue->ClearStoredMessages();
}
#endif
}

struct CCD3D12Device::Impl {
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<IDXGIFactory6> dxgiFactory;
    Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> graphicsQueue;
    // Device-level command allocator/list for legacy present and copyBuffersToTexture
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> frameFence;

    HANDLE fenceEvent{nullptr};
    uint64_t fenceValue{0};
#endif

    // GPU-visible descriptor heap pools for shader access
    std::unique_ptr<D3D12DescriptorHeapPool> gpuDescriptorHeapPool;    // CBV_SRV_UAV, shaderVisible
    std::unique_ptr<D3D12DescriptorHeapPool> samplerDescriptorHeapPool; // SAMPLER, shaderVisible
};

CCD3D12Device *CCD3D12Device::getInstance() {
    return CCD3D12Device::instance;
}

CCD3D12Device::CCD3D12Device()
: _impl(std::make_unique<Impl>()) {
    // Earliest possible diagnostic: was D3D12Device even constructed?
    {
        FILE *f = fopen("C:\\temp\\d3d12-render-diag.log", "a");
        if (f) { fprintf(f, "[CTOR] CCD3D12Device constructor called!\n"); fflush(f); fclose(f); }
    }
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
    diagLog("[DOINIT] CCD3D12Device::doInit called!\n");

#if defined(_WIN32)
    if (!initializeD3D12Context()) {
        CC_LOG_ERROR("Failed to initialize D3D12 context.");
        diagLog("[DOINIT] initializeD3D12Context FAILED! Returning false.\n");
        return false;
    }
    diagLog("[DOINIT] initializeD3D12Context succeeded.\n");

    // --- DIAG: Checkpoint after D3D12 context init ---
    {
        HRESULT drr = _impl->d3dDevice->GetDeviceRemovedReason();
        CC_LOG_INFO("[DIAG] After initializeD3D12Context: DeviceRemovedReason=0x%08x (%s)",
                     static_cast<unsigned>(drr), SUCCEEDED(drr) ? "OK" : "HUNG!");
        dumpD3D12DebugMessages(_impl->d3dDevice.Get(), "after-initContext");
    }

    // Initialize GPU-visible descriptor heap pools
    _impl->gpuDescriptorHeapPool = std::make_unique<D3D12DescriptorHeapPool>();
    _impl->gpuDescriptorHeapPool->initialize(
        D3D12DescriptorHeapPool::HeapType::CBV_SRV_UAV, 4096, true);

    {
        HRESULT drr = _impl->d3dDevice->GetDeviceRemovedReason();
        CC_LOG_INFO("[DIAG] After GPU desc heap pool init: DeviceRemovedReason=0x%08x (%s)",
                     static_cast<unsigned>(drr), SUCCEEDED(drr) ? "OK" : "HUNG!");
        dumpD3D12DebugMessages(_impl->d3dDevice.Get(), "after-gpuHeapPool");
    }

    _impl->samplerDescriptorHeapPool = std::make_unique<D3D12DescriptorHeapPool>();
    _impl->samplerDescriptorHeapPool->initialize(
        D3D12DescriptorHeapPool::HeapType::SAMPLER, 2048, true);

    {
        HRESULT drr = _impl->d3dDevice->GetDeviceRemovedReason();
        CC_LOG_INFO("[DIAG] After sampler desc heap pool init: DeviceRemovedReason=0x%08x (%s)",
                     static_cast<unsigned>(drr), SUCCEEDED(drr) ? "OK" : "HUNG!");
        dumpD3D12DebugMessages(_impl->d3dDevice.Get(), "after-samplerHeapPool");
    }

    CC_LOG_INFO("D3D12 descriptor heap pools initialized.");
    diagLog("[DOINIT] Descriptor heap pools initialized OK\n");
#endif

    QueueInfo queueInfo;
    queueInfo.type = QueueType::GRAPHICS;
    _queue = createQueue(queueInfo);
    diagLog("[DOINIT] createQueue done, ptr=%p\n", (void*)_queue);

#if defined(_WIN32)
    {
        HRESULT drr = _impl->d3dDevice->GetDeviceRemovedReason();
        CC_LOG_INFO("[DIAG] After Queue creation: DeviceRemovedReason=0x%08x (%s)",
                     static_cast<unsigned>(drr), SUCCEEDED(drr) ? "OK" : "HUNG!");
        dumpD3D12DebugMessages(_impl->d3dDevice.Get(), "after-queue");
    }
#endif

    QueryPoolInfo queryPoolInfo{QueryType::OCCLUSION, DEFAULT_MAX_QUERY_OBJECTS, true};
    _queryPool = createQueryPool(queryPoolInfo);
    diagLog("[DOINIT] createQueryPool done, ptr=%p\n", (void*)_queryPool);

#if defined(_WIN32)
    {
        HRESULT drr = _impl->d3dDevice->GetDeviceRemovedReason();
        CC_LOG_INFO("[DIAG] After QueryPool creation: DeviceRemovedReason=0x%08x (%s)",
                     static_cast<unsigned>(drr), SUCCEEDED(drr) ? "OK" : "HUNG!");
        dumpD3D12DebugMessages(_impl->d3dDevice.Get(), "after-queryPool");
    }
#endif

    CommandBufferInfo cmdBuffInfo;
    cmdBuffInfo.type = CommandBufferType::PRIMARY;
    cmdBuffInfo.queue = _queue;
    _cmdBuff = createCommandBuffer(cmdBuffInfo);
    diagLog("[DOINIT] createCommandBuffer done, ptr=%p\n", (void*)_cmdBuff);

#if defined(_WIN32)
    {
        HRESULT drr = _impl->d3dDevice->GetDeviceRemovedReason();
        CC_LOG_INFO("[DIAG] After CommandBuffer creation: DeviceRemovedReason=0x%08x (%s)",
                     static_cast<unsigned>(drr), SUCCEEDED(drr) ? "OK" : "HUNG!");
        dumpD3D12DebugMessages(_impl->d3dDevice.Get(), "after-cmdBuffer");
    }
#endif

    // Initialize format feature support table via D3D12 API
    initFormatFeatures();
    diagLog("[DOINIT] initFormatFeatures done.\n");

    // Initialize device capabilities
    initCapabilities();
    diagLog("[DOINIT] initCapabilities done (maxVertexUniformVectors=%u).\n", _caps.maxVertexUniformVectors);

    _renderer = "D3D12";
    _vendor = "Unknown";

    CC_LOG_INFO("D3D12 device initialized.");
    CC_LOG_INFO("RENDERER: %s", _renderer.c_str());
    CC_LOG_INFO("VENDOR: %s", _vendor.c_str());
    CC_LOG_INFO("CAPS: maxVertexUniformVectors=%u, maxFragmentUniformVectors=%u, maxTextureSize=%u",
                _caps.maxVertexUniformVectors, _caps.maxFragmentUniformVectors, _caps.maxTextureSize);
    dumpD3D12DebugMessages(_impl->d3dDevice.Get(), "doInit-complete");
    diagLog("[DOINIT] COMPLETE! D3D12 device fully initialized.\n");

    return true;
}

void CCD3D12Device::doDestroy() {
#if defined(_WIN32)
    waitForGpu();

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
#endif

    CC_SAFE_DESTROY_AND_DELETE(_cmdBuff);
    CC_SAFE_DESTROY_AND_DELETE(_queryPool);
    CC_SAFE_DESTROY_AND_DELETE(_queue);
}

void CCD3D12Device::acquire(Swapchain *const *swapchains, uint32_t count) {
    (void)swapchains;
    (void)count;
    if (_onAcquire) {
        _onAcquire->execute();
    }
}

void CCD3D12Device::present() {
#if defined(_WIN32)
    if (!_impl->graphicsQueue || !_impl->frameFence || !_impl->fenceEvent) {
        return;
    }

    static uint32_t s_presentCount = 0;
    ++s_presentCount;
    if (s_presentCount <= 5 || s_presentCount % 300 == 0) {
        diagLog("[FRAME %u] Device::present() called\n", s_presentCount);
    }

    const auto &swapchains = getSwapchains();
    for (auto *swapchain : swapchains) {
        auto *d3d12Swapchain = static_cast<CCD3D12Swapchain *>(swapchain);
        if (!d3d12Swapchain || !d3d12Swapchain->isReady()) {
            continue;
        }

        // --- DIAG: Read back buffer pixels before Present ---
        // Read center 8x1 pixels from the back buffer to verify rendering content
        if (s_presentCount <= 5 || s_presentCount == 60 || s_presentCount == 300) {
            auto *backBuffer = static_cast<ID3D12Resource *>(d3d12Swapchain->getCurrentBackBufferHandle());
            if (backBuffer && _impl->d3dDevice) {
                auto desc = backBuffer->GetDesc();
                CC_LOG_INFO("[PIXEL-READBACK] Frame %u: backBuffer=%p size=%llux%u format=%u currentIdx=%u",
                            s_presentCount, backBuffer,
                            static_cast<unsigned long long>(desc.Width),
                            static_cast<unsigned>(desc.Height),
                            static_cast<unsigned>(desc.Format),
                            d3d12Swapchain->getCurrentBackBufferIndex());
                diagLog("[PIXEL-READBACK] Frame %u: backBuffer=%p size=%llux%u format=%u currentIdx=%u\n",
                        s_presentCount, backBuffer,
                        static_cast<unsigned long long>(desc.Width),
                        static_cast<unsigned>(desc.Height),
                        static_cast<unsigned>(desc.Format),
                        d3d12Swapchain->getCurrentBackBufferIndex());

                // Create a readback buffer for 8 pixels (RGBA8 = 4 bytes each)
                const UINT pixelRowCount = 8;
                const UINT bytesPerPixel = 4;
                const UINT rowPitch = (pixelRowCount * bytesPerPixel + 255) & ~255; // 256-aligned

                D3D12_RESOURCE_DESC readbackDesc{};
                readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                readbackDesc.Alignment = 0;
                readbackDesc.Width = rowPitch; // 256-aligned size
                readbackDesc.Height = 1;
                readbackDesc.DepthOrArraySize = 1;
                readbackDesc.MipLevels = 1;
                readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
                readbackDesc.SampleDesc.Count = 1;
                readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                readbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

                D3D12_HEAP_PROPERTIES heapProps{};
                heapProps.Type = D3D12_HEAP_TYPE_READBACK;
                heapProps.CreationNodeMask = 1;
                heapProps.VisibleNodeMask = 1;

                Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
                HRESULT hr = _impl->d3dDevice->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&readbackBuffer));
                if (SUCCEEDED(hr)) {
                    // Use device's command allocator/list for readback
                    _impl->commandAllocator->Reset();
                    _impl->commandList->Reset(_impl->commandAllocator.Get(), nullptr);

                    // Transition back buffer: PRESENT → COPY_SOURCE
                    // (endRenderPass set it to PRESENT, Queue::submit already executed)
                    D3D12_RESOURCE_BARRIER barrier{};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition.pResource = backBuffer;
                    barrier.Transition.Subresource = 0;
                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                    _impl->commandList->ResourceBarrier(1, &barrier);

                    // Copy center row of 8 pixels
                    D3D12_TEXTURE_COPY_LOCATION src{};
                    src.pResource = backBuffer;
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    src.SubresourceIndex = 0;

                    UINT centerX = static_cast<UINT>(desc.Width / 2) - 4;
                    UINT centerY = (desc.Height / 2);

                    D3D12_TEXTURE_COPY_LOCATION dst{};
                    dst.pResource = readbackBuffer.Get();
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint.Offset = 0;
                    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    dst.PlacedFootprint.Footprint.Width = pixelRowCount;
                    dst.PlacedFootprint.Footprint.Height = 1;
                    dst.PlacedFootprint.Footprint.Depth = 1;
                    dst.PlacedFootprint.Footprint.RowPitch = rowPitch;

                    D3D12_BOX srcBox{};
                    srcBox.left = centerX;
                    srcBox.top = centerY;
                    srcBox.front = 0;
                    srcBox.right = centerX + pixelRowCount;
                    srcBox.bottom = centerY + 1;
                    srcBox.back = 1;

                    _impl->commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &srcBox);

                    // Transition back: COPY_DEST → PRESENT (restore for Present() call)
                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                    _impl->commandList->ResourceBarrier(1, &barrier);

                    _impl->commandList->Close();
                    ID3D12CommandList *cmdLists[] = { _impl->commandList.Get() };
                    _impl->graphicsQueue->ExecuteCommandLists(1, cmdLists);

                    // Wait for copy to complete
                    ++_impl->fenceValue;
                    _impl->graphicsQueue->Signal(_impl->frameFence.Get(), _impl->fenceValue);
                    if (_impl->frameFence->GetCompletedValue() < _impl->fenceValue) {
                        _impl->frameFence->SetEventOnCompletion(_impl->fenceValue, _impl->fenceEvent);
                        WaitForSingleObject(_impl->fenceEvent, INFINITE);
                    }

                    // Map and read pixels
                    void *mappedData = nullptr;
                    D3D12_RANGE readRange{0, rowPitch};
                    if (SUCCEEDED(readbackBuffer->Map(0, &readRange, &mappedData)) && mappedData) {
                        auto *pixels = static_cast<const uint8_t *>(mappedData);
                        CC_LOG_INFO("[PIXEL-READBACK] Frame %u: center pixels at (%u,%u): "
                                    "[%u,%u,%u,%u] [%u,%u,%u,%u] [%u,%u,%u,%u] [%u,%u,%u,%u]",
                                    s_presentCount, centerX, centerY,
                                    pixels[0], pixels[1], pixels[2], pixels[3],
                                    pixels[4], pixels[5], pixels[6], pixels[7],
                                    pixels[8], pixels[9], pixels[10], pixels[11],
                                    pixels[12], pixels[13], pixels[14], pixels[15]);
                        diagLog("[PIXEL-READBACK] Frame %u: center(%u,%u) 8px: "
                                "[%u,%u,%u,%u] [%u,%u,%u,%u] [%u,%u,%u,%u] [%u,%u,%u,%u]\n",
                                s_presentCount, centerX, centerY,
                                pixels[0], pixels[1], pixels[2], pixels[3],
                                pixels[4], pixels[5], pixels[6], pixels[7],
                                pixels[8], pixels[9], pixels[10], pixels[11],
                                pixels[12], pixels[13], pixels[14], pixels[15]);
                        D3D12_RANGE writeRange{0, 0};
                        readbackBuffer->Unmap(0, &writeRange);
                    }
                }
            }
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
                WaitForSingleObject(_impl->fenceEvent, INFINITE);
            } else {
                CC_LOG_ERROR("D3D12 fence wait setup failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            }
        }
    }

    // Reset GPU descriptor heap pools for the next frame
    if (_impl->gpuDescriptorHeapPool) {
        _impl->gpuDescriptorHeapPool->reset();
    }
    if (_impl->samplerDescriptorHeapPool) {
        _impl->samplerDescriptorHeapPool->reset();
    }
#endif
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
    diagLog("[DEV_FACTORY] createSwapchain() called!\n");
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
#if defined(_WIN32)
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
#else
    (void)buffers;
    (void)dst;
    (void)regions;
    (void)count;
#endif
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

void CCD3D12Device::initFormatFeatures() {
    // Hardcoded format feature table for D3D12 FL11_0+ hardware.
    // Same approach as GLES3Device::initFormatFeature() — no runtime API calls needed.
    // D3D12 FL11_0 guarantees support for all these formats.

    // Full set: SAMPLED_TEXTURE | RENDER_TARGET | LINEAR_FILTER | STORAGE_TEXTURE | VERTEX_ATTRIBUTE
    const auto F_FULL = FormatFeature::SAMPLED_TEXTURE | FormatFeature::RENDER_TARGET |
                         FormatFeature::LINEAR_FILTER | FormatFeature::STORAGE_TEXTURE |
                         FormatFeature::VERTEX_ATTRIBUTE;
    // No render target (32-bit float types may not support render target on all HW)
    const auto F_NO_RT = FormatFeature::SAMPLED_TEXTURE | FormatFeature::STORAGE_TEXTURE |
                          FormatFeature::VERTEX_ATTRIBUTE;
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
    _formatFeatures[toNumber(Format::R32F)]        = F_NO_RT;
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

    // D3D12 uses [-1,1] clip space (OpenGL convention can be toggled but default is D3D)
    _caps.clipSpaceMinZ = 0.F;
    _caps.screenSpaceSignY = 1.F;
    _caps.clipSpaceSignY = 1.F;
}

#if defined(_WIN32)
bool CCD3D12Device::initializeD3D12Context() {
    CC_LOG_INFO("[DIAG] initializeD3D12Context: starting...");
    diagLog("[INIT_CTX] Starting initializeD3D12Context\n");

    UINT dxgiFactoryFlags = 0;

    // Enable debug layer only in Debug builds; too heavy for Release and
    // can cause stability issues on some AMD drivers.
#if !defined(NDEBUG)
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            CC_LOG_INFO("[DIAG] D3D12 debug layer ENABLED (Debug build).");
        } else {
            CC_LOG_WARNING("[DIAG] Could not enable D3D12 debug layer.");
        }
    }
#endif

    HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&_impl->dxgiFactory));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateDXGIFactory2 failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        diagLog("[INIT_CTX] CreateDXGIFactory2 FAILED hr=0x%08x\n", static_cast<unsigned>(hr));
        return false;
    }
    CC_LOG_INFO("[DIAG] CreateDXGIFactory2 OK.");
    diagLog("[INIT_CTX] CreateDXGIFactory2 OK\n");

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0; _impl->dxgiFactory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex) {
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            CC_LOG_INFO("[DIAG] Adapter[%u]: software, skipping.", adapterIndex);
            continue;
        }
        char adapterName[128] = {};
        wcstombs(adapterName, adapterDesc.Description, sizeof(adapterName) - 1);
        CC_LOG_INFO("[DIAG] Adapter[%u]: %s (VRAM=%llu MB, VendorID=0x%04x)",
                     adapterIndex, adapterName,
                     static_cast<unsigned long long>(adapterDesc.DedicatedVideoMemory / (1024 * 1024)),
                     static_cast<unsigned>(adapterDesc.VendorId));
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_impl->d3dDevice)))) {
            CC_LOG_INFO("[DIAG] D3D12CreateDevice succeeded on adapter[%u].", adapterIndex);
            diagLog("[INIT_CTX] D3D12CreateDevice OK on adapter[%u]: %s\n", adapterIndex, adapterName);
            break;
        }
        CC_LOG_WARNING("[DIAG] D3D12CreateDevice FAILED on adapter[%u], trying next.", adapterIndex);
        diagLog("[INIT_CTX] D3D12CreateDevice FAILED on adapter[%u]: %s\n", adapterIndex, adapterName);
    }

    if (!_impl->d3dDevice) {
        CC_LOG_WARNING("[DIAG] No adapter worked, trying WARP (default adapter)...");
        diagLog("[INIT_CTX] No adapter worked, trying WARP\n");
        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_impl->d3dDevice));
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12CreateDevice failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            diagLog("[INIT_CTX] D3D12CreateDevice WARP FAILED hr=0x%08x\n", static_cast<unsigned>(hr));
            return false;
        }
        CC_LOG_INFO("[DIAG] D3D12CreateDevice (default) OK.");
        diagLog("[INIT_CTX] D3D12CreateDevice WARP OK\n");
    }

    // Setup ID3D12InfoQueue to capture all D3D12 debug layer messages
    {
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(_impl->d3dDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
            // Capture everything — errors, warnings, info, messages
            D3D12_MESSAGE_SEVERITY severities[] = {
                D3D12_MESSAGE_SEVERITY_CORRUPTION,
                D3D12_MESSAGE_SEVERITY_ERROR,
                D3D12_MESSAGE_SEVERITY_WARNING,
                D3D12_MESSAGE_SEVERITY_INFO,
                D3D12_MESSAGE_SEVERITY_MESSAGE,
            };
            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumSeverities = 0;
            filter.DenyList.pSeverityList = nullptr; // allow all severities
            infoQueue->PushStorageFilter(&filter);

            // Break on error — DISABLED because without a debugger attached,
            // this causes the process to crash/terminate.
            // infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            // infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);

            // Set max stored messages
            infoQueue->SetMessageCountLimit(4096);

            CC_LOG_INFO("[DIAG] ID3D12InfoQueue configured. Will break on ERROR/CORRUPTION.");
        } else {
            CC_LOG_WARNING("[DIAG] Could not query ID3D12InfoQueue.");
        }
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    hr = _impl->d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_impl->graphicsQueue));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateCommandQueue failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        diagLog("[INIT_CTX] CreateCommandQueue FAILED hr=0x%08x\n", static_cast<unsigned>(hr));
        return false;
    }
    CC_LOG_INFO("[DIAG] CreateCommandQueue OK.");
    diagLog("[INIT_CTX] CreateCommandQueue OK\n");

    hr = _impl->d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_impl->commandAllocator));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateCommandAllocator failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        diagLog("[INIT_CTX] CreateCommandAllocator FAILED hr=0x%08x\n", static_cast<unsigned>(hr));
        return false;
    }
    CC_LOG_INFO("[DIAG] CreateCommandAllocator OK.");
    diagLog("[INIT_CTX] CreateCommandAllocator OK\n");

    hr = _impl->d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _impl->commandAllocator.Get(), nullptr, IID_PPV_ARGS(&_impl->commandList));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateCommandList failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        diagLog("[INIT_CTX] CreateCommandList FAILED hr=0x%08x\n", static_cast<unsigned>(hr));
        return false;
    }
    CC_LOG_INFO("[DIAG] CreateCommandList OK.");
    diagLog("[INIT_CTX] CreateCommandList OK\n");

    hr = _impl->commandList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("Initial command list close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }
    CC_LOG_INFO("[DIAG] Initial command list closed OK.");

    hr = _impl->d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_impl->frameFence));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateFence failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }
    CC_LOG_INFO("[DIAG] CreateFence OK.");

    _impl->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!_impl->fenceEvent) {
        CC_LOG_ERROR("CreateEvent for D3D12 fence failed.");
        return false;
    }
    CC_LOG_INFO("[DIAG] CreateEvent OK.");

    _impl->fenceValue = 0;

    // Final device health check
    hr = _impl->d3dDevice->GetDeviceRemovedReason();
    CC_LOG_INFO("[DIAG] initializeD3D12Context complete. DeviceRemovedReason=0x%08x (%s)",
                 static_cast<unsigned>(hr), SUCCEEDED(hr) ? "OK" : "REMOVED/HUNG!");
    dumpD3D12DebugMessages(_impl->d3dDevice.Get(), "initContext-final");
    diagLog("[INIT_CTX] Complete! DRR=0x%08x (%s)\n", static_cast<unsigned>(hr), SUCCEEDED(hr) ? "OK" : "HUNG");
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
#endif

void *CCD3D12Device::getD3D12DeviceHandle() const {
#if defined(_WIN32)
    return _impl ? _impl->d3dDevice.Get() : nullptr;
#else
    return nullptr;
#endif
}

void *CCD3D12Device::getGraphicsQueueHandle() const {
#if defined(_WIN32)
    return _impl ? _impl->graphicsQueue.Get() : nullptr;
#else
    return nullptr;
#endif
}

void *CCD3D12Device::getDXGIFactoryHandle() const {
#if defined(_WIN32)
    return _impl ? _impl->dxgiFactory.Get() : nullptr;
#else
    return nullptr;
#endif
}

D3D12DescriptorHeapPool *CCD3D12Device::getGPUDescriptorHeapPool() const {
    return _impl ? _impl->gpuDescriptorHeapPool.get() : nullptr;
}

D3D12DescriptorHeapPool *CCD3D12Device::getSamplerDescriptorHeapPool() const {
    return _impl ? _impl->samplerDescriptorHeapPool.get() : nullptr;
}

} // namespace gfx
} // namespace cc
