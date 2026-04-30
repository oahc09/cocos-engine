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
#include "D3D12Swapchain.h"
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
        case Format::RGB32F:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::RGBA32F:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::R16UI:
            return DXGI_FORMAT_R16_UINT;
        case Format::RG16UI:
            return DXGI_FORMAT_R16G16_UINT;
        case Format::RGBA16UI:
            return DXGI_FORMAT_R16G16B16A16_UINT;
        case Format::R32UI:
            return DXGI_FORMAT_R32_UINT;
        case Format::RG32UI:
            return DXGI_FORMAT_R32G32_UINT;
        case Format::RGBA32UI:
            return DXGI_FORMAT_R32G32B32A32_UINT;
        case Format::R16I:
            return DXGI_FORMAT_R16_SINT;
        case Format::R32I:
            return DXGI_FORMAT_R32_SINT;
        case Format::RG16I:
            return DXGI_FORMAT_R16G16_SINT;
        case Format::RGBA16I:
            return DXGI_FORMAT_R16G16B16A16_SINT;
        case Format::RG32I:
            return DXGI_FORMAT_R32G32_SINT;
        case Format::RGB32I:
            return DXGI_FORMAT_R32G32B32_SINT;
        case Format::RGBA32I:
            return DXGI_FORMAT_R32G32B32A32_SINT;
        case Format::RGB10A2:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case Format::RGB10A2UI:
            return DXGI_FORMAT_R10G10B10A2_UINT;
        case Format::R11G11B10F:
            return DXGI_FORMAT_R11G11B10_FLOAT;
        case Format::DEPTH:
            return DXGI_FORMAT_D32_FLOAT;
        case Format::DEPTH_STENCIL:
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case Format::R5G6B5:
            return DXGI_FORMAT_B5G6R5_UNORM;
        case Format::RGBA4:
            return DXGI_FORMAT_B4G4R4A4_UNORM;
        case Format::RGB5A1:
            return DXGI_FORMAT_B5G5R5A1_UNORM;
        case Format::RGB9E5:
            return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        // BC compressed formats
        case Format::BC1:
        case Format::BC1_ALPHA:
            return DXGI_FORMAT_BC1_UNORM;
        case Format::BC1_SRGB:
        case Format::BC1_SRGB_ALPHA:
            return DXGI_FORMAT_BC1_UNORM_SRGB;
        case Format::BC2:
            return DXGI_FORMAT_BC2_UNORM;
        case Format::BC2_SRGB:
            return DXGI_FORMAT_BC2_UNORM_SRGB;
        case Format::BC3:
            return DXGI_FORMAT_BC3_UNORM;
        case Format::BC3_SRGB:
            return DXGI_FORMAT_BC3_UNORM_SRGB;
        case Format::BC4:
            return DXGI_FORMAT_BC4_UNORM;
        case Format::BC4_SNORM:
            return DXGI_FORMAT_BC4_SNORM;
        case Format::BC5:
            return DXGI_FORMAT_BC5_UNORM;
        case Format::BC5_SNORM:
            return DXGI_FORMAT_BC5_SNORM;
        case Format::BC6H_UF16:
            return DXGI_FORMAT_BC6H_UF16;
        case Format::BC6H_SF16:
            return DXGI_FORMAT_BC6H_SF16;
        case Format::BC7:
            return DXGI_FORMAT_BC7_UNORM;
        case Format::BC7_SRGB:
            return DXGI_FORMAT_BC7_UNORM_SRGB;
        // ETC2 - D3D12 has no native support, use fallback
        case Format::ETC_RGB8:
        case Format::ETC2_RGB8:
        case Format::ETC2_SRGB8:
        case Format::ETC2_RGB8_A1:
        case Format::ETC2_SRGB8_A1:
        case Format::ETC2_RGBA8:
        case Format::ETC2_SRGB8_A8:
        case Format::EAC_R11:
        case Format::EAC_R11SN:
        case Format::EAC_RG11:
        case Format::EAC_RG11SN:
            // D3D12 does not support ETC2 natively - fallback to RGBA8
            CC_LOG_WARNING("D3D12: ETC2 format %u not supported natively, falling back to RGBA8.", static_cast<unsigned>(format));
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::ASTC_RGBA_4X4:
        case Format::ASTC_RGBA_5X4:
        case Format::ASTC_RGBA_5X5:
        case Format::ASTC_RGBA_6X5:
        case Format::ASTC_RGBA_6X6:
        case Format::ASTC_RGBA_8X5:
        case Format::ASTC_RGBA_8X6:
        case Format::ASTC_RGBA_8X8:
        case Format::ASTC_RGBA_10X5:
        case Format::ASTC_RGBA_10X6:
        case Format::ASTC_RGBA_10X8:
        case Format::ASTC_RGBA_10X10:
        case Format::ASTC_RGBA_12X10:
        case Format::ASTC_RGBA_12X12:
        case Format::ASTC_SRGBA_4X4:
        case Format::ASTC_SRGBA_5X5:
        case Format::ASTC_SRGBA_6X6:
        case Format::ASTC_SRGBA_8X6:
        case Format::ASTC_SRGBA_8X8:
        case Format::ASTC_SRGBA_10X5:
        case Format::ASTC_SRGBA_10X10:
        case Format::ASTC_SRGBA_12X12:
            CC_LOG_WARNING("D3D12: ASTC format %u not supported natively, falling back to RGBA8.", static_cast<unsigned>(format));
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        default:
            CC_LOG_WARNING("D3D12: Unmapped texture format %u, returning UNKNOWN.", static_cast<unsigned>(format));
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
#if defined(_WIN32)
    // createResource() uses CreateCommittedResource(..., D3D12_RESOURCE_STATE_COMMON, ...),
    // so tracked state must start from COMMON until an explicit barrier changes it.
    _currentState = D3D12_RESOURCE_STATE_COMMON;
