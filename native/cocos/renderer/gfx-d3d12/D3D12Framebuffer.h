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

#pragma once

#include "D3D12ResourceState.h"
#include "gfx-base/GFXFramebuffer.h"
#include <cstdint>
#include <memory>

namespace cc {
namespace gfx {

class CCD3D12Swapchain;

struct D3D12FramebufferAttachment {
    D3D12ResourceBackingPtr backing;
    Format format{Format::UNKNOWN};
    TextureType type{TextureType::TEX2D};
    SampleCount samples{SampleCount::X1};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t baseMip{0};
    uint32_t mipCount{1};
    uint32_t baseLayer{0};
    uint32_t layerCount{1};
    uint32_t basePlane{0};
    uint32_t planeCount{1};
    uintptr_t resourceId{0};
    uint64_t backingGeneration{0};
    bool isSwapchain{false};
};

class CC_DLL CCD3D12Framebuffer final : public Framebuffer {
public:
    CCD3D12Framebuffer();
    ~CCD3D12Framebuffer() override;

    // Returns D3D12 CPU descriptor handle for RTV as {ptr, value} pair
    struct DescriptorPair {
        uint64_t ptr{0};
        uint32_t value{0};
    };

    DescriptorPair getRTVHandle(uint32_t index) const;
    DescriptorPair getDSVHandle() const;
    uint32_t getWidth() const;
    uint32_t getHeight() const;
    uint32_t getColorTextureCount() const;
    const D3D12FramebufferAttachment *getColorAttachment(uint32_t index) const;
    const D3D12FramebufferAttachment *getDepthStencilAttachment() const;
    void *getColorResource(uint32_t index) const;
    void *getDepthStencilResource() const;
    D3D12ResourceBackingPtr getColorBacking(uint32_t index) const;
    D3D12ResourceBackingPtr getDepthStencilBacking() const;
    bool hasColorTextureState(uint32_t index) const;
    bool isValid() const;
    CCD3D12Swapchain *getSwapchain() const;
    bool isOffscreen() const;

protected:
    void doInit(const FramebufferInfo &info) override;
    void doDestroy() override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    CCD3D12Swapchain *_swapchain{nullptr};
    bool _isOffscreen{true};
};

} // namespace gfx
} // namespace cc
