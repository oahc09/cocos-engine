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

#include "gfx-base/GFXPipelineState.h"
#include <memory>

namespace cc {
namespace gfx {

class CC_DLL CCD3D12PipelineState final : public PipelineState {
public:
    CCD3D12PipelineState();
    ~CCD3D12PipelineState() override;

    // Returns the D3D12 PSO as opaque pointer. Caller should cast to ID3D12PipelineState*.
    void *getID3D12PipelineState() const;

    // Returns the D3D12_PRIMITIVE_TOPOLOGY for command list IA setup
    uint32_t getD3D12PrimitiveTopology() const;

    // Returns the actual root signature used to create this PSO.
    void *getID3D12RootSignature() const;

    // True when the PSO root signature matches the PipelineLayout root signature.
    bool usesPipelineLayoutRootSignature() const;

    // True when this PSO is using the built-in diagnostic fallback shader.
    bool isDiagnosticFallback() const;

protected:
    void doInit(const PipelineStateInfo &info) override;
    void doDestroy() override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace gfx
} // namespace cc
