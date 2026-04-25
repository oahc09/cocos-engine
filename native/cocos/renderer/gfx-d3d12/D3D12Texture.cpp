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

#include "D3D12Texture.h"
#include "D3D12Device.h"
#include "base/Log.h"
#include "gfx-base/GFXDef.h"

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <dxgiformat.h>
    #include <wrl/client.h>
#endif

namespace cc {
namespace gfx {

struct CCD3D12Texture::Impl {
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
#endif
};

namespace {
#if defined(_WIN32)
DXGI_FORMAT toD3D12Format(Format format) {
    switch (format) {
        case Format::R8:
            return DXGI_FORMAT_R8_UNORM;
        case Format::R8UI:
            return DXGI_FORMAT_R8_UINT;
        case Format::R8I:
            return DXGI_FORMAT_R8_SINT;
        case Format::RG8:
            return DXGI_FORMAT_R8G8_UNORM;
        case Format::RG8UI:
            return DXGI_FORMAT_R8G8_UINT;
        case Format::RG8I:
            return DXGI_FORMAT_R8G8_SINT;
        case Format::RGBA8:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::BGRA8:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::SRGB8_A8:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::RGBA8UI:
            return DXGI_FORMAT_R8G8B8A8_UINT;
        case Format::RGBA8I:
            return DXGI_FORMAT_R8G8B8A8_SINT;
        case Format::R16F:
            return DXGI_FORMAT_R16_FLOAT;
        case Format::RG16F:
            return DXGI_FORMAT_R16G16_FLOAT;
        case Format::RGBA16F:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R32F:
            return DXGI_FORMAT_R32_FLOAT;
        case Format::RG32F:
            return DXGI_FORMAT_R32G32_FLOAT;
        case Format::RGBA32F:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::DEPTH:
            return DXGI_FORMAT_D32_FLOAT;
        case Format::DEPTH_STENCIL:
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

UINT toD3D12SampleCount(SampleCount samples) {
    switch (samples) {
        case SampleCount::X2:
            return 2;
        case SampleCount::X4:
            return 4;
        case SampleCount::X8:
            return 8;
        case SampleCount::X16:
            return 16;
        case SampleCount::X32:
            return 32;
        case SampleCount::X64:
            return 64;
        case SampleCount::X1:
        default:
            return 1;
    }
}
#endif
} // namespace

CCD3D12Texture::CCD3D12Texture() {
    _impl = std::make_unique<Impl>();
}

CCD3D12Texture::~CCD3D12Texture() {
    destroy();
}

void CCD3D12Texture::doInit(const TextureInfo &info) {
    (void)info;
    createResource(_info.width, _info.height);
}

void CCD3D12Texture::doInit(const TextureViewInfo &info) {
    auto *texture = static_cast<CCD3D12Texture *>(info.texture);
    if (!texture) {
        return;
    }
#if defined(_WIN32)
    _impl->resource = static_cast<ID3D12Resource *>(texture->getD3D12ResourceHandle());
#endif
}

void CCD3D12Texture::doInit(const SwapchainTextureInfo &info) {
    (void)info;
}

void CCD3D12Texture::doDestroy() {
#if defined(_WIN32)
    if (_impl) {
        _impl->resource.Reset();
    }
#endif
}

void CCD3D12Texture::doResize(uint32_t width, uint32_t height, uint32_t size) {
    (void)size;
    if (_isTextureView || _swapchain) {
        return;
    }
    createResource(width, height);
}

void *CCD3D12Texture::getD3D12ResourceHandle() const {
#if defined(_WIN32)
    return _impl ? _impl->resource.Get() : nullptr;
#else
    return nullptr;
#endif
}

bool CCD3D12Texture::createResource(uint32_t width, uint32_t height) {
#if defined(_WIN32)
    if (!_impl || width == 0 || height == 0 || _swapchain) {
        return false;
    }

    DXGI_FORMAT format = toD3D12Format(_info.format);
    if (format == DXGI_FORMAT_UNKNOWN) {
        CC_LOG_WARNING("Unsupported D3D12 texture format: %u", static_cast<unsigned>(_info.format));
        return false;
    }

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12 device unavailable when creating texture.");
        return false;
    }

    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (hasFlag(_info.usage, TextureUsageBit::COLOR_ATTACHMENT)) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (hasFlag(_info.usage, TextureUsageBit::DEPTH_STENCIL_ATTACHMENT)) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    if (hasFlag(_info.usage, TextureUsageBit::STORAGE)) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = _info.type == TextureType::TEX3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = width;
    resourceDesc.Height = height;
    resourceDesc.DepthOrArraySize = _info.type == TextureType::TEX3D ? static_cast<UINT16>(_info.depth) : static_cast<UINT16>(_info.layerCount);
    resourceDesc.MipLevels = static_cast<UINT16>(_info.levelCount);
    resourceDesc.Format = format;
    resourceDesc.SampleDesc.Count = toD3D12SampleCount(_info.samples);
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = flags;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    HRESULT hr = d3dDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateCommittedResource(texture) failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    _impl->resource = resource;
    return true;
#else
    (void)width;
    (void)height;
    return false;
#endif
}

} // namespace gfx
} // namespace cc
