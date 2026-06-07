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

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <dxgiformat.h>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

namespace {

DXGI_FORMAT toD3D12DSVFormat(Format format) {
    switch (format) {
        case Format::DEPTH:
            return DXGI_FORMAT_D32_FLOAT;
        case Format::DEPTH_STENCIL:
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        default:
            return DXGI_FORMAT_D32_FLOAT;
    }
}

D3D12_DEPTH_STENCIL_VIEW_DESC makeDepthStencilViewDesc(const CCD3D12Texture *texture) {
    D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
    const auto &info = texture->getInfo();
    const auto &viewInfo = texture->getViewInfo();
    const bool isView = texture->isTextureView();
    const Format format = isView ? viewInfo.format : info.format;
    const TextureType type = isView ? viewInfo.type : info.type;
    const uint32_t baseLevel = isView ? viewInfo.baseLevel : 0;
    const uint32_t baseLayer = isView ? viewInfo.baseLayer : 0;
    uint32_t layerCount = isView ? viewInfo.layerCount : info.layerCount;
    if (layerCount == 0) {
        layerCount = 1;
    }
    const bool isArrayView = info.layerCount > 1 || baseLayer > 0 ||
                             layerCount > 1 || type == TextureType::CUBE;
    const bool isMS = info.samples != SampleCount::X1;

    desc.Format = toD3D12DSVFormat(format);
    desc.Flags = D3D12_DSV_FLAG_NONE;
    if (isMS) {
        if (isArrayView) {
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            desc.Texture2DMSArray.FirstArraySlice = baseLayer;
            desc.Texture2DMSArray.ArraySize = layerCount;
        } else {
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        }
    } else if (isArrayView) {
        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.MipSlice = baseLevel;
        desc.Texture2DArray.FirstArraySlice = baseLayer;
        desc.Texture2DArray.ArraySize = layerCount;
    } else {
        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = baseLevel;
    }

    return desc;
}

} // namespace

struct CCD3D12Framebuffer::Impl {
    // RTV descriptor heap (one heap for all render targets)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    // DSV descriptor heap
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;

