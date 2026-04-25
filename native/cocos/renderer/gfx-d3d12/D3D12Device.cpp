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

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
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
bool gLoggedPresentSubmitted = false;
}

struct CCD3D12Device::Impl {
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<IDXGIFactory6> dxgiFactory;
    Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> graphicsQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> frameFence;

    HANDLE fenceEvent{nullptr};
    uint64_t fenceValue{0};
#endif
};

CCD3D12Device *CCD3D12Device::getInstance() {
    return CCD3D12Device::instance;
}

CCD3D12Device::CCD3D12Device() {
    CCD3D12Device::instance = this;
    _api = API::D3D12;
    _deviceName = "D3D12";
    _impl = std::make_unique<Impl>();
}

CCD3D12Device::~CCD3D12Device() {
    CCD3D12Device::instance = nullptr;
}

bool CCD3D12Device::doInit(const DeviceInfo &info) {
    (void)info;

#if defined(_WIN32)
    if (!initializeD3D12Context()) {
        CC_LOG_ERROR("Failed to initialize D3D12 context.");
        return false;
    }
#endif

    QueueInfo queueInfo;
    queueInfo.type = QueueType::GRAPHICS;
    _queue = createQueue(queueInfo);

    QueryPoolInfo queryPoolInfo{QueryType::OCCLUSION, DEFAULT_MAX_QUERY_OBJECTS, true};
    _queryPool = createQueryPool(queryPoolInfo);

    CommandBufferInfo cmdBuffInfo;
    cmdBuffInfo.type = CommandBufferType::PRIMARY;
    cmdBuffInfo.queue = _queue;
    _cmdBuff = createCommandBuffer(cmdBuffInfo);

    _renderer = "D3D12";
    _vendor = "Unknown";

    CC_LOG_INFO("D3D12 device initialized.");
    CC_LOG_INFO("RENDERER: %s", _renderer.c_str());
    CC_LOG_INFO("VENDOR: %s", _vendor.c_str());
    return true;
}

void CCD3D12Device::doDestroy() {
#if defined(_WIN32)
    waitForGpu();
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
    if (!_impl->graphicsQueue || !_impl->commandAllocator || !_impl->commandList || !_impl->frameFence || !_impl->fenceEvent) {
        return;
    }

    const auto &swapchains = getSwapchains();
    for (auto *swapchain : swapchains) {
        auto *d3d12Swapchain = static_cast<CCD3D12Swapchain *>(swapchain);
        if (!d3d12Swapchain || !d3d12Swapchain->isReady()) {
            continue;
        }

        HRESULT hr = _impl->commandAllocator->Reset();
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12 command allocator reset failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            continue;
        }

        hr = _impl->commandList->Reset(_impl->commandAllocator.Get(), nullptr);
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12 command list reset failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            continue;
        }

        auto *backBuffer = static_cast<ID3D12Resource *>(d3d12Swapchain->getCurrentBackBufferHandle());
        if (!backBuffer) {
            continue;
        }

        D3D12_RESOURCE_BARRIER toRenderTarget{};
        toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRenderTarget.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        toRenderTarget.Transition.pResource = backBuffer;
        toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        _impl->commandList->ResourceBarrier(1, &toRenderTarget);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        rtv.ptr = d3d12Swapchain->getCurrentRTVHandle();
        if (rtv.ptr == 0) {
            continue;
        }
        _impl->commandList->ClearRenderTargetView(rtv, D3D12_POC_CLEAR_COLOR, 0, nullptr);

        D3D12_RESOURCE_BARRIER toPresent{};
        toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toPresent.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        toPresent.Transition.pResource = backBuffer;
        toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        _impl->commandList->ResourceBarrier(1, &toPresent);

        hr = _impl->commandList->Close();
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12 command list close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            continue;
        }

        ID3D12CommandList *commandLists[] = {_impl->commandList.Get()};
        _impl->graphicsQueue->ExecuteCommandLists(1, commandLists);

        if (!d3d12Swapchain->present()) {
            continue;
        }

        ++_impl->fenceValue;
        hr = _impl->graphicsQueue->Signal(_impl->frameFence.Get(), _impl->fenceValue);
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

    if (!gLoggedPresentSubmitted) {
        gLoggedPresentSubmitted = true;
        CC_LOG_INFO("D3D12 present submitted.");
    }
#else
    if (!gLoggedPresentSubmitted) {
        gLoggedPresentSubmitted = true;
        CC_LOG_INFO("D3D12 present submitted.");
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
    (void)buffers;
    (void)dst;
    (void)regions;
    (void)count;
}

void CCD3D12Device::copyTextureToBuffers(Texture *src, uint8_t *const *buffers, const BufferTextureCopy *region, uint32_t count) {
    (void)src;
    (void)buffers;
    (void)region;
    (void)count;
}

void CCD3D12Device::getQueryPoolResults(QueryPool *queryPool) {
    (void)queryPool;
}

#if defined(_WIN32)
bool CCD3D12Device::initializeD3D12Context() {
    UINT dxgiFactoryFlags = 0;

#if !defined(NDEBUG)
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&_impl->dxgiFactory));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateDXGIFactory2 failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0; _impl->dxgiFactory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex) {
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_impl->d3dDevice)))) {
            break;
        }
    }

    if (!_impl->d3dDevice) {
        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_impl->d3dDevice));
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12CreateDevice failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return false;
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

} // namespace gfx
} // namespace cc
