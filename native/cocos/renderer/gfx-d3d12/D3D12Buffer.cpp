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
#include "D3D12MemAlloc.h"
#include "D3D12ResourceState.h"
#include "base/Log.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <algorithm>
    #include <array>
    #include <atomic>
    #include <cstring>
    #include <d3d12.h>
    #include <limits>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

namespace {
std::atomic<uint64_t> BUFFER_RESOURCE_GENERATION{1};

ccstd::string describeD3D12BufferUsageFlags(BufferUsage usage) {
    struct FlagName {
        BufferUsageBit bit;
        const char *name;
    };
    static const FlagName NAMES[] = {
        {BufferUsageBit::TRANSFER_SRC, "TRANSFER_SRC"},
        {BufferUsageBit::TRANSFER_DST, "TRANSFER_DST"},
        {BufferUsageBit::INDEX, "INDEX"},
        {BufferUsageBit::VERTEX, "VERTEX"},
        {BufferUsageBit::UNIFORM, "UNIFORM"},
        {BufferUsageBit::STORAGE, "STORAGE"},
        {BufferUsageBit::INDIRECT, "INDIRECT"},
    };
    ccstd::string result;
    for (const auto &entry : NAMES) {
        if (hasFlag(usage, entry.bit)) {
            if (!result.empty()) {
                result += '|';
            }
            result += entry.name;
        }
    }
    if (result.empty()) {
        result = "NONE";
    }
    return result;
}
}

struct CCD3D12Buffer::Impl {
    // Immutable backing owner — shared_ptr to D3D12BufferBacking which holds
    // ALL D3D12MA allocations and D3D12 resources. Resize creates a NEW
    // backing and swaps this pointer; the old backing stays alive until all
    // consumers (command buffers, BufferViews) drop their references.
    std::shared_ptr<D3D12BufferBacking> backing;
    uint32_t activeUploadResource{0};
    uint64_t resourceVersion{1};
    bool uploadHeap{true};
    bool seenDynamicUniformBinding{false};
    bool seenNonDynamicUniformBinding{false};
    bool hasBufferViews{false};
    D3D12_RESOURCE_STATES currentState{D3D12_RESOURCE_STATE_GENERIC_READ};
    uint64_t stateEpoch{0};
    ccstd::vector<uint8_t> pendingData;
    ccstd::vector<uint8_t> uniformShadowData;
    uint64_t uniformBackingSize{0};
    bool updateQueued{false};
    // Guards the once-per-buffer warning for an update overwriting an
    // un-flushed pending payload (last-write-wins within a flush window).
    bool pendingOverwriteWarned{false};
    ID3D12Resource *transientUniformResource{nullptr};
    uint64_t transientUniformGPUAddress{0};
    uint64_t transientUniformEpoch{std::numeric_limits<uint64_t>::max()};
    uint32_t transientUniformSlotIndex{std::numeric_limits<uint32_t>::max()};
    uint64_t transientUniformSlotEpoch{std::numeric_limits<uint64_t>::max()};
    uint64_t uniformContentVersion{0};
    uint64_t uploadedContentVersion{std::numeric_limits<uint64_t>::max()};
    uint64_t uniformDescriptorVersion{1};
    // Base byte offset of this backing resource. Shared UPLOAD allocations use
    // a non-zero base; BufferView instances keep their own relative offsets.
    uint64_t resourceBaseOffset{0};
    // True when this HOST buffer sub-allocates from the device's shared
    // UPLOAD heap instead of owning a dedicated committed resource. The
    // resource pointer is non-owning (the device owns the heap). If a
    // cross-frame write triggers ensureUploadResource, the buffer switches
    // to a dedicated committed resource and this flag is cleared.
    bool usingSharedUploadPool{false};
};

CCD3D12Buffer::CCD3D12Buffer() {
    _impl = std::make_shared<Impl>();
}

CCD3D12Buffer::~CCD3D12Buffer() {
    destroy();
}

void CCD3D12Buffer::doInit(const BufferInfo &info) {
    (void)info;
    if (!_impl || _impl.use_count() > 1) {
        _impl = std::make_shared<Impl>();
    }
    _resourceOffset = 0;
    _viewValid = true;
    createResource(_size);
}

