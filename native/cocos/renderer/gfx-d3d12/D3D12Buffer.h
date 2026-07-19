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

#include "gfx-base/GFXBuffer.h"
#include <memory>
#include <d3d12.h>
#include <wrl/client.h>

namespace cc {
namespace gfx {

class CCD3D12CommandBuffer;

class CC_DLL CCD3D12Buffer final : public Buffer {
public:
    CCD3D12Buffer();
    ~CCD3D12Buffer() override;

    void update(const void *buffer, uint32_t size) override;

    void *getD3D12ResourceHandle() const;
    uint64_t getD3D12GPUVirtualAddress() const;
    uint64_t getD3D12ResourceVersion() const;
    uint64_t getD3D12UniformGPUVirtualAddress() const;
    uint64_t getUniformDescriptorVersion() const;
    uint32_t getD3D12ConstantBufferSize() const;
    uint32_t getPendingTransientUniformUploadSize() const;
    bool flushTransientUniformUpload(void *resource, void *mappedData,
                                     uint64_t gpuAddress, uint64_t epoch);
    bool ensureTransientUniformUpload();
    uint32_t getD3D12ResourceOffset() const;
    bool isD3D12UploadHeap() const;
    void markUniformDescriptorBinding(bool dynamic);
    bool isDynamicUniformOnly() const;
    D3D12_RESOURCE_STATES getCurrentState() const;
    void setCurrentState(D3D12_RESOURCE_STATES state);
    void flushPendingUpdate(CCD3D12CommandBuffer *commandBuffer);

protected:
    void doInit(const BufferInfo &info) override;
    void doInit(const BufferViewInfo &info) override;
    void doResize(uint32_t size, uint32_t count) override;
    void doDestroy() override;

private:
    bool createResource(uint32_t size);
    bool isTransientUniformEligible() const;
    bool canUseTransientUniformUpload() const;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace gfx
} // namespace cc
