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

#include "gfx-base/GFXInputAssembler.h"
#include <memory>

namespace cc {
namespace gfx {

class CC_DLL CCD3D12InputAssembler final : public InputAssembler {
public:
    CCD3D12InputAssembler();
    ~CCD3D12InputAssembler() override;

    // Returns the D3D12_INPUT_ELEMENT_DESC array as void* (caller casts to D3D12_INPUT_ELEMENT_DESC*)
    void *getInputElementDescs() const;
    uint32_t getInputElementDescCount() const;

    // Returns D3D12_VERTEX_BUFFER_VIEW info as flat arrays
    // gpuAddresses/sizeInBytes/strideInBytes arrays, count = num vertex buffers
    uint32_t getVertexBufferCount() const;
    // Rebuild cached views only when a backing D3D12 resource was replaced.
    bool refreshBufferViews();
    void fillVertexBufferViews(void *views) const; // fills D3D12_VERTEX_BUFFER_VIEW array

    // Returns index buffer view info (or nullptr if no index buffer)
    bool hasIndexBuffer() const;
    void fillIndexBufferView(void *view) const; // fills D3D12_INDEX_BUFFER_VIEW

    // Returns DXGI_FORMAT for the index buffer
    uint32_t getIndexFormat() const;

protected:
    void doInit(const InputAssemblerInfo &info) override;
    void doDestroy() override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace gfx
} // namespace cc
