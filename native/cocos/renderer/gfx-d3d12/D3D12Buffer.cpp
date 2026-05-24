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

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <algorithm>
    #include <cstring>
    #include <d3d12.h>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

struct CCD3D12Buffer::Impl {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    CCD3D12Buffer *parent{nullptr};
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

    _impl->parent = buffer;
    _impl->resource.Reset();
    _impl->resourceOffset = info.offset;

    auto *resource = static_cast<ID3D12Resource *>(buffer->getD3D12ResourceHandle());
    const uint64_t requestedOffset = static_cast<uint64_t>(buffer->getD3D12ResourceOffset()) + static_cast<uint64_t>(info.offset);

    if (resource) {
        const uint64_t resourceWidth = static_cast<uint64_t>(resource->GetDesc().Width);
        if (requestedOffset > resourceWidth) {
            CC_LOG_ERROR("D3D12Buffer view offset out of range. parentOffset=%llu info.offset=%u resourceWidth=%llu",
                         static_cast<unsigned long long>(buffer->getD3D12ResourceOffset()),
                         info.offset,
                         static_cast<unsigned long long>(resourceWidth));
            _impl->resourceOffset = static_cast<uint32_t>(resourceWidth - buffer->getD3D12ResourceOffset());
            return;
        }
    }
}

void CCD3D12Buffer::doResize(uint32_t size, uint32_t count) {
    (void)count;
    if (_isBufferView) {
        return;
    }
    createResource(size);
}

void CCD3D12Buffer::doDestroy() {
    if (_impl) {
        _impl->resource.Reset();
        _impl->parent = nullptr;
    }
    if (_impl) {
        _impl->resourceOffset = 0;
    }
}

void CCD3D12Buffer::update(const void *buffer, uint32_t size) {
    if (!buffer || size == 0 || _size == 0) {
        return;
    }

    if (!_impl) {
        return;
    }

    auto *resource = static_cast<ID3D12Resource *>(getD3D12ResourceHandle());
    if (!resource) {
        return;
    }
    void *mappedData = nullptr;
    D3D12_RANGE readRange{};
    HRESULT hr = resource->Map(0, &readRange, &mappedData);
    if (FAILED(hr) || !mappedData) {
        CC_LOG_ERROR("D3D12 buffer Map failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    const uint32_t copySize = std::min(size, _size);
    const uint32_t resourceOffset = getD3D12ResourceOffset();
    auto *dst = static_cast<uint8_t *>(mappedData) + resourceOffset;
    std::memcpy(dst, buffer, copySize);

    D3D12_RANGE writeRange{resourceOffset, resourceOffset + copySize};
    resource->Unmap(0, &writeRange);
}

void *CCD3D12Buffer::getD3D12ResourceHandle() const {
    if (!_impl) {
        return nullptr;
    }
    if (_impl->parent) {
        return _impl->parent->getD3D12ResourceHandle();
    }
    return _impl->resource.Get();
}

uint64_t CCD3D12Buffer::getD3D12GPUVirtualAddress() const {
    if (!_impl) {
        return 0;
    }
    if (_impl->parent) {
        return _impl->parent->getD3D12GPUVirtualAddress() + _impl->resourceOffset;
    }

    auto *resource = _impl->resource.Get();
    if (!resource) {
        return 0;
    }
    return static_cast<uint64_t>(resource->GetGPUVirtualAddress() + _impl->resourceOffset);
}

uint32_t CCD3D12Buffer::getD3D12ResourceOffset() const {
    if (!_impl) {
        return 0;
    }
    if (_impl->parent) {
        return _impl->parent->getD3D12ResourceOffset() + _impl->resourceOffset;
    }
    return _impl->resourceOffset;
}

Microsoft::WRL::ComPtr<ID3D12Resource> CCD3D12Buffer::replaceD3D12Resource(Microsoft::WRL::ComPtr<ID3D12Resource> resource) {
    Microsoft::WRL::ComPtr<ID3D12Resource> previous;
    if (!_impl || _isBufferView || !resource) {
        return previous;
    }

    previous = _impl->resource;
    _impl->resource = std::move(resource);
    _impl->resourceOffset = 0;
    return previous;
}

bool CCD3D12Buffer::createResource(uint32_t size) {
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
}

} // namespace gfx
} // namespace cc
