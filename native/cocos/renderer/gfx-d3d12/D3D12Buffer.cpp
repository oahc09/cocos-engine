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

#include "D3D12Buffer.h"
#include "D3D12Device.h"
#include "base/Log.h"

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <algorithm>
    #include <cstring>
    #include <d3d12.h>
    #include <wrl/client.h>
#endif

namespace cc {
namespace gfx {

struct CCD3D12Buffer::Impl {
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
#endif
    uint32_t resourceOffset{0};
};

CCD3D12Buffer::CCD3D12Buffer() {
    _impl = std::make_unique<Impl>();
}

CCD3D12Buffer::~CCD3D12Buffer() {
    destroy();
}

void CCD3D12Buffer::doInit(const BufferInfo &info) {
    (void)info;
    createResource(_size);
}

void CCD3D12Buffer::doInit(const BufferViewInfo &info) {
    auto *buffer = static_cast<CCD3D12Buffer *>(info.buffer);
    if (!buffer) {
        return;
    }

#if defined(_WIN32)
    _impl->resource = static_cast<ID3D12Resource *>(buffer->getD3D12ResourceHandle());
#endif
    // Buffer views can be created from another view. In that case the final
    // GPU VA must include the parent view's offset; otherwise we will bind
    // descriptors to the wrong address/range.
    const uint64_t parentOffset = static_cast<uint64_t>(buffer->getD3D12ResourceOffset());
    const uint64_t requestedOffset = parentOffset + static_cast<uint64_t>(info.offset);

#if defined(_WIN32)
    if (_impl->resource) {
        const uint64_t resourceWidth = static_cast<uint64_t>(_impl->resource->GetDesc().Width);
        if (requestedOffset > resourceWidth) {
            CC_LOG_ERROR("D3D12Buffer view offset out of range. parentOffset=%llu info.offset=%u resourceWidth=%llu",
                         static_cast<unsigned long long>(parentOffset),
                         info.offset,
                         static_cast<unsigned long long>(resourceWidth));
            _impl->resourceOffset = static_cast<uint32_t>(resourceWidth);
            return;
        }
    }
#endif

    _impl->resourceOffset = static_cast<uint32_t>(requestedOffset);
}

void CCD3D12Buffer::doResize(uint32_t size, uint32_t count) {
    (void)count;
    if (_isBufferView) {
        return;
    }
    createResource(size);
}

void CCD3D12Buffer::doDestroy() {
#if defined(_WIN32)
    if (_impl) {
        _impl->resource.Reset();
    }
#endif
    if (_impl) {
        _impl->resourceOffset = 0;
    }
}

void CCD3D12Buffer::update(const void *buffer, uint32_t size) {
    if (!buffer || size == 0 || _size == 0) {
        return;
    }

#if defined(_WIN32)
    if (!_impl || !_impl->resource) {
        return;
    }

    void *mappedData = nullptr;
    D3D12_RANGE readRange{};
    HRESULT hr = _impl->resource->Map(0, &readRange, &mappedData);
    if (FAILED(hr) || !mappedData) {
        CC_LOG_ERROR("D3D12 buffer Map failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    const uint32_t copySize = std::min(size, _size);
    auto *dst = static_cast<uint8_t *>(mappedData) + _impl->resourceOffset;
    std::memcpy(dst, buffer, copySize);

    static uint32_t s_diagBufferUpdateCount = 0;
    if (s_diagBufferUpdateCount < 48 && copySize >= sizeof(float) * 4 && _size <= 256) {
        const auto *floats = static_cast<const float *>(buffer);
        CC_LOG_INFO("[D3D12-BUF] size=%u copy=%u f0=%.3f f1=%.3f f2=%.3f f3=%.3f",
                    _size, copySize, floats[0], floats[1], floats[2], floats[3]);
        ++s_diagBufferUpdateCount;
    }

    D3D12_RANGE writeRange{_impl->resourceOffset, _impl->resourceOffset + copySize};
    _impl->resource->Unmap(0, &writeRange);
#endif
}

void *CCD3D12Buffer::getD3D12ResourceHandle() const {
#if defined(_WIN32)
    return _impl ? _impl->resource.Get() : nullptr;
#else
    return nullptr;
#endif
}

uint64_t CCD3D12Buffer::getD3D12GPUVirtualAddress() const {
#if defined(_WIN32)
    if (!_impl || !_impl->resource) {
        return 0;
    }
    return static_cast<uint64_t>(_impl->resource->GetGPUVirtualAddress() + _impl->resourceOffset);
#else
    return 0;
#endif
}

uint32_t CCD3D12Buffer::getD3D12ResourceOffset() const {
    return _impl ? _impl->resourceOffset : 0;
}

bool CCD3D12Buffer::createResource(uint32_t size) {
#if defined(_WIN32)
    if (!_impl || size == 0) {
        return false;
    }

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12 device unavailable when creating buffer.");
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = static_cast<UINT64>((size + 255U) & ~255U);
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    HRESULT hr = d3dDevice->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateCommittedResource(buffer) failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    _impl->resource = resource;
    _impl->resourceOffset = 0;
    return true;
#else
    (void)size;
    return false;
#endif
}

} // namespace gfx
} // namespace cc
