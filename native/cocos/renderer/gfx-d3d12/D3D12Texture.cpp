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
#include "D3D12MemAlloc.h"
#include "D3D12Swapchain.h"
#include "base/Log.h"
#include "gfx-base/GFXDef.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <dxgiformat.h>
    #include <wrl/client.h>
    #include <algorithm>
    #include <atomic>
    #include <unordered_map>

namespace cc {
namespace gfx {

namespace {
std::atomic<uint64_t> TEXTURE_RESOURCE_GENERATION{1};
}

SampleCount getD3D12EffectiveSampleCount(SampleCount samples) {
    // The built-in pipeline requests X4 by default. D3D12 uses X2 as its
    // backend default while preserving explicit non-default sample counts.
    return samples == SampleCount::X4 ? SampleCount::X2 : samples;
}

struct CCD3D12Texture::Impl {
    D3D12ResourceBackingPtr backing;
};

namespace {
DXGI_FORMAT mapD3D12Format(Format format) {
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
        // ETC2/EAC are not supported by D3D12 and require transcoding before
        // texture creation. Treating compressed bytes as RGBA8 corrupts data.
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
            return DXGI_FORMAT_UNKNOWN;
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
            return DXGI_FORMAT_UNKNOWN;
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
} // namespace

DXGI_FORMAT toD3D12Format(Format format) {
    return mapD3D12Format(format);
}

bool getD3D12TextureUploadFootprint(
    ID3D12Device *device,
    const D3D12_RESOURCE_DESC &textureDesc,
    const BufferTextureCopy &region,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint,
    UINT &rowCount,
    UINT64 &rowSizeInBytes,
    UINT64 &uploadSize) {
    footprint = {};
    rowCount = 0;
    rowSizeInBytes = 0;
    uploadSize = 0;
    if (!device || textureDesc.SampleDesc.Count != 1 ||
        region.texExtent.width == 0 || region.texExtent.height == 0 ||
        region.texExtent.depth == 0) {
        return false;
    }

    D3D12_RESOURCE_DESC uploadDesc = textureDesc;
    uploadDesc.Alignment = 0;
    uploadDesc.Width = region.texExtent.width;
    uploadDesc.Height = region.texExtent.height;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.SampleDesc.Quality = 0;
    switch (uploadDesc.Dimension) {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            uploadDesc.Height = 1;
            uploadDesc.DepthOrArraySize = 1;
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            uploadDesc.DepthOrArraySize = 1;
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            uploadDesc.DepthOrArraySize = static_cast<UINT16>(region.texExtent.depth);
            break;
        default:
            return false;
    }

    device->GetCopyableFootprints(&uploadDesc, 0, 1, 0, &footprint, &rowCount, &rowSizeInBytes, &uploadSize);
    return uploadSize > 0 && rowCount > 0;
}

DXGI_FORMAT toD3D12VertexFormat(Format format) {
    switch (format) {
        case Format::R8:         return DXGI_FORMAT_R8_UNORM;
        case Format::R8SN:       return DXGI_FORMAT_R8_SNORM;
        case Format::R8UI:       return DXGI_FORMAT_R8_UINT;
        case Format::R8I:        return DXGI_FORMAT_R8_SINT;
        case Format::R16F:       return DXGI_FORMAT_R16_FLOAT;
        case Format::R16UI:      return DXGI_FORMAT_R16_UINT;
        case Format::R16I:       return DXGI_FORMAT_R16_SINT;
        case Format::R32F:       return DXGI_FORMAT_R32_FLOAT;
        case Format::R32UI:      return DXGI_FORMAT_R32_UINT;
        case Format::R32I:       return DXGI_FORMAT_R32_SINT;
        case Format::RG8:        return DXGI_FORMAT_R8G8_UNORM;
        case Format::RG8SN:      return DXGI_FORMAT_R8G8_SNORM;
        case Format::RG8UI:      return DXGI_FORMAT_R8G8_UINT;
        case Format::RG8I:       return DXGI_FORMAT_R8G8_SINT;
        case Format::RG16F:      return DXGI_FORMAT_R16G16_FLOAT;
        case Format::RG16UI:     return DXGI_FORMAT_R16G16_UINT;
        case Format::RG16I:      return DXGI_FORMAT_R16G16_SINT;
        case Format::RG32F:      return DXGI_FORMAT_R32G32_FLOAT;
        case Format::RG32UI:     return DXGI_FORMAT_R32G32_UINT;
        case Format::RG32I:      return DXGI_FORMAT_R32G32_SINT;
        case Format::RGB32F:     return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::RGB32UI:    return DXGI_FORMAT_R32G32B32_UINT;
        case Format::RGB32I:     return DXGI_FORMAT_R32G32B32_SINT;
        case Format::RGBA8:      return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::BGRA8:      return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::SRGB8_A8:   return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::RGBA8SN:    return DXGI_FORMAT_R8G8B8A8_SNORM;
        case Format::RGBA8UI:    return DXGI_FORMAT_R8G8B8A8_UINT;
        case Format::RGBA8I:     return DXGI_FORMAT_R8G8B8A8_SINT;
        case Format::RGBA16F:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::RGBA16UI:   return DXGI_FORMAT_R16G16B16A16_UINT;
        case Format::RGBA16I:    return DXGI_FORMAT_R16G16B16A16_SINT;
        case Format::RGBA32F:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::RGBA32UI:   return DXGI_FORMAT_R32G32B32A32_UINT;
        case Format::RGBA32I:    return DXGI_FORMAT_R32G32B32A32_SINT;
        case Format::RGB10A2:    return DXGI_FORMAT_R10G10B10A2_UNORM;
        case Format::RGB10A2UI:  return DXGI_FORMAT_R10G10B10A2_UINT;
        case Format::R11G11B10F: return DXGI_FORMAT_R11G11B10_FLOAT;
        case Format::RGB8:
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

CCD3D12Texture::CCD3D12Texture() {
    _impl = std::make_unique<Impl>();
}

CCD3D12Texture::~CCD3D12Texture() {
    destroy();
}

void CCD3D12Texture::doInit(const TextureInfo &info) {
    (void)info;
    _baseMipUploadedLayers.clear();
    _mipmapsGenerated = false;
    _isSwapchainTexture = false;
    _swapchain = nullptr;
    _isTextureView = false;
    _info.samples = getD3D12EffectiveSampleCount(_info.samples);
    _hash = Texture::computeHash(this);
    if (!createResource(_info.width, _info.height)) {
        CC_LOG_ERROR("D3D12Texture: createResource failed for format=%u, %ux%u, usage=0x%x. "
                     "RTV/SRV creation will fail downstream.",
                     static_cast<unsigned>(_info.format), _info.width, _info.height,
                     static_cast<uint32_t>(_info.usage));
        return;
    }
    // createResource() uses CreateCommittedResource(..., D3D12_RESOURCE_STATE_COMMON, ...),
    // so tracked state must start from COMMON until an explicit barrier changes it.
}

void CCD3D12Texture::doInit(const TextureViewInfo &info) {
    _baseMipUploadedLayers.clear();
    _mipmapsGenerated = false;
    auto *texture = static_cast<CCD3D12Texture *>(info.texture);
    if (!texture) {
        return;
    }
    _isSwapchainTexture = texture->isSwapchainColorTexture();
    _swapchain = _isSwapchainTexture ? texture->getSwapchain() : nullptr;
    _hash = Texture::computeHash(this);
    _impl->backing = _isSwapchainTexture
                         ? D3D12ResourceBackingPtr{}
                         : texture->getD3D12ResourceBacking();
}

void CCD3D12Texture::doInit(const SwapchainTextureInfo &info) {
    (void)info;
    _baseMipUploadedLayers.clear();
    _mipmapsGenerated = false;
    _isTextureView = false;
    _isSwapchainTexture = hasFlag(_info.usage, TextureUsageBit::COLOR_ATTACHMENT) &&
                          !hasFlag(_info.usage, TextureUsageBit::DEPTH_STENCIL_ATTACHMENT);
    _hash = Texture::computeHash(this);
    if (hasFlag(_info.usage, TextureUsageBit::DEPTH_STENCIL_ATTACHMENT)) {
        createResource(_info.width, _info.height);
    }
    // Swapchain color textures wrap the swapchain's back buffers;
    // the resource is obtained dynamically via getD3D12ResourceHandle()
}

void CCD3D12Texture::doDestroy() {
    if (auto *device = CCD3D12Device::getInstance()) {
        device->discardDeferredCubeUploadsForTexture(this);
    }
    _baseMipUploadedLayers.clear();
    _mipmapsGenerated = false;
    if (_impl) {
        // Drop the backing shared_ptr. If no other shared_ptr references
        // remain (views, command buffers), D3D12ResourceBacking's destructor
        // releases resource first, then d3d12maAllocation — the correct
        // D3D12MA teardown order enforced by reverse declaration order.
        _impl->backing.reset();
    }
    _isSwapchainTexture = false;
    _swapchain = nullptr;
}

void CCD3D12Texture::doResize(uint32_t width, uint32_t height, uint32_t size) {
    (void)size;
    if (_isTextureView || _isSwapchainTexture) {
        return;
    }
    if (auto *device = CCD3D12Device::getInstance()) {
        device->discardDeferredCubeUploadsForTexture(this);
    }
    _baseMipUploadedLayers.clear();
    _mipmapsGenerated = false;
    createResource(width, height);
}

void *CCD3D12Texture::getD3D12ResourceHandle() const {
    // For swapchain color textures, dynamically return the current back buffer
    if (isSwapchainColorTexture()) {
        return static_cast<CCD3D12Swapchain *>(_swapchain)->getCurrentBackBufferHandle();
    }
    const auto backing = getD3D12ResourceBacking();
    return backing ? backing->resource.Get() : nullptr;
}

void *CCD3D12Texture::getD3D12OwnedResourceHandle() const {
    return _impl && _impl->backing && _impl->backing->valid
               ? _impl->backing->resource.Get()
               : nullptr;
}

D3D12ResourceBackingPtr CCD3D12Texture::getD3D12ResourceBacking() const {
    if (isSwapchainColorTexture()) {
        return static_cast<CCD3D12Swapchain *>(_swapchain)->getCurrentBackBufferBacking();
    }
    return _impl && _impl->backing && _impl->backing->valid
               ? _impl->backing
               : D3D12ResourceBackingPtr{};
}

uint64_t CCD3D12Texture::getD3D12GlobalResourceGeneration() {
    return TEXTURE_RESOURCE_GENERATION.load(std::memory_order_relaxed);
}

bool CCD3D12Texture::isSwapchainColorTexture() const {
    return _isSwapchainTexture && _swapchain != nullptr &&
           !hasFlag(_info.usage, TextureUsageBit::DEPTH_STENCIL_ATTACHMENT);
}

D3D12_RESOURCE_STATES CCD3D12Texture::getCurrentState() const {
    const auto backing = getD3D12ResourceBacking();
    if (!backing) {
        return isSwapchainColorTexture() ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;
    }
    D3D12_RESOURCE_STATES uniformState{};
    if (backing->states.tryGetUniform(uniformState)) {
        return uniformState;
    }
    const uint32_t mip = _isTextureView ? _viewInfo.baseLevel : 0;
    const uint32_t layer = _isTextureView && _info.type != TextureType::TEX3D ? _viewInfo.baseLayer : 0;
    const uint32_t plane = _isTextureView ? _viewInfo.basePlane : 0;
    return backing->states.get(backing->subresourceIndex(mip, layer, plane));
}

void CCD3D12Texture::setCurrentState(D3D12_RESOURCE_STATES state) {
    const auto backing = getD3D12ResourceBacking();
    if (!backing) {
        return;
    }
    if (!_isTextureView) {
        backing->states.setAll(state);
        return;
    }

    const uint32_t baseMip = std::min(_viewInfo.baseLevel, backing->mipLevels - 1);
    const uint32_t mipCount = std::min(std::max(_viewInfo.levelCount, 1U), backing->mipLevels - baseMip);
    const uint32_t baseLayer = std::min(_viewInfo.baseLayer, backing->arraySize - 1);
    const uint32_t layerCount = std::min(std::max(_viewInfo.layerCount, 1U), backing->arraySize - baseLayer);
    const uint32_t basePlane = std::min(_viewInfo.basePlane, backing->planeCount - 1);
    const uint32_t planeCount = std::min(std::max(_viewInfo.planeCount, 1U), backing->planeCount - basePlane);
    for (uint32_t plane = 0; plane < planeCount; ++plane) {
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            for (uint32_t mip = 0; mip < mipCount; ++mip) {
                backing->states.set(
                    backing->subresourceIndex(baseMip + mip, baseLayer + layer, basePlane + plane), state);
            }
        }
    }
}

D3D12_RESOURCE_STATES CCD3D12Texture::getSubresourceState(uint32_t subresource) const {
    const auto backing = getD3D12ResourceBacking();
    return backing ? backing->states.get(subresource) : D3D12_RESOURCE_STATE_COMMON;
}

void CCD3D12Texture::setSubresourceState(uint32_t subresource, D3D12_RESOURCE_STATES state) {
    const auto backing = getD3D12ResourceBacking();
    if (backing) {
        backing->states.set(subresource, state);
    }
}

void CCD3D12Texture::markBaseMipLayerUploaded(uint32_t mipLevel, uint32_t baseLayer, uint32_t layerCount) {
    if (!hasFlag(_info.flags, TextureFlagBit::GEN_MIPMAP) ||
        _info.levelCount <= 1 || _info.type == TextureType::TEX3D || mipLevel != 0) {
        return;
    }

    const bool deferUntilAllCubeFacesUploaded = _info.type == TextureType::CUBE && _info.layerCount > 1;
    const uint32_t trackedLayers = deferUntilAllCubeFacesUploaded ? std::max<uint32_t>(_info.layerCount, 1) : 1;
    if (_baseMipUploadedLayers.size() != trackedLayers) {
        _baseMipUploadedLayers.assign(trackedLayers, 0);
    }

    if (!deferUntilAllCubeFacesUploaded) {
        _baseMipUploadedLayers[0] = 1;
    } else {
        const uint32_t safeLayerCount = std::max<uint32_t>(layerCount, 1);
        const uint32_t endLayer = std::min<uint32_t>(baseLayer + safeLayerCount, trackedLayers);
        for (uint32_t layer = baseLayer; layer < endLayer; ++layer) {
            _baseMipUploadedLayers[layer] = 1;
        }
    }
    _mipmapsGenerated = false;
}

bool CCD3D12Texture::shouldGenerateMipmapsAfterUpload() const {
    if (!hasFlag(_info.flags, TextureFlagBit::GEN_MIPMAP) ||
        _info.levelCount <= 1 || _info.type == TextureType::TEX3D || _mipmapsGenerated) {
        return false;
    }

    const bool deferUntilAllCubeFacesUploaded = _info.type == TextureType::CUBE && _info.layerCount > 1;
    const uint32_t trackedLayers = deferUntilAllCubeFacesUploaded ? std::max<uint32_t>(_info.layerCount, 1) : 1;
    if (_baseMipUploadedLayers.size() != trackedLayers) {
        return false;
    }

    for (uint8_t uploaded : _baseMipUploadedLayers) {
        if (uploaded == 0) {
            return false;
        }
    }
    return true;
}

void CCD3D12Texture::markMipmapsGenerated() {
    _mipmapsGenerated = true;
}

bool CCD3D12Texture::createResource(uint32_t width, uint32_t height) {
    if (!_impl || width == 0 || height == 0 || isSwapchainColorTexture()) {
        return false;
    }

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12 device unavailable when creating texture.");
        return false;
    }

    const DXGI_FORMAT viewFormat = toD3D12Format(_info.format);
    if (viewFormat == DXGI_FORMAT_UNKNOWN) {
        CC_LOG_WARNING("Unsupported D3D12 texture format: %u", static_cast<unsigned>(_info.format));
        return false;
    }
    DXGI_FORMAT resourceFormat = viewFormat;
    if (_info.format == Format::DEPTH) {
        resourceFormat = DXGI_FORMAT_R32_TYPELESS;
    } else if (_info.format == Format::DEPTH_STENCIL) {
        resourceFormat = DXGI_FORMAT_R24G8_TYPELESS;
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
    if (hasFlag(_info.flags, TextureFlagBit::GEN_MIPMAP) &&
        _info.samples == SampleCount::X1 &&
        _info.type != TextureType::TEX3D) {
        const auto &formatInfo = GFX_FORMAT_INFOS[toNumber(_info.format)];
        D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport{viewFormat};
        const bool supportsRenderTarget =
            !formatInfo.hasDepth && !formatInfo.hasStencil && !formatInfo.isCompressed &&
            formatInfo.type != FormatType::UINT && formatInfo.type != FormatType::INT &&
            SUCCEEDED(d3dDevice->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport))) &&
            (formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0 &&
            (formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
        if (supportsRenderTarget) {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        } else {
            CC_LOG_WARNING("D3D12 texture format %u cannot generate mipmaps on GPU.",
                           static_cast<unsigned>(_info.format));
        }
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
    resourceDesc.Format = resourceFormat;
    resourceDesc.SampleDesc.Count = toD3D12SampleCount(_info.samples);
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = flags;

    D3D12_CLEAR_VALUE clearValue{};
    D3D12_CLEAR_VALUE *optimizedClearValue = nullptr;
    if (hasFlag(_info.usage, TextureUsageBit::DEPTH_STENCIL_ATTACHMENT)) {
        clearValue.Format = viewFormat;
        clearValue.DepthStencil.Depth = 1.0F;
        clearValue.DepthStencil.Stencil = 0;
        optimizedClearValue = &clearValue;
    } else if (hasFlag(_info.usage, TextureUsageBit::COLOR_ATTACHMENT)) {
        clearValue.Format = viewFormat;
        clearValue.Color[0] = 0.0F;
        clearValue.Color[1] = 0.0F;
        clearValue.Color[2] = 0.0F;
        clearValue.Color[3] = 0.0F;
        optimizedClearValue = &clearValue;
    }

    // Create the new resource before replacing the old backing. The old
    // backing shared_ptr stays alive until all consumers (views, command
    // buffers) release their references — this naturally defers the old
    // D3D12MA allocation release until no GPU work references it.
    Microsoft::WRL::ComPtr<D3D12MA::Allocation> allocation;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    auto *allocator = device->getMemoryAllocator();
    HRESULT hr = E_FAIL;
    if (allocator) {
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        allocDesc.ExtraHeapFlags = D3D12_HEAP_FLAG_NONE;
        hr = allocator->CreateResource(&allocDesc, &resourceDesc, D3D12_RESOURCE_STATE_COMMON,
                                       optimizedClearValue, allocation.ReleaseAndGetAddressOf(),
                                       IID_PPV_ARGS(&resource));
        if (FAILED(hr) || !allocation) {
            resource.Reset();
            allocation.Reset();
        }
    }
    if (!allocation) {
        hr = d3dDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COMMON,
            optimizedClearValue,
            IID_PPV_ARGS(&resource));
        if (FAILED(hr)) {
            // Log device removed reason for diagnosis
            HRESULT removedReason = d3dDevice->GetDeviceRemovedReason();
            CC_LOG_ERROR("D3D12 CreateCommittedResource(texture) failed. "
                         "HRESULT=0x%08x, DeviceRemovedReason=0x%08x, "
                         "format=%u (DXGI=%u), %ux%u, depth=%u, layers=%u, mips=%u, usage=0x%x, flags=0x%x, samples=%u",
                         static_cast<unsigned>(hr), static_cast<unsigned>(removedReason),
                         static_cast<unsigned>(_info.format), static_cast<unsigned>(resourceFormat),
                         width, height,
                         static_cast<unsigned>(_info.depth),
                         static_cast<unsigned>(_info.layerCount),
                         static_cast<unsigned>(_info.levelCount),
                         static_cast<uint32_t>(_info.usage),
                         static_cast<unsigned>(flags),
                         toD3D12SampleCount(_info.samples));
            return false;
        }
    }

    const uint32_t mipLevels = std::max(_info.levelCount, 1U);
    const uint32_t arraySize = _info.type == TextureType::TEX3D ? 1U : std::max(_info.layerCount, 1U);
    const uint32_t planeCount = _info.format == Format::DEPTH_STENCIL ? 2U : 1U;
    auto newBacking = std::make_shared<D3D12ResourceBacking>();
    const auto oldBacking = _impl->backing;
    newBacking->resource = std::move(resource);
    newBacking->d3d12maAllocation = std::move(allocation);
    newBacking->deviceEpoch = device->getDeviceEpoch();
    newBacking->generation = oldBacking ? oldBacking->generation + 1 : 1;
    newBacking->mipLevels = mipLevels;
    newBacking->arraySize = arraySize;
    newBacking->planeCount = planeCount;
    newBacking->valid = true;
    newBacking->states.reset(
        newBacking->subresourceCount(), D3D12_RESOURCE_STATE_COMMON);
    if (oldBacking) {
        oldBacking->valid = false;
    }
    _impl->backing = std::move(newBacking);
    TEXTURE_RESOURCE_GENERATION.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace gfx
} // namespace cc
