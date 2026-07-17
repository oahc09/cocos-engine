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
#include "D3D12CommandBuffer.h"
#include "D3D12Device.h"
#include "base/Log.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <algorithm>
    #include <cstring>
    #include <d3d12.h>
    #include <limits>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

struct CCD3D12Buffer::Impl {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    CCD3D12Buffer *parent{nullptr};
    uint32_t resourceOffset{0};
    bool uploadHeap{true};
    bool seenDynamicUniformBinding{false};
    bool seenNonDynamicUniformBinding{false};
    bool hasBufferViews{false};
    D3D12_RESOURCE_STATES currentState{D3D12_RESOURCE_STATE_GENERIC_READ};
    uint64_t stateEpoch{0};
    ccstd::vector<uint8_t> pendingData;
    bool updateQueued{false};
    ID3D12Resource *transientUniformResource{nullptr};
    uint64_t transientUniformGPUAddress{0};
    uint64_t transientUniformEpoch{std::numeric_limits<uint64_t>::max()};
    uint64_t uniformContentVersion{0};
    uint64_t uploadedContentVersion{std::numeric_limits<uint64_t>::max()};
    uint64_t uniformDescriptorVersion{1};
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

    auto *owner = buffer;
    while (owner->_impl && owner->_impl->parent) {
        owner = owner->_impl->parent;
    }
    if (owner->_impl) {
        owner->_impl->hasBufferViews = true;
        owner->_impl->transientUniformResource = nullptr;
        owner->_impl->transientUniformGPUAddress = 0;
        ++owner->_impl->uniformDescriptorVersion;
        if (auto *device = CCD3D12Device::getInstance()) {
            device->notifyTransientUniformUpload();
        }
        if (!owner->_impl->pendingData.empty() && !owner->_impl->updateQueued) {
            owner->_impl->updateQueued = true;
            if (auto *device = CCD3D12Device::getInstance()) {
                device->enqueueBufferUpdate(owner);
            }
        }
    }

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
    if (auto *device = CCD3D12Device::getInstance()) {
        device->discardPendingBufferUpdate(this);
    }
    if (_impl) {
        _impl->pendingData.clear();
        _impl->updateQueued = false;
        _impl->resource.Reset();
        _impl->parent = nullptr;
        _impl->transientUniformResource = nullptr;
        _impl->transientUniformGPUAddress = 0;
        _impl->transientUniformEpoch = std::numeric_limits<uint64_t>::max();
        _impl->uniformContentVersion = 0;
        _impl->uploadedContentVersion = std::numeric_limits<uint64_t>::max();
        _impl->uniformDescriptorVersion = 1;
        _impl->hasBufferViews = false;
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

    const uint32_t copySize = std::min(size, _size);
    const uint32_t resourceOffset = getD3D12ResourceOffset();

    if (isTransientUniformEligible()) {
        _impl->pendingData.resize(copySize);
        std::memcpy(_impl->pendingData.data(), buffer, copySize);
        ++_impl->uniformContentVersion;
        if (!_impl->updateQueued) {
            _impl->updateQueued = true;
            if (auto *device = CCD3D12Device::getInstance()) {
                device->enqueueBufferUpdate(this);
            }
        }
        return;
    }

    if (!isD3D12UploadHeap()) {
        auto *device = CCD3D12Device::getInstance();
        if (!device) {
            return;
        }

        // Buffer::update is frequently called once per model before drawing.
        // Keep the latest CPU payload and record all GPU copies into the main
        // command list instead of creating/submitting/waiting on one command
        // list per buffer. Queue ordering then protects data still used by the
        // previous frame while descriptors keep their stable DEFAULT resource.
        _impl->pendingData.resize(copySize);
        std::memcpy(_impl->pendingData.data(), buffer, copySize);
        if (!_impl->updateQueued) {
            _impl->updateQueued = true;
            device->enqueueBufferUpdate(this);
        }
        return;
    }

    void *mappedData = nullptr;
    D3D12_RANGE readRange{};
    HRESULT hr = resource->Map(0, &readRange, &mappedData);
    if (FAILED(hr) || !mappedData) {
        CC_LOG_ERROR("D3D12 buffer Map failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    auto *dst = static_cast<uint8_t *>(mappedData) + resourceOffset;
    std::memcpy(dst, buffer, copySize);

    D3D12_RANGE writeRange{resourceOffset, resourceOffset + copySize};
    resource->Unmap(0, &writeRange);
}

void CCD3D12Buffer::flushPendingUpdate(CCD3D12CommandBuffer *commandBuffer) {
    if (!_impl || !commandBuffer) {
        return;
    }

    _impl->updateQueued = false;
    if (_impl->pendingData.empty()) {
        return;
    }

    commandBuffer->updateBuffer(this, _impl->pendingData.data(), static_cast<uint32_t>(_impl->pendingData.size()));
    if (!canUseTransientUniformUpload()) {
        _impl->pendingData.clear();
    }
}

bool CCD3D12Buffer::isTransientUniformEligible() const {
    if (!_impl || _impl->parent || _impl->hasBufferViews ||
        !_impl->seenNonDynamicUniformBinding || _impl->seenDynamicUniformBinding) {
        return false;
    }
    return hasFlag(_usage, BufferUsageBit::UNIFORM) &&
           !hasFlag(_usage, BufferUsageBit::INDEX) &&
           !hasFlag(_usage, BufferUsageBit::VERTEX) &&
           !hasFlag(_usage, BufferUsageBit::STORAGE) &&
           !hasFlag(_usage, BufferUsageBit::INDIRECT);
}

bool CCD3D12Buffer::canUseTransientUniformUpload() const {
    return isTransientUniformEligible() && !_impl->updateQueued;
}

uint32_t CCD3D12Buffer::getPendingTransientUniformUploadSize() const {
    return isTransientUniformEligible() && _impl->updateQueued && !_impl->pendingData.empty()
               ? getD3D12ConstantBufferSize()
               : 0;
}

bool CCD3D12Buffer::flushTransientUniformUpload(void *resource, void *mappedData,
                                                uint64_t gpuAddress, uint64_t epoch) {
    if (!resource || !mappedData || gpuAddress == 0 ||
        getPendingTransientUniformUploadSize() == 0) {
        return false;
    }
    std::memcpy(mappedData, _impl->pendingData.data(), _impl->pendingData.size());
    _impl->updateQueued = false;
    _impl->transientUniformResource = static_cast<ID3D12Resource *>(resource);
    _impl->transientUniformGPUAddress = gpuAddress;
    _impl->transientUniformEpoch = epoch;
    _impl->uploadedContentVersion = _impl->uniformContentVersion;
    ++_impl->uniformDescriptorVersion;
    return true;
}

bool CCD3D12Buffer::ensureTransientUniformUpload() {
    if (!canUseTransientUniformUpload() || _impl->pendingData.empty()) {
        return false;
    }
    auto *device = CCD3D12Device::getInstance();
    if (!device) {
        return false;
    }
    const uint64_t epoch = device->getBufferStateEpoch();
    if (_impl->transientUniformResource && _impl->transientUniformGPUAddress != 0 &&
        _impl->transientUniformEpoch == epoch &&
        _impl->uploadedContentVersion == _impl->uniformContentVersion) {
        return true;
    }
    const auto allocation = device->allocateUploadBuffer(
        _impl->pendingData.size(), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!allocation.isValid || !allocation.resource || !allocation.mappedData || allocation.gpuAddress == 0) {
        return false;
    }
    std::memcpy(allocation.mappedData, _impl->pendingData.data(), _impl->pendingData.size());
    _impl->transientUniformResource = static_cast<ID3D12Resource *>(allocation.resource);
    _impl->transientUniformGPUAddress = allocation.gpuAddress;
    _impl->transientUniformEpoch = epoch;
    _impl->uploadedContentVersion = _impl->uniformContentVersion;
    ++_impl->uniformDescriptorVersion;
    return true;
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

uint64_t CCD3D12Buffer::getD3D12UniformGPUVirtualAddress() const {
    if (!_impl) {
        return 0;
    }
    if (_impl->parent) {
        return _impl->parent->getD3D12UniformGPUVirtualAddress() + _impl->resourceOffset;
    }
    return _impl->transientUniformGPUAddress != 0
               ? _impl->transientUniformGPUAddress
               : getD3D12GPUVirtualAddress();
}

uint64_t CCD3D12Buffer::getUniformDescriptorVersion() const {
    if (!_impl) {
        return 0;
    }
    return _impl->parent ? _impl->parent->getUniformDescriptorVersion()
                         : _impl->uniformDescriptorVersion;
}

uint32_t CCD3D12Buffer::getD3D12ConstantBufferSize() const {
    const uint64_t alignedSize = (static_cast<uint64_t>(_size) + 255ULL) & ~255ULL;
    return static_cast<uint32_t>(std::min<uint64_t>(alignedSize, 64ULL * 1024ULL));
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

bool CCD3D12Buffer::isD3D12UploadHeap() const {
    if (!_impl) {
        return true;
    }
    if (_impl->parent) {
        return _impl->parent->isD3D12UploadHeap();
    }
    return _impl->uploadHeap;
}

void CCD3D12Buffer::markUniformDescriptorBinding(bool dynamic) {
    if (!_impl) {
        return;
    }
    auto *owner = _impl->parent ? _impl->parent : this;
    if (!owner->_impl) {
        return;
    }
    if (dynamic) {
        owner->_impl->seenDynamicUniformBinding = true;
    } else {
        owner->_impl->seenNonDynamicUniformBinding = true;
    }
}

bool CCD3D12Buffer::isDynamicUniformOnly() const {
    if (!_impl) {
        return false;
    }
    const auto *owner = _impl->parent ? _impl->parent : this;
    return owner->_impl && owner->_impl->seenDynamicUniformBinding &&
           !owner->_impl->seenNonDynamicUniformBinding;
}

D3D12_RESOURCE_STATES CCD3D12Buffer::getCurrentState() const {
    if (!_impl) {
        return D3D12_RESOURCE_STATE_COMMON;
    }
    if (_impl->parent) {
        return _impl->parent->getCurrentState();
    }
    auto *device = CCD3D12Device::getInstance();
    if (device && _impl->stateEpoch != device->getBufferStateEpoch()) {
        // D3D12 buffer resources decay to COMMON after ExecuteCommandLists
        // completes, including buffers that used explicit transitions.
        return D3D12_RESOURCE_STATE_COMMON;
    }
    return _impl->currentState;
}

void CCD3D12Buffer::setCurrentState(D3D12_RESOURCE_STATES state) {
    if (!_impl) {
        return;
    }
    if (_impl->parent) {
        _impl->parent->setCurrentState(state);
        return;
    }
    _impl->currentState = state;
    if (auto *device = CCD3D12Device::getInstance()) {
        _impl->stateEpoch = device->getBufferStateEpoch();
    }
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

    const bool useUploadHeap = hasFlag(_memUsage, MemoryUsageBit::HOST) &&
                               !hasFlag(_usage, BufferUsageBit::UNIFORM);

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = useUploadHeap ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
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
        useUploadHeap ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        CC_LOG_ERROR("CreateCommittedResource(buffer) failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    _impl->resource = resource;
    if (_impl->updateQueued) {
        device->discardPendingBufferUpdate(this);
    }
    _impl->pendingData.clear();
    _impl->updateQueued = false;
    _impl->transientUniformResource = nullptr;
    _impl->transientUniformGPUAddress = 0;
    _impl->transientUniformEpoch = std::numeric_limits<uint64_t>::max();
    _impl->uniformContentVersion = 0;
    _impl->uploadedContentVersion = std::numeric_limits<uint64_t>::max();
    ++_impl->uniformDescriptorVersion;
    device->notifyTransientUniformUpload();
    _impl->resourceOffset = 0;
    _impl->uploadHeap = useUploadHeap;
    _impl->currentState = useUploadHeap ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;
    _impl->stateEpoch = device->getBufferStateEpoch();
    return true;
}

} // namespace gfx
} // namespace cc
