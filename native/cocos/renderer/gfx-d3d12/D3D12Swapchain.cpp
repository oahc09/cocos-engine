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

struct CCD3D12Swapchain::Impl {
#if defined(_WIN32)
    static constexpr uint32_t BACK_BUFFER_COUNT = 2;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffers[BACK_BUFFER_COUNT];
    uint32_t currentBackBufferIndex{0};
    uint32_t rtvDescriptorSize{0};
    bool ready{false};
#endif
};

CCD3D12Swapchain::CCD3D12Swapchain() = default;
CCD3D12Swapchain::~CCD3D12Swapchain() = default;

bool CCD3D12Swapchain::isReady() const {
#if defined(_WIN32)
    return _impl ? _impl->ready : false;
#else
    return false;
#endif
}

void CCD3D12Swapchain::doInit(const SwapchainInfo &info) {
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }

#if defined(_WIN32)
    auto hwnd = reinterpret_cast<HWND>(_windowHandle);
    CC_ASSERT(hwnd != nullptr);
#else
    CC_ASSERT(_windowHandle != nullptr);
#endif

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

#if defined(_WIN32)
    _impl->ready = createOrResizeSwapchain(info.width, info.height);
    if (!_impl->ready) {
        CC_LOG_ERROR("D3D12 swapchain creation failed.");
    }
#endif
}

void CCD3D12Swapchain::doDestroy() {
#if defined(_WIN32)
    if (_impl) {
        for (auto &backBuffer : _impl->backBuffers) {
            backBuffer.Reset();
        }
        _impl->rtvHeap.Reset();
        _impl->swapChain.Reset();
        _impl->ready = false;
        _impl->currentBackBufferIndex = 0;
    }
#endif

    CC_SAFE_DESTROY_NULL(_depthStencilTexture);
    CC_SAFE_DESTROY_NULL(_colorTexture);
}

void CCD3D12Swapchain::doResize(uint32_t width, uint32_t height, SurfaceTransform transform) {
    (void)transform;
    if (_colorTexture) {
        _colorTexture->resize(width, height);
    }
    if (_depthStencilTexture) {
        _depthStencilTexture->resize(width, height);
    }
#if defined(_WIN32)
    if (_impl) {
        _impl->ready = createOrResizeSwapchain(width, height);
    }
#endif
}

void CCD3D12Swapchain::doDestroySurface() {
}

void CCD3D12Swapchain::doCreateSurface(void *windowHandle) {
    (void)windowHandle;
}

#if defined(_WIN32)
void *CCD3D12Swapchain::getCurrentBackBufferHandle() const {
    if (!_impl || !_impl->ready) {
        return nullptr;
    }
    return _impl->backBuffers[_impl->currentBackBufferIndex].Get();
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
#if defined(_WIN32)
    return _impl ? _impl->currentBackBufferIndex : 0;
#else
    return 0;
#endif
}

bool CCD3D12Swapchain::present() {
    if (!_impl || !_impl->ready || !_impl->swapChain) {
        return false;
    }

    // Test both syncInterval values:
    // - syncInterval=1: VSync, guaranteed to display, but may black-screen with sync submit
    // - syncInterval=0: Immediate, no VSync, but content may not display on some drivers
    // Try syncInterval=1 first — if the rendering pipeline is correct (which pixel readback
    // confirms), VSync present should show the content.
    const UINT syncInterval = 1;
    HRESULT hr = _impl->swapChain->Present(syncInterval, 0);
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
        for (auto &backBuffer : _impl->backBuffers) {
            backBuffer.Reset();
        }
        hr = _impl->swapChain->ResizeBuffers(Impl::BACK_BUFFER_COUNT, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        if (FAILED(hr)) {
            CC_LOG_ERROR("ResizeBuffers failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
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
        HRESULT hr = _impl->swapChain->GetBuffer(index, IID_PPV_ARGS(&_impl->backBuffers[index]));
        if (FAILED(hr)) {
            CC_LOG_ERROR("GetBuffer(%u) failed. HRESULT=0x%08x", index, static_cast<unsigned>(hr));
            return false;
        }
        d3dDevice->CreateRenderTargetView(_impl->backBuffers[index].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += _impl->rtvDescriptorSize;
    }

    return true;
}
#else
void *CCD3D12Swapchain::getCurrentBackBufferHandle() const {
    return nullptr;
}

uintptr_t CCD3D12Swapchain::getCurrentRTVHandle() const {
    return 0;
}

bool CCD3D12Swapchain::present() {
    return false;
}
#endif

} // namespace gfx
} // namespace cc
