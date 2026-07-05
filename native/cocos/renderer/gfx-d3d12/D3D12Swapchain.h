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

#pragma once

#include "gfx-base/GFXSwapchain.h"
#include <cstdint>
#include <memory>

namespace cc {
namespace gfx {

class CCD3D12Device;

class CC_DLL CCD3D12Swapchain final : public Swapchain {
public:
    CCD3D12Swapchain();
    ~CCD3D12Swapchain() override;

    bool isReady() const;
    void *getCurrentBackBufferHandle() const;
    uintptr_t getCurrentRTVHandle() const;
    uint32_t getCurrentBackBufferIndex() const;
    bool containsBackBuffer(void *resource) const;
    bool present();

protected:
    void doInit(const SwapchainInfo &info) override;
    void doDestroy() override;
    void doResize(uint32_t width, uint32_t height, SurfaceTransform transform) override;
    void doDestroySurface() override;
    void doCreateSurface(void *windowHandle) override;

    bool createOrResizeSwapchain(uint32_t width, uint32_t height);
    bool createRenderTargetViews();

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace gfx
} // namespace cc