#endif
}

void CCD3D12Texture::doInit(const TextureViewInfo &info) {
    auto *texture = static_cast<CCD3D12Texture *>(info.texture);
    if (!texture) {
        return;
    }
#if defined(_WIN32)
    _impl->resource = static_cast<ID3D12Resource *>(texture->getD3D12ResourceHandle());
    _currentState = texture->getCurrentState();
#endif
}

void CCD3D12Texture::doInit(const SwapchainTextureInfo &info) {
    (void)info;
#if defined(_WIN32)
    if (hasFlag(_info.usage, TextureUsageBit::DEPTH_STENCIL_ATTACHMENT)) {
        createResource(_info.width, _info.height);
        // Depth resource is created in COMMON and transitioned on first use.
        _currentState = D3D12_RESOURCE_STATE_COMMON;
    } else if (hasFlag(_info.usage, TextureUsageBit::COLOR_ATTACHMENT)) {
        // Swapchain color textures start as PRESENT, will be transitioned by beginRenderPass
        _currentState = D3D12_RESOURCE_STATE_PRESENT;
    }
#endif
    // Swapchain color textures wrap the swapchain's back buffers;
    // the resource is obtained dynamically via getD3D12ResourceHandle()
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
    // For swapchain color textures, dynamically return the current back buffer
    if (isSwapchainColorTexture()) {
        return static_cast<CCD3D12Swapchain *>(_swapchain)->getCurrentBackBufferHandle();
    }
    return _impl ? _impl->resource.Get() : nullptr;
#else
    return nullptr;
#endif
}

bool CCD3D12Texture::isSwapchainColorTexture() const {
    return _swapchain != nullptr && !hasFlag(_info.usage, TextureUsageBit::DEPTH_STENCIL_ATTACHMENT);
}

bool CCD3D12Texture::createResource(uint32_t width, uint32_t height) {
#if defined(_WIN32)
    if (!_impl || width == 0 || height == 0 || isSwapchainColorTexture()) {
        return false;
    }

    // --- DIAG: Pre-flight device health check ---
    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12 device unavailable when creating texture.");
        return false;
    }

    {
        HRESULT drr = d3dDevice->GetDeviceRemovedReason();
        if (FAILED(drr)) {
            CC_LOG_ERROR("[DIAG] createResource PRE-FLIGHT: device already HUNG! "
                         "DeviceRemovedReason=0x%08x, format=%u, %ux%u, usage=0x%x. "
                         "The device hung BEFORE this texture creation.",
                         static_cast<unsigned>(drr),
                         static_cast<unsigned>(_info.format), width, height,
                         static_cast<uint32_t>(_info.usage));
            // Dump all queued D3D12 debug messages to find the real cause
            Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
            if (SUCCEEDED(d3dDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
                UINT64 msgCount = infoQueue->GetNumStoredMessages();
                CC_LOG_ERROR("[DIAG-INFOQUEUE] Messages queued at HUNG detection: %llu",
                             static_cast<unsigned long long>(msgCount));
                for (UINT64 i = 0; i < msgCount && i < 50; ++i) {
                    SIZE_T msgSize = 0;
                    infoQueue->GetMessage(i, nullptr, &msgSize);
                    if (msgSize == 0) continue;
                    auto *msgData = static_cast<D3D12_MESSAGE *>(malloc(msgSize));
                    if (!msgData) continue;
                    if (SUCCEEDED(infoQueue->GetMessage(i, msgData, &msgSize))) {
                        CC_LOG_ERROR("[DIAG-INFOQUEUE]   [%u] %.*s",
                                     static_cast<unsigned>(msgData->ID),
                                     static_cast<int>(msgData->DescriptionByteLength),
                                     msgData->pDescription);
                    }
                    free(msgData);
                }
            }
            return false;
        }
    }

    DXGI_FORMAT format = toD3D12Format(_info.format);
    if (format == DXGI_FORMAT_UNKNOWN) {
        CC_LOG_WARNING("Unsupported D3D12 texture format: %u", static_cast<unsigned>(_info.format));
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
        // Log device removed reason for diagnosis
        HRESULT removedReason = d3dDevice->GetDeviceRemovedReason();
        CC_LOG_ERROR("[DIAG] CreateCommittedResource(texture) FAILED. "
                     "HRESULT=0x%08x, DeviceRemovedReason=0x%08x, "
                     "format=%u (DXGI=%u), %ux%u, depth=%u, layers=%u, mips=%u, usage=0x%x, flags=0x%x, samples=%u",
                     static_cast<unsigned>(hr), static_cast<unsigned>(removedReason),
                     static_cast<unsigned>(_info.format), static_cast<unsigned>(format),
                     width, height,
                     static_cast<unsigned>(_info.depth),
                     static_cast<unsigned>(_info.layerCount),
                     static_cast<unsigned>(_info.levelCount),
                     static_cast<uint32_t>(_info.usage),
                     static_cast<unsigned>(flags),
                     toD3D12SampleCount(_info.samples));
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
