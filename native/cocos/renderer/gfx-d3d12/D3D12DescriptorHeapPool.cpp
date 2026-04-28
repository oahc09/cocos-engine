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

#include "D3D12DescriptorHeapPool.h"
#include "D3D12Device.h"
#include "base/Log.h"

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <wrl/client.h>
#endif

namespace cc {
namespace gfx {

struct HeapEntry {
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
#endif
    uint32_t usedCount{0};
    uint32_t capacity{0};
};

struct D3D12DescriptorHeapPool::Impl {
#if defined(_WIN32)
    D3D12_DESCRIPTOR_HEAP_TYPE d3dHeapType{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
#endif
    HeapType heapType{HeapType::CBV_SRV_UAV};
    uint32_t maxDescriptorsPerHeap{1024};
    uint32_t descriptorSize{0};
    bool shaderVisible{false};
    bool initialized{false};

    ccstd::vector<HeapEntry> heaps;

    // Free list for deallocation (offset within heap, count)
    struct FreeBlock {
        uint32_t heapIndex{0};
        uint32_t offset{0};
        uint32_t count{0};
    };
    ccstd::vector<FreeBlock> freeList;
};

D3D12DescriptorHeapPool::D3D12DescriptorHeapPool()
: _impl(std::make_unique<Impl>()) {
}

D3D12DescriptorHeapPool::~D3D12DescriptorHeapPool() {
    shutdown();
}

void D3D12DescriptorHeapPool::initialize(HeapType heapType, uint32_t maxDescriptorsPerHeap, bool shaderVisible) {
    shutdown();

    _impl->heapType = heapType;
    _impl->maxDescriptorsPerHeap = maxDescriptorsPerHeap;
    _impl->shaderVisible = shaderVisible;

#if defined(_WIN32)
    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12DescriptorHeapPool: device unavailable.");
        return;
    }

    switch (heapType) {
        case HeapType::CBV_SRV_UAV:
            _impl->d3dHeapType = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            break;
        case HeapType::SAMPLER:
            _impl->d3dHeapType = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            break;
    }

    _impl->descriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(_impl->d3dHeapType);
#endif

    _impl->initialized = true;
    CC_LOG_INFO("D3D12DescriptorHeapPool initialized: type=%s, maxPerHeap=%u, shaderVisible=%s",
                heapType == HeapType::CBV_SRV_UAV ? "CBV_SRV_UAV" : "SAMPLER",
                maxDescriptorsPerHeap, shaderVisible ? "true" : "false");
}

void D3D12DescriptorHeapPool::shutdown() {
    if (_impl) {
        _impl->heaps.clear();
        _impl->freeList.clear();
        _impl->initialized = false;
    }
}

D3D12DescriptorHeapPool::Allocation D3D12DescriptorHeapPool::allocate(uint32_t count) {
    Allocation alloc;

    if (!_impl->initialized || count == 0) {
        return alloc;
    }

#if defined(_WIN32)
    // First try to find a free block that fits
    for (auto it = _impl->freeList.begin(); it != _impl->freeList.end(); ++it) {
        if (it->count >= count) {
            auto &heap = _impl->heaps[it->heapIndex];
            alloc.heapIndex = it->heapIndex;
            alloc.numDescriptors = count;

            D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = heap.heap->GetCPUDescriptorHandleForHeapStart();
            cpuStart.ptr += static_cast<UINT64>(it->offset) * _impl->descriptorSize;
            alloc.cpuHandle = reinterpret_cast<void *>(cpuStart.ptr);

            if (_impl->shaderVisible) {
                D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = heap.heap->GetGPUDescriptorHandleForHeapStart();
                gpuStart.ptr += static_cast<UINT64>(it->offset) * _impl->descriptorSize;
                alloc.gpuHandle = gpuStart.ptr;
            }

            alloc.isValid = true;

            // If the free block is larger, shrink it; otherwise remove it
            if (it->count > count) {
                it->offset += count;
                it->count -= count;
            } else {
                _impl->freeList.erase(it);
            }

            return alloc;
        }
    }

    // No free block found; try to allocate from the end of an existing heap
    for (uint32_t i = 0; i < static_cast<uint32_t>(_impl->heaps.size()); ++i) {
        auto &heap = _impl->heaps[i];
        if (heap.usedCount + count <= heap.capacity) {
            alloc.heapIndex = i;
            alloc.numDescriptors = count;

            D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = heap.heap->GetCPUDescriptorHandleForHeapStart();
            cpuStart.ptr += static_cast<UINT64>(heap.usedCount) * _impl->descriptorSize;
            alloc.cpuHandle = reinterpret_cast<void *>(cpuStart.ptr);

            if (_impl->shaderVisible) {
                D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = heap.heap->GetGPUDescriptorHandleForHeapStart();
                gpuStart.ptr += static_cast<UINT64>(heap.usedCount) * _impl->descriptorSize;
                alloc.gpuHandle = gpuStart.ptr;
            }

            alloc.isValid = true;
            heap.usedCount += count;
            return alloc;
        }
    }

    // Need to create a new heap
    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12DescriptorHeapPool: device unavailable for new heap.");
        return alloc;
    }

