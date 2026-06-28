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
SampleCount getD3D12EffectiveSampleCount(SampleCount samples);

class CC_DLL CCD3D12Texture final : public Texture {
public:
    CCD3D12Texture();
    ~CCD3D12Texture() override;

    void *getD3D12ResourceHandle() const;
    void *getD3D12OwnedResourceHandle() const;

    // Returns true if this texture wraps a swapchain back buffer (color attachment)
    bool isSwapchainColorTexture() const;

    static void *findLatestOwnedColorResource(uint32_t width, uint32_t height, Format format, SampleCount samples);

    // Returns the parent swapchain for swapchain textures, nullptr otherwise
    Swapchain *getSwapchain() const { return _isSwapchainTexture ? _swapchain : nullptr; }

    // D3D12 resource state tracking - used by pipelineBarrier
    D3D12_RESOURCE_STATES getCurrentState() const { return _currentState; }
    void setCurrentState(D3D12_RESOURCE_STATES state);
    static D3D12_RESOURCE_STATES getTrackedResourceState(void *resource, D3D12_RESOURCE_STATES fallback);
    static void setTrackedResourceState(void *resource, D3D12_RESOURCE_STATES state);
    static void clearTrackedResourceState(void *resource);

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
    D3D12_RESOURCE_STATES _currentState = D3D12_RESOURCE_STATE_COMMON;
    ccstd::vector<uint8_t> _baseMipUploadedLayers;
    bool _mipmapsGenerated{false};
};

} // namespace gfx
} // namespace cc