    // Store CPU descriptor handles
    ccstd::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    ccstd::vector<CCD3D12Texture *> colorTextures;
    ccstd::vector<bool> colorHasTextureState;
    ccstd::vector<bool> colorRepairsFromOwnedResource;
    ccstd::vector<uint32_t> colorRepairWidths;
    ccstd::vector<uint32_t> colorRepairHeights;
    ccstd::vector<Format> colorRepairFormats;
    ccstd::vector<SampleCount> colorRepairSamples;
    CCD3D12Texture *depthStencilTexture{nullptr};
    ccstd::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> colorResources;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencilResource;

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
    _impl->rtvHeap.Reset();
    _impl->dsvHeap.Reset();
    _impl->rtvHandles.clear();
    _impl->dsvHandle = D3D12_CPU_DESCRIPTOR_HANDLE{};
    _impl->colorTextures.clear();
    _impl->colorHasTextureState.clear();
    _impl->colorRepairsFromOwnedResource.clear();
    _impl->colorRepairWidths.clear();
    _impl->colorRepairHeights.clear();
    _impl->colorRepairFormats.clear();
    _impl->colorRepairSamples.clear();
    _impl->colorResources.clear();
    _impl->depthStencilTexture = nullptr;
    _impl->depthStencilResource.Reset();
    _impl->width = 0;
    _impl->height = 0;
    _impl->rtvDescriptorSize = 0;

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12Framebuffer: device unavailable.");
        return;
    }

    const uint32_t colorCount = static_cast<uint32_t>(_colorTextures.size());
    _impl->colorTextures.resize(colorCount);
    _impl->colorHasTextureState.assign(colorCount, false);
    _impl->colorRepairsFromOwnedResource.assign(colorCount, false);
    _impl->colorRepairWidths.assign(colorCount, 0);
    _impl->colorRepairHeights.assign(colorCount, 0);
    _impl->colorRepairFormats.assign(colorCount, Format::UNKNOWN);
    _impl->colorRepairSamples.assign(colorCount, SampleCount::X1);
    _impl->colorResources.resize(colorCount);
    for (uint32_t i = 0; i < colorCount; ++i) {
        _impl->colorTextures[i] = static_cast<CCD3D12Texture *>(_colorTextures[i]);
    }
    _impl->depthStencilTexture = static_cast<CCD3D12Texture *>(_depthStencilTexture);

    // Determine dimensions from first color texture or depth texture
    Texture *sizeRef = colorCount > 0 ? _impl->colorTextures[0] : _impl->depthStencilTexture;
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

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = _impl->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < colorCount; ++i) {
            auto handleSlot = rtvHandle;
            rtvHandle.ptr += _impl->rtvDescriptorSize;

            auto *texture = _impl->colorTextures[i];
            if (!texture) continue;

            if (texture->isSwapchainColorTexture() && _impl->depthStencilTexture) {
                auto *depthTexture = _impl->depthStencilTexture;
                const bool mixedOffscreenDepth = depthTexture &&
                                                 !depthTexture->isSwapchainColorTexture() &&
                                                 depthTexture->getD3D12OwnedResourceHandle() != nullptr &&
                                                 (depthTexture->getWidth() != texture->getWidth() ||
                                                  depthTexture->getHeight() != texture->getHeight());
                if (mixedOffscreenDepth) {
                    auto *replacementResource = static_cast<ID3D12Resource *>(CCD3D12Texture::findLatestOwnedColorResource(
                        depthTexture->getWidth(), depthTexture->getHeight(), texture->getFormat(),
                        depthTexture->getInfo().samples));
                    if (replacementResource) {
                        CC_LOG_WARNING("D3D12Framebuffer: mixed swapchain color/offscreen depth repaired with owned RT. "
                                       "color[%u] swapchain %ux%u replaced with owned RT resource %p %ux%u.",
                                       i, texture->getWidth(), texture->getHeight(), replacementResource,
                                       depthTexture->getWidth(), depthTexture->getHeight());
                        _impl->colorTextures[i] = nullptr;
                        _impl->colorRepairsFromOwnedResource[i] = true;
                        _impl->colorRepairWidths[i] = depthTexture->getWidth();
                        _impl->colorRepairHeights[i] = depthTexture->getHeight();
                        _impl->colorRepairFormats[i] = texture->getFormat();
                        _impl->colorRepairSamples[i] = depthTexture->getInfo().samples;
                        _impl->colorResources[i] = replacementResource;
                        d3dDevice->CreateRenderTargetView(replacementResource, nullptr, handleSlot);
                        _impl->rtvHandles[i] = handleSlot;
                        if (i == 0) {
                            _impl->width = depthTexture->getWidth();
                            _impl->height = depthTexture->getHeight();
                        }
                        continue;
                    } else {
                        CC_LOG_WARNING("D3D12Framebuffer: mixed swapchain color/offscreen depth detected. "
                                        "color[%u]=%ux%u depth=%ux%u format=%u, but no owned color RT resource was found.",
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

            _impl->colorResources[i] = resource;
            _impl->colorHasTextureState[i] = true;
            d3dDevice->CreateRenderTargetView(resource, nullptr, handleSlot);
            _impl->rtvHandles[i] = handleSlot;
        }
    }

    // Create DSV descriptor heap and depth-stencil view
    if (_impl->depthStencilTexture) {
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

        auto *depthTexture = _impl->depthStencilTexture;
        if (depthTexture) {
            auto *resource = static_cast<ID3D12Resource *>(depthTexture->getD3D12ResourceHandle());
            if (resource) {
                _impl->depthStencilResource = resource;
                _impl->dsvHandle = _impl->dsvHeap->GetCPUDescriptorHandleForHeapStart();
                const auto dsvDesc = makeDepthStencilViewDesc(depthTexture);
                d3dDevice->CreateDepthStencilView(resource, &dsvDesc, _impl->dsvHandle);
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
        _impl->colorTextures.clear();
        _impl->colorHasTextureState.clear();
        _impl->colorRepairsFromOwnedResource.clear();
        _impl->colorRepairWidths.clear();
        _impl->colorRepairHeights.clear();
        _impl->colorRepairFormats.clear();
        _impl->colorRepairSamples.clear();
        _impl->colorResources.clear();
        _impl->depthStencilTexture = nullptr;
        _impl->depthStencilResource.Reset();
        _impl->dsvHandle = D3D12_CPU_DESCRIPTOR_HANDLE{};
        _impl->width = 0;
        _impl->height = 0;
        _impl->rtvDescriptorSize = 0;
    }
}

void CCD3D12Framebuffer::refreshRepairedColorResource(uint32_t index) const {
    if (!_impl || index >= _impl->colorRepairsFromOwnedResource.size() ||
        !_impl->colorRepairsFromOwnedResource[index]) {
        return;
    }

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice || index >= _impl->rtvHandles.size() || _impl->rtvHandles[index].ptr == 0) {
        return;
    }

    auto *latestResource = static_cast<ID3D12Resource *>(CCD3D12Texture::findLatestOwnedColorResource(
        _impl->colorRepairWidths[index],
        _impl->colorRepairHeights[index],
        _impl->colorRepairFormats[index],
        _impl->colorRepairSamples[index]));
    if (!latestResource) {
        return;
    }

    auto *currentResource = _impl->colorResources[index].Get();
    if (currentResource == latestResource) {
        return;
    }

    _impl->colorResources[index] = latestResource;
    d3dDevice->CreateRenderTargetView(latestResource, nullptr, _impl->rtvHandles[index]);
    CC_LOG_WARNING("D3D12Framebuffer: refreshed repaired offscreen color[%u] to latest owned RT resource %p.",
                   index, latestResource);
}

CCD3D12Framebuffer::DescriptorPair CCD3D12Framebuffer::getRTVHandle(uint32_t index) const {
    if (!_impl) return {};
    if (index >= _impl->rtvHandles.size()) return {};

    refreshRepairedColorResource(index);

    if (index < _impl->colorTextures.size()) {
        auto *texture = _impl->colorTextures[index];
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

uint32_t CCD3D12Framebuffer::getColorTextureCount() const {
    return _impl ? static_cast<uint32_t>(_impl->colorTextures.size()) : 0;
}

CCD3D12Texture *CCD3D12Framebuffer::getColorTexture(uint32_t index) const {
    if (!_impl || index >= _impl->colorTextures.size()) return nullptr;
    return _impl->colorTextures[index];
}

CCD3D12Texture *CCD3D12Framebuffer::getDepthStencilTexture() const {
    return _impl ? _impl->depthStencilTexture : nullptr;
}

void *CCD3D12Framebuffer::getColorResource(uint32_t index) const {
    if (!_impl || index >= _impl->colorResources.size()) return nullptr;
    refreshRepairedColorResource(index);
    return _impl->colorResources[index].Get();
}

void *CCD3D12Framebuffer::getDepthStencilResource() const {
    return _impl ? _impl->depthStencilResource.Get() : nullptr;
}

bool CCD3D12Framebuffer::hasColorTextureState(uint32_t index) const {
    return _impl && index < _impl->colorHasTextureState.size() && _impl->colorHasTextureState[index];
}

CCD3D12Swapchain *CCD3D12Framebuffer::getSwapchain() const {
    return _isOffscreen ? nullptr : _swapchain;
}

bool CCD3D12Framebuffer::isOffscreen() const {
    return _isOffscreen;
}

} // namespace gfx
} // namespace cc
