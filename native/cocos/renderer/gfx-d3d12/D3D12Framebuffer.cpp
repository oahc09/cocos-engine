/****************************************************************************
 Copyright (c) 2020-2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
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

#include "D3D12Framebuffer.h"
#include "D3D12Device.h"
#include "D3D12Swapchain.h"
#include "D3D12Texture.h"
#include "D3D12RenderPass.h"
#include "base/Log.h"
#include "base/Ptr.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <dxgiformat.h>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

struct CCD3D12Framebuffer::Impl {
    // RTV descriptor heap (one heap for all render targets)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    // DSV descriptor heap
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;

    // Store CPU descriptor handles
    ccstd::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    ccstd::vector<IntrusivePtr<CCD3D12Texture>> repairedColorTextures;

    uint32_t width{0};
    uint32_t height{0};
    uint32_t rtvDescriptorSize{0};
};

CCD3D12Framebuffer::CCD3D12Framebuffer() {
    _impl = std::make_unique<Impl>();
}

CCD3D12Framebuffer::~CCD3D12Framebuffer() {
    destroy();
}

void CCD3D12Framebuffer::doInit(const FramebufferInfo &info) {
    (void)info;
    if (!_impl) return;
    _swapchain = nullptr;
    _isOffscreen = true;

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12Framebuffer: device unavailable.");
        return;
    }

    const uint32_t colorCount = static_cast<uint32_t>(_colorTextures.size());

    // Determine dimensions from first color texture or depth texture
    Texture *sizeRef = colorCount > 0 ? _colorTextures[0] : _depthStencilTexture;
    if (sizeRef) {
        _impl->width = sizeRef->getWidth();
        _impl->height = sizeRef->getHeight();
    }

    // Create RTV descriptor heap and render target views
    if (colorCount > 0) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = colorCount;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        rtvHeapDesc.NodeMask = 0;

        HRESULT hr = d3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&_impl->rtvHeap));
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12Framebuffer: CreateDescriptorHeap(RTV) failed. HRESULT=0x%08x",
                         static_cast<unsigned>(hr));
            return;
        }

        _impl->rtvDescriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        _impl->rtvHandles.resize(colorCount);
        _impl->repairedColorTextures.resize(colorCount);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = _impl->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < colorCount; ++i) {
            auto handleSlot = rtvHandle;
            rtvHandle.ptr += _impl->rtvDescriptorSize;

            auto *texture = static_cast<CCD3D12Texture *>(_colorTextures[i]);
            if (!texture) continue;

            if (texture->isSwapchainColorTexture() && _depthStencilTexture) {
                auto *depthTexture = static_cast<CCD3D12Texture *>(_depthStencilTexture);
                const bool mixedOffscreenDepth = depthTexture &&
                                                 !depthTexture->isSwapchainColorTexture() &&
                                                 depthTexture->getD3D12OwnedResourceHandle() != nullptr &&
                                                 (depthTexture->getWidth() != texture->getWidth() ||
                                                  depthTexture->getHeight() != texture->getHeight());
                if (mixedOffscreenDepth) {
                    if (auto *replacement = CCD3D12Texture::findCompatibleOwnedColorTexture(
                            depthTexture->getWidth(), depthTexture->getHeight(), texture->getFormat())) {
                        CC_LOG_WARNING("D3D12Framebuffer: mixed swapchain color/offscreen depth repaired. "
                                       "color[%u] swapchain %ux%u replaced with owned RT %p %ux%u.",
                                       i, texture->getWidth(), texture->getHeight(), replacement,
                                       replacement->getWidth(), replacement->getHeight());
                        _impl->repairedColorTextures[i] = replacement;
                        texture = replacement;
                        _colorTextures[i] = replacement;
                    } else {
                        CC_LOG_WARNING("D3D12Framebuffer: mixed swapchain color/offscreen depth detected. "
                                       "color[%u]=%ux%u depth=%ux%u format=%u, but no unique owned RT was found.",
                                       i, texture->getWidth(), texture->getHeight(),
                                       depthTexture->getWidth(), depthTexture->getHeight(),
                                       static_cast<unsigned>(texture->getFormat()));
                    }
                }
            }

            // Detect swapchain color textures — their RTVs are managed by the swapchain
            if (texture->isSwapchainColorTexture()) {
                _swapchain = static_cast<CCD3D12Swapchain *>(texture->getSwapchain());
                _isOffscreen = false;
                // Leave placeholder handle; getRTVHandle() returns swapchain's RTV dynamically
                _impl->rtvHandles[i] = D3D12_CPU_DESCRIPTOR_HANDLE{};
                continue;
            }

            auto *resource = static_cast<ID3D12Resource *>(texture->getD3D12OwnedResourceHandle());
            if (!resource) {
                CC_LOG_WARNING("D3D12Framebuffer: color texture %u has no D3D12 resource.", i);
                continue;
            }

            d3dDevice->CreateRenderTargetView(resource, nullptr, handleSlot);
            _impl->rtvHandles[i] = handleSlot;
        }
    }

    // Create DSV descriptor heap and depth-stencil view
    if (_depthStencilTexture) {
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        dsvHeapDesc.NodeMask = 0;

        HRESULT hr = d3dDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&_impl->dsvHeap));
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12Framebuffer: CreateDescriptorHeap(DSV) failed. HRESULT=0x%08x",
                         static_cast<unsigned>(hr));
            return;
        }

        auto *depthTexture = static_cast<CCD3D12Texture *>(_depthStencilTexture);
        if (depthTexture) {
            auto *resource = static_cast<ID3D12Resource *>(depthTexture->getD3D12ResourceHandle());
            if (resource) {
                _impl->dsvHandle = _impl->dsvHeap->GetCPUDescriptorHandleForHeapStart();
                d3dDevice->CreateDepthStencilView(resource, nullptr, _impl->dsvHandle);
            } else {
                CC_LOG_WARNING("D3D12Framebuffer: depth texture has no D3D12 resource.");
            }
        }
    }

    CC_LOG_INFO("D3D12Framebuffer initialized: %u color attachments, size=%ux%u, swapchain=%s",
                colorCount, _impl->width, _impl->height, _swapchain ? "yes" : "no");
}

void CCD3D12Framebuffer::doDestroy() {
    if (_impl) {
        _impl->rtvHeap.Reset();
        _impl->dsvHeap.Reset();
        _impl->rtvHandles.clear();
        _impl->repairedColorTextures.clear();
        _impl->dsvHandle = D3D12_CPU_DESCRIPTOR_HANDLE{};
        _impl->width = 0;
        _impl->height = 0;
        _impl->rtvDescriptorSize = 0;
    }
}

CCD3D12Framebuffer::DescriptorPair CCD3D12Framebuffer::getRTVHandle(uint32_t index) const {
    if (!_impl) return {};
    if (index >= _impl->rtvHandles.size()) return {};

    // Check if THIS specific color attachment is a swapchain texture.
    // Previously we checked only _swapchain (which is set if ANY attachment
    // is a swapchain texture), causing non-swapchain attachments in MRT
    // to incorrectly get the swapchain RTV handle.
    if (index < static_cast<uint32_t>(_colorTextures.size())) {
        auto *texture = static_cast<CCD3D12Texture *>(_colorTextures[index]);
        if (texture && texture->isSwapchainColorTexture()) {
            auto *sw = static_cast<CCD3D12Swapchain *>(texture->getSwapchain());
            if (sw) {
                uintptr_t rtvPtr = sw->getCurrentRTVHandle();
                return {static_cast<uint64_t>(rtvPtr), 0};
            }
        }
    }

    const auto &handle = _impl->rtvHandles[index];
    return {static_cast<uint64_t>(handle.ptr), 0};
}

CCD3D12Framebuffer::DescriptorPair CCD3D12Framebuffer::getDSVHandle() const {
    if (!_impl) return {};
    return {static_cast<uint64_t>(_impl->dsvHandle.ptr), 0};
}

uint32_t CCD3D12Framebuffer::getWidth() const {
    return _impl ? _impl->width : 0;
}

uint32_t CCD3D12Framebuffer::getHeight() const {
    return _impl ? _impl->height : 0;
}

CCD3D12Swapchain *CCD3D12Framebuffer::getSwapchain() const {
    return _isOffscreen ? nullptr : _swapchain;
}

bool CCD3D12Framebuffer::isOffscreen() const {
    return _isOffscreen;
}

} // namespace gfx
} // namespace cc
