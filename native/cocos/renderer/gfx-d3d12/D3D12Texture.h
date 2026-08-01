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

#pragma once

#include "D3D12ResourceState.h"
#include "gfx-base/GFXTexture.h"
#include <memory>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

namespace cc {
namespace gfx {

class Swapchain;

DXGI_FORMAT toD3D12Format(Format format);
DXGI_FORMAT toD3D12VertexFormat(Format format);
SampleCount getD3D12EffectiveSampleCount(SampleCount samples);
bool getD3D12TextureUploadFootprint(
    ID3D12Device *device,
    const D3D12_RESOURCE_DESC &textureDesc,
    const BufferTextureCopy &region,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint,
    UINT &rowCount,
    UINT64 &rowSizeInBytes,
    UINT64 &uploadSize);

class CC_DLL CCD3D12Texture final : public Texture {
public:
    CCD3D12Texture();
    ~CCD3D12Texture() override;

    void *getD3D12ResourceHandle() const;
    void *getD3D12OwnedResourceHandle() const;
    D3D12ResourceBackingPtr getD3D12ResourceBacking() const;
    static uint64_t getD3D12GlobalResourceGeneration();

    // Returns true if this texture wraps a swapchain back buffer (color attachment)
    bool isSwapchainColorTexture() const;

    // Returns the parent swapchain for swapchain textures, nullptr otherwise
    Swapchain *getSwapchain() const { return _isSwapchainTexture ? _swapchain : nullptr; }

    D3D12_RESOURCE_STATES getCurrentState() const;
    void setCurrentState(D3D12_RESOURCE_STATES state);
    D3D12_RESOURCE_STATES getSubresourceState(uint32_t subresource) const;
    void setSubresourceState(uint32_t subresource, D3D12_RESOURCE_STATES state);

    void markBaseMipLayerUploaded(uint32_t mipLevel, uint32_t baseLayer, uint32_t layerCount);
    bool shouldGenerateMipmapsAfterUpload() const;
    void markMipmapsGenerated();

protected:
    void doInit(const TextureInfo &info) override;
    void doInit(const TextureViewInfo &info) override;
    void doInit(const SwapchainTextureInfo &info) override;
    void doDestroy() override;
    void doResize(uint32_t width, uint32_t height, uint32_t size) override;

private:
    bool createResource(uint32_t width, uint32_t height);

    struct Impl;
    std::unique_ptr<Impl> _impl;

    bool _isSwapchainTexture{false};
    ccstd::vector<uint8_t> _baseMipUploadedLayers;
    bool _mipmapsGenerated{false};
};

} // namespace gfx
} // namespace cc
