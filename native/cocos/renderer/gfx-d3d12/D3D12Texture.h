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

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#endif

namespace cc {
namespace gfx {

class Swapchain;

class CC_DLL CCD3D12Texture final : public Texture {
public:
    CCD3D12Texture();
    ~CCD3D12Texture() override;

    void *getD3D12ResourceHandle() const;

    // Returns true if this texture wraps a swapchain back buffer (color attachment)
    bool isSwapchainColorTexture() const;

    // Returns the parent swapchain for swapchain textures, nullptr otherwise
    Swapchain *getSwapchain() const { return _swapchain; }

    // D3D12 resource state tracking — used by pipelineBarrier
#if defined(_WIN32)
    D3D12_RESOURCE_STATES getCurrentState() const { return _currentState; }
    void setCurrentState(D3D12_RESOURCE_STATES state) { _currentState = state; }
#endif

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

#if defined(_WIN32)
    D3D12_RESOURCE_STATES _currentState = D3D12_RESOURCE_STATE_COMMON;
#endif
};

} // namespace gfx
} // namespace cc
