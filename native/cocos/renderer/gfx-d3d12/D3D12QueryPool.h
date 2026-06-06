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

#include "gfx-base/GFXQueryPool.h"
#include <memory>

namespace cc {
namespace gfx {

class CC_DLL CCD3D12QueryPool final : public QueryPool {
public:
    CCD3D12QueryPool();
    ~CCD3D12QueryPool() override;

    /** Returns the D3D12 query heap as opaque pointer (ID3D12QueryHeap*). */
    void *getD3D12QueryHeap() const;

    /** Read back query results from GPU to CPU. Call from Device::getQueryPoolResults. */
    void fetchResults();

    uint32_t beginD3D12Query(uint32_t id);
    uint32_t endD3D12Query(uint32_t id);
    void resetD3D12Queries();

protected:
    void doInit(const QueryPoolInfo &info) override;
    void doDestroy() override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace gfx
} // namespace cc
