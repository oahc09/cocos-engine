/****************************************************************************
 Copyright (c) 2021-2023 Xiamen Yaji Software Co., Ltd.

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

#include "D3D12Swapchain.h"
#include "D3D12Device.h"
#include "D3D12Texture.h"
#include "base/Log.h"
#include "base/Macros.h"
#include <chrono>

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <d3d12.h>
    #include <dxgi1_6.h>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

namespace {
constexpr UINT D3D12_PRESENT_SYNC_INTERVAL = 0;
constexpr UINT D3D12_PRESENT_FLAGS = 0;
constexpr uint64_t D3D12_PRESENT_DIAG_THRESHOLD_MS = 2;
} // namespace

struct CCD3D12Swapchain::Impl {
    static constexpr uint32_t BACK_BUFFER_COUNT = D3D12_MAX_FRAMES_IN_FLIGHT;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    D3D12ResourceBackingPtr backBuffers[BACK_BUFFER_COUNT];
    uint32_t currentBackBufferIndex{0};
    uint32_t rtvDescriptorSize{0};
    bool ready{false};
};

CCD3D12Swapchain::CCD3D12Swapchain() = default;
CCD3D12Swapchain::~CCD3D12Swapchain() = default;

bool CCD3D12Swapchain::isReady() const {
    return _impl ? _impl->ready : false;
}

void CCD3D12Swapchain::doInit(const SwapchainInfo &info) {
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }

    auto hwnd = reinterpret_cast<HWND>(_windowHandle);
    CC_ASSERT(hwnd != nullptr);

    _colorTexture = ccnew CCD3D12Texture;
    _depthStencilTexture = ccnew CCD3D12Texture;

    SwapchainTextureInfo textureInfo;
    textureInfo.swapchain = this;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = info.width;
    textureInfo.height = info.height;
    initTexture(textureInfo, _colorTexture);

    textureInfo.format = Format::DEPTH_STENCIL;
    initTexture(textureInfo, _depthStencilTexture);

    CC_LOG_INFO("D3D12 swapchain initialized: %ux%u.", info.width, info.height);

    _impl->ready = createOrResizeSwapchain(info.width, info.height);
    if (!_impl->ready) {
        CC_LOG_ERROR("D3D12 swapchain creation failed.");
    }
}

void CCD3D12Swapchain::doDestroy() {
    if (_impl) {
        if (auto *device = CCD3D12Device::getInstance()) {
            device->unregisterSwapchain(this);
            if (_impl->swapChain) {
                if (device->waitForGpu()) {
                    device->retireFrameResources();
                } else {
                    CC_LOG_ERROR("D3D12 swapchain teardown could not drain the GPU queue.");
                }
            }
        }
        for (auto &backBuffer : _impl->backBuffers) {
            backBuffer.reset();
        }
        _impl->rtvHeap.Reset();
        _impl->swapChain.Reset();
        _impl->ready = false;
        _impl->currentBackBufferIndex = 0;
    }

    CC_SAFE_DESTROY_NULL(_depthStencilTexture);
    CC_SAFE_DESTROY_NULL(_colorTexture);
}

void CCD3D12Swapchain::doResize(uint32_t width, uint32_t height, SurfaceTransform transform) {
    (void)transform;
    if (!_impl) {
        return;
    }
    if (_impl->ready && _colorTexture && _colorTexture->getWidth() == width && _colorTexture->getHeight() == height) {
        return;
    }

    const bool resized = createOrResizeSwapchain(width, height);
    _impl->ready = resized || (_impl->swapChain && _impl->backBuffers[0] &&
                               _impl->backBuffers[0]->resource != nullptr);
    if (!resized) {
        return;
    }

    if (_colorTexture) {
        _colorTexture->resize(width, height);
    }
    if (_depthStencilTexture) {
        _depthStencilTexture->resize(width, height);
    }
}

void CCD3D12Swapchain::doDestroySurface() {
}

void CCD3D12Swapchain::doCreateSurface(void *windowHandle) {
    (void)windowHandle;
}

void *CCD3D12Swapchain::getCurrentBackBufferHandle() const {
    if (!_impl || !_impl->ready) {
        return nullptr;
    }
    const auto &backing = _impl->backBuffers[_impl->currentBackBufferIndex];
    return backing ? backing->resource.Get() : nullptr;
}

D3D12ResourceBackingPtr CCD3D12Swapchain::getCurrentBackBufferBacking() const {
    if (!_impl || !_impl->ready) {
        return {};
    }
    return _impl->backBuffers[_impl->currentBackBufferIndex];
}

uintptr_t CCD3D12Swapchain::getCurrentRTVHandle() const {
    if (!_impl || !_impl->rtvHeap) {
        return 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = _impl->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(_impl->currentBackBufferIndex) * _impl->rtvDescriptorSize;
    return handle.ptr;
}

uint32_t CCD3D12Swapchain::getCurrentBackBufferIndex() const {
    return _impl ? _impl->currentBackBufferIndex : 0;
}

bool CCD3D12Swapchain::containsBackBuffer(void *resource) const {
    if (!_impl || !resource) {
        return false;
    }

    auto *candidate = static_cast<ID3D12Resource *>(resource);
    for (const auto &backBuffer : _impl->backBuffers) {
        if (backBuffer && backBuffer->resource.Get() == candidate) {
            return true;
        }
    }
    return false;
}

bool CCD3D12Swapchain::present() {
    if (!_impl || !_impl->ready || !_impl->swapChain) {
        return false;
    }

    const auto presentStart = std::chrono::steady_clock::now();
    HRESULT hr = _impl->swapChain->Present(D3D12_PRESENT_SYNC_INTERVAL, D3D12_PRESENT_FLAGS);
    const auto presentEnd = std::chrono::steady_clock::now();
    const auto presentMs = std::chrono::duration_cast<std::chrono::milliseconds>(presentEnd - presentStart).count();
    if (presentMs >= D3D12_PRESENT_DIAG_THRESHOLD_MS) {
        CC_LOG_INFO("[D3D12-PERF] SwapchainPresent syncInterval=%u flags=%u totalMs=%llu",
                    D3D12_PRESENT_SYNC_INTERVAL,
                    D3D12_PRESENT_FLAGS,
                    static_cast<unsigned long long>(presentMs));
    }
    if (FAILED(hr)) {
        CC_LOG_ERROR("IDXGISwapChain::Present failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    _impl->currentBackBufferIndex = _impl->swapChain->GetCurrentBackBufferIndex();
    return true;
}

bool CCD3D12Swapchain::createOrResizeSwapchain(uint32_t width, uint32_t height) {
    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    auto *graphicsQueue = static_cast<ID3D12CommandQueue *>(device ? device->getGraphicsQueueHandle() : nullptr);
    if (!device || !d3dDevice || !graphicsQueue) {
        CC_LOG_ERROR("D3D12 device context unavailable when creating swapchain.");
        return false;
    }

    auto hwnd = reinterpret_cast<HWND>(_windowHandle);
    if (!hwnd) {
        CC_LOG_ERROR("Window handle is null for D3D12 swapchain.");
        return false;
    }

    CC_LOG_INFO("D3D12 swapchain creating: %ux%u, hwnd=%p", width, height, hwnd);

    DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
    swapchainDesc.Width = width;
    swapchainDesc.Height = height;
    swapchainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapchainDesc.SampleDesc.Count = 1;
    swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchainDesc.BufferCount = Impl::BACK_BUFFER_COUNT;
    swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    HRESULT hr = S_OK;
    if (!_impl->swapChain) {
        // Use the SAME factory that the device was created with
        auto *dxgiFactory = static_cast<IDXGIFactory4 *>(device->getDXGIFactoryHandle());
        if (!dxgiFactory) {
            CC_LOG_ERROR("D3D12 DXGI factory unavailable for swapchain creation.");
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
        hr = dxgiFactory->CreateSwapChainForHwnd(
            graphicsQueue,
            hwnd,
            &swapchainDesc,
            nullptr,
            nullptr,
            &swapChain1);
        if (FAILED(hr)) {
            CC_LOG_ERROR("CreateSwapChainForHwnd failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return false;
        }

        hr = swapChain1.As(&_impl->swapChain);
        if (FAILED(hr)) {
            CC_LOG_ERROR("Query IDXGISwapChain3 failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return false;
        }
    } else {
        if (!device->waitForGpu()) {
            CC_LOG_ERROR("D3D12 swapchain resize aborted because GPU synchronization failed.");
            return false;
        }
        device->retireFrameResources();
        for (auto &backBuffer : _impl->backBuffers) {
            backBuffer.reset();
        }
        hr = _impl->swapChain->ResizeBuffers(Impl::BACK_BUFFER_COUNT, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        if (FAILED(hr)) {
            CC_LOG_ERROR("ResizeBuffers failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            if (!createRenderTargetViews()) {
                CC_LOG_ERROR("D3D12 swapchain failed to restore existing back buffers after ResizeBuffers failure.");
            } else {
                _impl->currentBackBufferIndex = _impl->swapChain->GetCurrentBackBufferIndex();
            }
            return false;
        }
    }

    if (!_impl->rtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.NumDescriptors = Impl::BACK_BUFFER_COUNT;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = d3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&_impl->rtvHeap));
        if (FAILED(hr)) {
            CC_LOG_ERROR("CreateDescriptorHeap(RTV) failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return false;
        }
        _impl->rtvDescriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    if (!createRenderTargetViews()) {
        return false;
    }

    _impl->currentBackBufferIndex = _impl->swapChain->GetCurrentBackBufferIndex();
    return true;
}

bool CCD3D12Swapchain::createRenderTargetViews() {
    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!_impl || !d3dDevice || !_impl->swapChain || !_impl->rtvHeap) {
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = _impl->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t index = 0; index < Impl::BACK_BUFFER_COUNT; ++index) {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        HRESULT hr = _impl->swapChain->GetBuffer(index, IID_PPV_ARGS(&resource));
        if (FAILED(hr)) {
            CC_LOG_ERROR("GetBuffer(%u) failed. HRESULT=0x%08x", index, static_cast<unsigned>(hr));
            return false;
        }
        auto backing = std::make_shared<D3D12ResourceBacking>();
        backing->resource = std::move(resource);
        backing->deviceEpoch = device->getDeviceEpoch();
        backing->mipLevels = 1;
        backing->arraySize = 1;
        backing->planeCount = 1;
        backing->states.reset(1, D3D12_RESOURCE_STATE_PRESENT);
        _impl->backBuffers[index] = std::move(backing);
        d3dDevice->CreateRenderTargetView(_impl->backBuffers[index]->resource.Get(), nullptr, rtvHandle);
        rtvHandle.ptr += _impl->rtvDescriptorSize;
    }

    return true;
}

} // namespace gfx
} // namespace cc