    uint32_t heapCapacity = (count > _impl->maxDescriptorsPerHeap) ? count : _impl->maxDescriptorsPerHeap;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = _impl->d3dHeapType;
    heapDesc.NumDescriptors = heapCapacity;
    heapDesc.Flags = _impl->shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heapDesc.NodeMask = 0;

    HeapEntry newEntry;
    HRESULT hr = d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&newEntry.heap));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12DescriptorHeapPool: CreateDescriptorHeap failed. HRESULT=0x%08x",
                     static_cast<unsigned>(hr));
        return alloc;
    }

    newEntry.capacity = heapCapacity;
    newEntry.usedCount = count;

    uint32_t newHeapIndex = static_cast<uint32_t>(_impl->heaps.size());
    _impl->heaps.push_back(std::move(newEntry));

    alloc.heapIndex = newHeapIndex;
    alloc.numDescriptors = count;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = _impl->heaps[newHeapIndex].heap->GetCPUDescriptorHandleForHeapStart();
    alloc.cpuHandle = reinterpret_cast<void *>(cpuStart.ptr);

    if (_impl->shaderVisible) {
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = _impl->heaps[newHeapIndex].heap->GetGPUDescriptorHandleForHeapStart();
        alloc.gpuHandle = gpuStart.ptr;
    }

    alloc.isValid = true;
#endif

    return alloc;
}

void D3D12DescriptorHeapPool::deallocate(const Allocation &alloc) {
    if (!alloc.isValid || !_impl->initialized) {
        return;
    }

    // Add to free list for potential reuse
    typename Impl::FreeBlock block;
    // Recover offset from CPU handle
#if defined(_WIN32)
    if (alloc.heapIndex < static_cast<uint32_t>(_impl->heaps.size())) {
        auto &heap = _impl->heaps[alloc.heapIndex];
        D3D12_CPU_DESCRIPTOR_HANDLE heapStart = heap.heap->GetCPUDescriptorHandleForHeapStart();
        uint64_t offsetBytes = reinterpret_cast<uint64_t>(alloc.cpuHandle) - heapStart.ptr;
        uint32_t offset = static_cast<uint32_t>(offsetBytes / _impl->descriptorSize);

        block.heapIndex = alloc.heapIndex;
        block.offset = offset;
        block.count = alloc.numDescriptors;
        _impl->freeList.push_back(block);
    }
#endif
}

void D3D12DescriptorHeapPool::reset() {
    for (auto &heap : _impl->heaps) {
        heap.usedCount = 0;
    }
    _impl->freeList.clear();
}

uint32_t D3D12DescriptorHeapPool::getDescriptorSize() const {
    return _impl ? _impl->descriptorSize : 0;
}

void *D3D12DescriptorHeapPool::getHeap(uint32_t heapIndex) const {
#if defined(_WIN32)
    if (!_impl || heapIndex >= static_cast<uint32_t>(_impl->heaps.size())) {
        return nullptr;
    }
    return _impl->heaps[heapIndex].heap.Get();
#else
    return nullptr;
#endif
}

uint32_t D3D12DescriptorHeapPool::getHeapCount() const {
    return _impl ? static_cast<uint32_t>(_impl->heaps.size()) : 0;
}

} // namespace gfx
} // namespace cc