void CCD3D12Buffer::doInit(const BufferViewInfo &info) {
    _viewValid = false;
    auto *buffer = static_cast<CCD3D12Buffer *>(info.buffer);
    if (!buffer || !buffer->_impl) {
        return;
    }

    auto *resource = static_cast<ID3D12Resource *>(buffer->getD3D12ResourceHandle());
    if (!resource) {
        return;
    }
    const uint64_t parentOffset = buffer->getD3D12ResourceOffset();
    const uint64_t parentRange = buffer->getSize();
    const uint64_t viewOffset = info.offset;
    const uint64_t viewRange = info.range;
    const uint64_t resourceWidth = resource->GetDesc().Width;
    if (viewRange == 0 || viewOffset > parentRange ||
        viewRange > parentRange - viewOffset ||
        parentOffset > resourceWidth ||
        viewOffset > resourceWidth - parentOffset ||
        viewRange > resourceWidth - parentOffset - viewOffset) {
        CC_LOG_ERROR(
            "D3D12Buffer view range out of bounds. parentOffset=%llu parentRange=%llu "
            "viewOffset=%llu viewRange=%llu resourceWidth=%llu",
            static_cast<unsigned long long>(parentOffset),
            static_cast<unsigned long long>(parentRange),
            static_cast<unsigned long long>(viewOffset),
            static_cast<unsigned long long>(viewRange),
            static_cast<unsigned long long>(resourceWidth));
        return;
    }

    _impl = buffer->_impl;
    // Keep the offset relative to the shared backing. The backing may later
    // migrate from a shared UPLOAD allocation to a dedicated resource.
    _resourceOffset = buffer->_resourceOffset + viewOffset;
    _viewValid = true;
    BUFFER_RESOURCE_GENERATION.fetch_add(1, std::memory_order_relaxed);

    _impl->hasBufferViews = true;
    _impl->transientUniformResource = nullptr;
    _impl->transientUniformGPUAddress = 0;
    ++_impl->uniformDescriptorVersion;
    if (auto *device = CCD3D12Device::getInstance()) {
        device->notifyTransientUniformUpload();
    }
}

bool CCD3D12Buffer::doResize(uint32_t size, uint32_t count) {
    (void)count;
    if (_isBufferView) {
        return false;
    }
    return createResource(size);
}

void CCD3D12Buffer::doDestroy() {
    if (auto *device = CCD3D12Device::getInstance()) {
        device->discardPendingBufferUpdate(this);
    }
    // Do NOT modify shared Impl fields. BufferView instances share _impl
    // with the parent; clearing resources here would destroy their backing.
    // The Impl destructor releases resources before allocations via C++
    // reverse-declaration order when the last shared_ptr drops.
    BUFFER_RESOURCE_GENERATION.fetch_add(1, std::memory_order_relaxed);
    _impl.reset();
    _resourceOffset = 0;
    _viewValid = false;
}

void CCD3D12Buffer::update(const void *buffer, uint32_t size) {
    if (!buffer || size == 0 || _size == 0) {
        return;
    }
    // An update larger than the buffer is a caller bug; the silent clamp
    // below would only upload a prefix.
    CC_ASSERT(size <= _size);

    if (!_impl) {
        return;
    }

    auto *resource = static_cast<ID3D12Resource *>(getD3D12ResourceHandle());
    if (!resource) {
        return;
    }

    const uint32_t copySize = std::min(size, _size);
    uint64_t resourceOffset = getD3D12ResourceOffset();
    const uint64_t shadowOffset = _resourceOffset;
    const bool retainUniformShadow =
        hasFlag(_usage, BufferUsageBit::UNIFORM) &&
        !hasFlag(_usage, BufferUsageBit::INDEX) &&
        !hasFlag(_usage, BufferUsageBit::VERTEX) &&
        !hasFlag(_usage, BufferUsageBit::STORAGE) &&
        !hasFlag(_usage, BufferUsageBit::INDIRECT);
    if (retainUniformShadow) {
        if (_impl->uniformShadowData.size() != _impl->uniformBackingSize) {
            _impl->uniformShadowData.assign(_impl->uniformBackingSize, 0);
        }
        if (shadowOffset <= _impl->uniformShadowData.size() &&
            copySize <= _impl->uniformShadowData.size() - shadowOffset) {
            std::memcpy(_impl->uniformShadowData.data() + shadowOffset, buffer, copySize);
            ++_impl->uniformContentVersion;
        }
    }

    if (isTransientUniformEligible()) {
        auto *device = CCD3D12Device::getInstance();
        D3D12UploadAllocation allocation;
        constexpr uint32_t INVALID_SLOT = std::numeric_limits<uint32_t>::max();
        const auto &frameState = CCD3D12Device::getActiveTransientUniformFrameState();
        const uint64_t epoch = frameState.epoch;
        if (device && getD3D12ConstantBufferSize() <=
                          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT &&
            _impl->transientUniformSlotEpoch != epoch) {
            // Slot indices belong to one fence-protected frame resource. A
            // buffer may keep an index only for the current epoch; reusing a
            // historical index would prevent the frame-local arena from ever
            // reclaiming capacity and can alias a slot assigned this frame.
            _impl->transientUniformSlotIndex = INVALID_SLOT;
            allocation = device->getOrCreateTransientUniformSlot(
                _impl->transientUniformSlotIndex);
            if (allocation.isValid) {
                _impl->transientUniformSlotEpoch = epoch;
            }
        }
        if (!allocation.isValid && device) {
            device->recordTransientUniformSlotFallback();
            allocation = device->allocateUploadBuffer(
                getD3D12ConstantBufferSize(),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        }
        if (allocation.isValid && allocation.resource && allocation.mappedData &&
            allocation.gpuAddress != 0) {
            const auto &uploadData = _impl->uniformShadowData;
            const size_t uploadBytes = std::min<size_t>(uploadData.size(), allocation.size);
            std::memcpy(allocation.mappedData, uploadData.data(), uploadBytes);
            _impl->updateQueued = false;
            _impl->pendingData.clear();
            _impl->transientUniformResource = static_cast<ID3D12Resource *>(allocation.resource);
            _impl->transientUniformGPUAddress = allocation.gpuAddress;
            _impl->transientUniformEpoch = epoch;
            _impl->uploadedContentVersion = _impl->uniformContentVersion;
            ++_impl->uniformDescriptorVersion;
            device->notifyTransientUniformUpload();
            return;
        }
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
        if (_impl->updateQueued && !_impl->pendingOverwriteWarned) {
            // updateQueued is only cleared once the pending payload has been
            // flushed into a command list. Every consume point (draw /
            // beginDrawBatch / execute / end) drains pending updates first, so
            // two consecutive updates with no consume in between mean the
            // first payload was never issued to the GPU: dropping it is safe
            // (last-write-wins) and only flags a redundant CPU-side update.
            // Size/usage identify which engine buffer is updated twice per
            // flush window.
            _impl->pendingOverwriteWarned = true;
            CC_LOG_WARNING("D3D12 DEFAULT-heap buffer updated again before the pending upload was flushed; the earlier payload is dropped (last-write-wins within the flush window). size=%u usage=%s pending=%zu",
                           _size,
                           describeD3D12BufferUsageFlags(_usage).c_str(),
                           _impl->pendingData.size());
        }
        _impl->pendingData.resize(copySize);
        std::memcpy(_impl->pendingData.data(), buffer, copySize);
        if (!_impl->updateQueued) {
            _impl->updateQueued = true;
            device->enqueueBufferUpdate(this);
        }
        return;
    }

    void *mappedData = nullptr;
    if (auto *device = CCD3D12Device::getInstance()) {
        const uint32_t frameIndex = device->getActiveFrameResourceIndex();
        if (frameIndex != _impl->activeUploadResource) {
            // Lazily create the per-frame upload resource on first cross-frame
            // write. Static HOST buffers (updated once) never allocate the
            // second slot, halving their UPLOAD committed-resource footprint.
            if (!ensureUploadResource(frameIndex)) {
                CC_LOG_ERROR("D3D12 buffer update could not allocate the frame-local upload resource.");
                return;
            }
            _impl->activeUploadResource = frameIndex;
            _impl->backing->resource = _impl->backing->uploadResources[frameIndex];
            resource = _impl->backing->resource.Get();
            ++_impl->resourceVersion;
            BUFFER_RESOURCE_GENERATION.fetch_add(1, std::memory_order_relaxed);
            // ensureUploadResource may migrate from the shared UPLOAD pool
            // to dedicated committed resources, resetting _resourceOffset
            // to 0. Re-read so the Map/Unmap below uses the correct offset.
            resourceOffset = getD3D12ResourceOffset();
        }
    }
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

std::shared_ptr<D3D12BufferBacking> CCD3D12Buffer::getD3D12BufferBacking() const {
    return _impl ? _impl->backing : nullptr;
}

bool CCD3D12Buffer::isTransientUniformEligible() const {
    if (!_impl || _isBufferView || _impl->hasBufferViews ||
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

bool CCD3D12Buffer::updateTransientUniform(const void *data, uint32_t size) {
    if (!_impl || !data || size == 0 || !isDynamicUniformOnly() ||
        !hasFlag(_usage, BufferUsageBit::UNIFORM) ||
        hasFlag(_usage, BufferUsageBit::INDEX) ||
        hasFlag(_usage, BufferUsageBit::VERTEX) ||
        hasFlag(_usage, BufferUsageBit::STORAGE) ||
        hasFlag(_usage, BufferUsageBit::INDIRECT)) {
        return false;
    }

    const uint32_t copySize = std::min(size, _size);
    const uint64_t resourceOffset = _resourceOffset;
    if (_impl->uniformBackingSize == 0 ||
        resourceOffset > _impl->uniformBackingSize ||
        copySize > _impl->uniformBackingSize - resourceOffset) {
        return false;
    }
    if (_impl->uniformShadowData.size() != _impl->uniformBackingSize) {
        _impl->uniformShadowData.assign(_impl->uniformBackingSize, 0);
    }
    std::memcpy(_impl->uniformShadowData.data() + resourceOffset, data, copySize);

    auto *device = CCD3D12Device::getInstance();
    const auto allocation = device
                                ? device->allocateUploadBuffer(
                                      _impl->uniformBackingSize,
                                      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)
                                : D3D12UploadAllocation{};
    if (!allocation.isValid || !allocation.resource || !allocation.mappedData ||
        allocation.gpuAddress == 0) {
        return false;
    }

    std::memcpy(allocation.mappedData, _impl->uniformShadowData.data(),
                _impl->uniformShadowData.size());
    ++_impl->uniformContentVersion;
    _impl->uploadedContentVersion = _impl->uniformContentVersion;
    _impl->updateQueued = false;
    _impl->pendingData.clear();
    _impl->transientUniformResource = static_cast<ID3D12Resource *>(allocation.resource);
    _impl->transientUniformGPUAddress = allocation.gpuAddress;
    _impl->transientUniformEpoch = device->getBufferStateEpoch();
    ++_impl->uniformDescriptorVersion;
    device->notifyTransientUniformUpload();
    return true;
}

bool CCD3D12Buffer::flushTransientUniformUpload(void *resource, void *mappedData,
                                                 uint64_t gpuAddress, uint64_t epoch) {
    if (!resource || !mappedData || gpuAddress == 0 ||
        getPendingTransientUniformUploadSize() == 0 ||
        _impl->uniformShadowData.empty()) {
        return false;
    }
    const size_t uploadBytes = std::min<size_t>(
        _impl->uniformShadowData.size(), getD3D12ConstantBufferSize());
    std::memcpy(mappedData, _impl->uniformShadowData.data(), uploadBytes);
    _impl->updateQueued = false;
    _impl->pendingData.clear();
    _impl->transientUniformResource = static_cast<ID3D12Resource *>(resource);
    _impl->transientUniformGPUAddress = gpuAddress;
    _impl->transientUniformEpoch = epoch;
    _impl->uploadedContentVersion = _impl->uniformContentVersion;
    ++_impl->uniformDescriptorVersion;
    return true;
}

bool CCD3D12Buffer::ensureTransientUniformUpload() {
    if (!canUseTransientUniformUpload()) {
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
    if (_impl->uniformShadowData.empty()) {
        return false;
    }
    const auto allocation = device->allocateUploadBuffer(
        getD3D12ConstantBufferSize(), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!allocation.isValid || !allocation.resource || !allocation.mappedData || allocation.gpuAddress == 0) {
        return false;
    }
    const size_t uploadBytes = std::min<size_t>(
        _impl->uniformShadowData.size(), allocation.size);
    std::memcpy(allocation.mappedData, _impl->uniformShadowData.data(), uploadBytes);
    _impl->transientUniformResource = static_cast<ID3D12Resource *>(allocation.resource);
    _impl->transientUniformGPUAddress = allocation.gpuAddress;
    _impl->transientUniformEpoch = epoch;
    _impl->uploadedContentVersion = _impl->uniformContentVersion;
    ++_impl->uniformDescriptorVersion;
    return true;
}

void *CCD3D12Buffer::getD3D12ResourceHandle() const {
    if (!_impl || !_impl->backing || (_isBufferView && !_viewValid)) {
        return nullptr;
    }
    return _impl->backing->resource.Get();
}

uint64_t CCD3D12Buffer::getD3D12GPUVirtualAddress() const {
    if (!_impl || !_impl->backing || (_isBufferView && !_viewValid)) {
        return 0;
    }

    auto *resource = _impl->backing->resource.Get();
    if (!resource) {
        return 0;
    }
    return static_cast<uint64_t>(resource->GetGPUVirtualAddress() + getD3D12ResourceOffset());
}

uint64_t CCD3D12Buffer::getD3D12ResourceVersion() const {
    if (!_impl || (_isBufferView && !_viewValid)) {
        return 0;
    }
    return (_impl && _impl->backing && _impl->backing->resource) ? _impl->resourceVersion : 0;
}

uint64_t CCD3D12Buffer::getD3D12GlobalResourceGeneration() {
    return BUFFER_RESOURCE_GENERATION.load(std::memory_order_relaxed);
}

uint64_t CCD3D12Buffer::getD3D12UniformGPUVirtualAddress() const {
    if (!_impl || (_isBufferView && !_viewValid)) {
        return 0;
    }
    if (_impl->transientUniformGPUAddress != 0) {
        return _impl->transientUniformGPUAddress + _resourceOffset;
    }
    return getD3D12GPUVirtualAddress();
}

bool CCD3D12Buffer::getD3D12UniformGPUVirtualAddressStorage(
    const uint64_t *&storage) const {
    storage = nullptr;
    if (!_impl || _isBufferView || !isTransientUniformEligible()) {
        return false;
    }
    storage = &_impl->transientUniformGPUAddress;
    return true;
}

uint64_t CCD3D12Buffer::getUniformDescriptorVersion() const {
    return _impl && (!_isBufferView || _viewValid)
               ? _impl->uniformDescriptorVersion
               : 0;
}

uint32_t CCD3D12Buffer::getD3D12ConstantBufferSize() const {
    const uint64_t alignedSize = (static_cast<uint64_t>(_size) + 255ULL) & ~255ULL;
    return static_cast<uint32_t>(std::min<uint64_t>(alignedSize, 64ULL * 1024ULL));
}

uint64_t CCD3D12Buffer::getD3D12ResourceOffset() const {
    return _impl && (!_isBufferView || _viewValid)
               ? _impl->resourceBaseOffset + _resourceOffset
               : 0;
}

bool CCD3D12Buffer::isD3D12UploadHeap() const {
    return !_impl || _impl->uploadHeap;
}

void CCD3D12Buffer::markUniformDescriptorBinding(bool dynamic) {
    if (!_impl) {
        return;
    }
    if (dynamic) {
        _impl->seenDynamicUniformBinding = true;
    } else {
        _impl->seenNonDynamicUniformBinding = true;
    }
}

bool CCD3D12Buffer::isDynamicUniformOnly() const {
    if (!_impl) {
        return false;
    }
    return _impl->seenDynamicUniformBinding &&
           !_impl->seenNonDynamicUniformBinding;
}

D3D12_RESOURCE_STATES CCD3D12Buffer::getCurrentState() const {
    if (!_impl) {
        return D3D12_RESOURCE_STATE_COMMON;
    }
    if (_impl->uploadHeap) {
        return D3D12_RESOURCE_STATE_GENERIC_READ;
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
    if (_impl->uploadHeap) {
        if (state != D3D12_RESOURCE_STATE_GENERIC_READ) {
            CC_LOG_ERROR("D3D12 upload heap buffers must remain in GENERIC_READ state.");
        }
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

    // HOST buffers use the UPLOAD heap. UNIFORM HOST buffers > 1KB stay
    // on DEFAULT heap (their data is uploaded via pendingData + command list
    // copy), but tiny UNIFORM HOST buffers (≤1KB) are allowed onto the UPLOAD
    // heap so they can sub-allocate from the shared UPLOAD pool. For static
    // uniform bindings the buffer's own resource is never read (the transient
    // uniform slot provides the GPU address), so pooling eliminates a
    // 64KB-minimum DEFAULT committed resource per buffer. Dynamic bindings
    // migrate to dedicated per-frame UPLOAD resources on first cross-frame
    // write via ensureUploadResource.
    const bool useUploadHeap = hasFlag(_memUsage, MemoryUsageBit::HOST) &&
                               (!hasFlag(_usage, BufferUsageBit::UNIFORM) ||
                                size <= 1024U);

    // Save old resources for deferred release. New resources are created
    // first; old ones are released only after success. If creation fails,
    // the buffer and all sharing BufferViews keep their old backing.
    auto newBacking = std::make_shared<D3D12BufferBacking>();

    const uint64_t alignedSize = static_cast<uint64_t>((size + 255U) & ~255U);

    // Small HOST (vertex/index/uniform) buffers sub-allocate from a long-lived
    // shared UPLOAD heap instead of creating individual 64KB-minimum committed
    // resources. This eliminates per-buffer alignment waste that dominates
    // memory for small buffers. If a cross-frame write later requires a
    // dedicated resource (ensureUploadResource), the buffer transparently
    // migrates out of the shared pool.
    if (useUploadHeap && alignedSize <= 16ULL * 1024ULL) {
        D3D12SharedUploadAllocation sharedAlloc = device->allocateSharedUploadOffset(
            alignedSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (sharedAlloc.isValid) {
            newBacking->resource = static_cast<ID3D12Resource *>(sharedAlloc.resource);
            // Atomic swap: replace old backing with new. Old backing stays
            // alive until all consumers (command buffers) drop references.
            _impl->backing = std::move(newBacking);
            _impl->usingSharedUploadPool = true;
            _impl->activeUploadResource = 0U;
            _impl->resourceBaseOffset = sharedAlloc.offset;
            _resourceOffset = 0;
            ++_impl->resourceVersion;
            BUFFER_RESOURCE_GENERATION.fetch_add(1, std::memory_order_relaxed);
            if (_impl->updateQueued) {
                device->discardPendingBufferUpdate(this);
            }
            _impl->pendingData.clear();
            _impl->uniformShadowData.clear();
            _impl->uniformBackingSize = alignedSize;
            _impl->updateQueued = false;
            _impl->transientUniformResource = nullptr;
            _impl->transientUniformGPUAddress = 0;
            _impl->transientUniformEpoch = std::numeric_limits<uint64_t>::max();
            _impl->transientUniformSlotEpoch = std::numeric_limits<uint64_t>::max();
            _impl->uniformContentVersion = 0;
            _impl->uploadedContentVersion = std::numeric_limits<uint64_t>::max();
            ++_impl->uniformDescriptorVersion;
            device->notifyTransientUniformUpload();
            _viewValid = true;
            _impl->uploadHeap = true;
            _impl->currentState = D3D12_RESOURCE_STATE_GENERIC_READ;
            _impl->stateEpoch = device->getBufferStateEpoch();
            return true;
        }
        // Shared pool full or unavailable: fall through to committed resource.
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = useUploadHeap ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = static_cast<UINT64>(alignedSize);
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<D3D12MA::Allocation> allocation;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    // HOST (UPLOAD-heap) buffers: create only the first-slot resource
    // upfront. The second slot is created lazily by ensureUploadResource() when
    // a cross-frame write is detected. Static HOST buffers (updated once) never
    // allocate the second slot, saving one 64KB-minimum committed resource each.
    // Dynamic buffers pay the same total cost, just deferred to first cross-frame
    // update. This is safe because every access to backing->uploadResources[i]
    // either goes through ensureUploadResource (which creates on demand) or
    // accesses slot 0 (always created here).
    const D3D12_RESOURCE_STATES initialState =
        useUploadHeap ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;
    auto *allocator = device->getMemoryAllocator();
    HRESULT hr = E_FAIL;
    if (allocator) {
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
        allocDesc.HeapType = useUploadHeap ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
        allocDesc.ExtraHeapFlags = D3D12_HEAP_FLAG_NONE;
        hr = allocator->CreateResource(&allocDesc, &resourceDesc, initialState,
                                       nullptr, allocation.ReleaseAndGetAddressOf(),
                                       IID_PPV_ARGS(&resource));
        if (FAILED(hr) || !allocation) {
            resource.Reset();
            allocation.Reset();
        }
    }
    if (!allocation) {
        hr = d3dDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&resource));
        if (FAILED(hr)) {
            CC_LOG_ERROR("CreateCommittedResource(buffer) failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return false;
        }
    }
    // Populate the new backing with the created resources. Upload-heap
    // buffers store the resource in slot 0; DEFAULT-heap buffers store
    // it as the main resource.
    newBacking->d3d12maAllocation = std::move(allocation);
    if (useUploadHeap) {
        newBacking->uploadResources[0] = resource;
        newBacking->uploadAllocations[0] = newBacking->d3d12maAllocation;
        newBacking->resource = newBacking->uploadResources[0];
    } else {
        newBacking->resource = std::move(resource);
    }

    // Atomic swap: replace old backing with new. Old backing stays alive
    // until all consumers (command buffers, BufferViews) drop references.
    _impl->backing = std::move(newBacking);
    _impl->usingSharedUploadPool = false;
    _impl->activeUploadResource = 0U;
    ++_impl->resourceVersion;
    BUFFER_RESOURCE_GENERATION.fetch_add(1, std::memory_order_relaxed);
    if (_impl->updateQueued) {
        device->discardPendingBufferUpdate(this);
    }
    _impl->pendingData.clear();
    _impl->uniformShadowData.clear();
    _impl->uniformBackingSize = resourceDesc.Width;
    _impl->updateQueued = false;
    _impl->transientUniformResource = nullptr;
    _impl->transientUniformGPUAddress = 0;
    _impl->transientUniformEpoch = std::numeric_limits<uint64_t>::max();
    _impl->transientUniformSlotEpoch = std::numeric_limits<uint64_t>::max();
    _impl->uniformContentVersion = 0;
    _impl->uploadedContentVersion = std::numeric_limits<uint64_t>::max();
    ++_impl->uniformDescriptorVersion;
    device->notifyTransientUniformUpload();
    _impl->resourceBaseOffset = 0;
    _resourceOffset = 0;
    _viewValid = true;
    _impl->uploadHeap = useUploadHeap;
    _impl->currentState = useUploadHeap ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;
    _impl->stateEpoch = device->getBufferStateEpoch();
    return true;
}

bool CCD3D12Buffer::ensureUploadResource(uint32_t frameIndex) {
    if (!_impl || !_impl->uploadHeap || frameIndex >= D3D12_MAX_FRAMES_IN_FLIGHT) {
        return false;
    }

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice || _impl->uniformBackingSize == 0) {
        return false;
    }

    // COW: if the backing is shared (e.g., retained by a command buffer's
    // fence context), create a private copy before modifying. The old backing
    // keeps the previous upload resources alive for in-flight GPU work.
    if (_impl->backing.use_count() > 1) {
        _impl->backing = std::make_shared<D3D12BufferBacking>(*_impl->backing);
    }

    // If this buffer is still sub-allocated from the shared UPLOAD pool, the
    // first cross-frame write requires migrating to dedicated per-frame
    // committed resources. Slot 0 is created here with the current data
    // copied from the shared pool, then the requested frameIndex slot is
    // created below. After migration, the shared resource reference is
    // released (the device pool still owns it).
    if (_impl->usingSharedUploadPool) {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProperties.CreationNodeMask = 1;
        heapProperties.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Alignment = 0;
        resourceDesc.Width = _impl->uniformBackingSize;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        Microsoft::WRL::ComPtr<D3D12MA::Allocation> alloc0;
        Microsoft::WRL::ComPtr<ID3D12Resource> uploadRes0;
        auto *allocator = device->getMemoryAllocator();
        HRESULT hr = E_FAIL;
        if (allocator) {
            D3D12MA::ALLOCATION_DESC allocDesc{};
            allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
            allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
            allocDesc.ExtraHeapFlags = D3D12_HEAP_FLAG_NONE;
            hr = allocator->CreateResource(&allocDesc, &resourceDesc,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                           alloc0.ReleaseAndGetAddressOf(),
                                           IID_PPV_ARGS(&uploadRes0));
            if (FAILED(hr) || !alloc0) {
                uploadRes0.Reset();
                alloc0.Reset();
            }
        }
        if (!alloc0) {
            hr = d3dDevice->CreateCommittedResource(
                &heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&uploadRes0));
            if (FAILED(hr)) {
                CC_LOG_ERROR("ensureUploadResource migration: CreateCommittedResource(0) failed. HRESULT=0x%08x",
                             static_cast<unsigned>(hr));
                return false;
            }
        }
        _impl->backing->uploadResources[0] = std::move(uploadRes0);
        _impl->backing->uploadAllocations[0] = std::move(alloc0);

        // Copy current data from the shared pool into the new dedicated slot 0
        // so the GPU can keep reading valid data while the CPU writes to the
        // frameIndex slot.
        D3D12_RANGE readRange{};
        void *dstMapped = nullptr;
        hr = _impl->backing->uploadResources[0]->Map(0, &readRange, &dstMapped);
        if (SUCCEEDED(hr) && dstMapped) {
            auto *sharedResource = _impl->backing->resource.Get();
            D3D12_RANGE srcReadRange{0, 0};
            void *srcMapped = nullptr;
            hr = sharedResource->Map(0, &srcReadRange, &srcMapped);
            if (SUCCEEDED(hr) && srcMapped) {
                std::memcpy(dstMapped,
                            static_cast<uint8_t *>(srcMapped) + _impl->resourceBaseOffset,
                            _impl->uniformBackingSize);
                D3D12_RANGE noWrite{0, 0};
                sharedResource->Unmap(0, &noWrite);
            }
            D3D12_RANGE writeRange{0, _impl->uniformBackingSize};
            _impl->backing->uploadResources[0]->Unmap(0, &writeRange);
        }

        _impl->backing->resource = _impl->backing->uploadResources[0];
        _impl->resourceBaseOffset = 0;
        // Do NOT reset _resourceOffset here: it is a per-instance member, and
        // a BufferView keeps its relative offset within the shared backing.
        // The parent buffer's offset is already 0. Resetting a view's offset
        // would collapse its GPU VA and write position to the resource start.
        _impl->usingSharedUploadPool = false;
        ++_impl->resourceVersion;
        BUFFER_RESOURCE_GENERATION.fetch_add(1, std::memory_order_relaxed);
    }

    if (_impl->backing->uploadResources[frameIndex]) {
        return true;
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
    resourceDesc.Width = _impl->uniformBackingSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<D3D12MA::Allocation> allocN;
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadResN;
    auto *allocator = device->getMemoryAllocator();
    HRESULT hr = E_FAIL;
    if (allocator) {
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
        allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        allocDesc.ExtraHeapFlags = D3D12_HEAP_FLAG_NONE;
        hr = allocator->CreateResource(&allocDesc, &resourceDesc,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                       allocN.ReleaseAndGetAddressOf(),
                                       IID_PPV_ARGS(&uploadResN));
        if (FAILED(hr) || !allocN) {
            uploadResN.Reset();
            allocN.Reset();
        }
    }
    if (!allocN) {
        hr = d3dDevice->CreateCommittedResource(
            &heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&uploadResN));
        if (FAILED(hr)) {
            CC_LOG_ERROR("ensureUploadResource: CreateCommittedResource failed. HRESULT=0x%08x",
                         static_cast<unsigned>(hr));
            return false;
        }
    }
    _impl->backing->uploadResources[frameIndex] = std::move(uploadResN);
    _impl->backing->uploadAllocations[frameIndex] = std::move(allocN);
    return true;
}

} // namespace gfx
} // namespace cc
