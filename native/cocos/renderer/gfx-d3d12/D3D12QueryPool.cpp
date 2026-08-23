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

#include "D3D12QueryPool.h"
#include "D3D12DebugOptimization.h"
#include "D3D12Device.h"
#include "base/Log.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <cstring>
    #include <d3d12.h>
    #include <limits>
    #include <unordered_map>
    #include <vector>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

struct CCD3D12QueryPool::Impl {
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent{nullptr};
    uint64_t fenceValue{0};
    std::vector<uint32_t> completedIds;
    std::unordered_map<uint32_t, uint32_t> activeIndices;
};

static constexpr uint32_t INVALID_D3D12_QUERY_INDEX = std::numeric_limits<uint32_t>::max();

CCD3D12QueryPool::CCD3D12QueryPool()
: _impl(std::make_unique<Impl>()) {
}

CCD3D12QueryPool::~CCD3D12QueryPool() = default;

void CCD3D12QueryPool::doInit(const QueryPoolInfo &info) {
    auto *devicePtr = static_cast<ID3D12Device *>(CCD3D12Device::getInstance()->getD3D12DeviceHandle());
    if (!devicePtr) {
        CC_LOG_ERROR("D3D12QueryPool::doInit — device handle is null.");
        return;
    }

    D3D12_QUERY_HEAP_TYPE heapType{};
    switch (_type) {
        case QueryType::OCCLUSION:
            heapType = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
            break;
        case QueryType::TIMESTAMP:
            heapType = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            break;
        case QueryType::PIPELINE_STATISTICS:
            CC_LOG_ERROR("D3D12QueryPool: pipeline statistics are not supported by the scalar GFX query result API.");
            return;
    }

    // Create query heap
    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Count = _maxQueryObjects;
    heapDesc.NodeMask = 0;
    heapDesc.Type = heapType;

    HRESULT hr = devicePtr->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&_impl->queryHeap));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool CreateQueryHeap failed. HRESULT=0x%08x, count=%u",
                     static_cast<unsigned>(hr), _maxQueryObjects);
        return;
    }

    // Create readback buffer for fetching results
    // Each query result is a uint64_t (8 bytes)
    const uint64_t bufferSize = static_cast<uint64_t>(_maxQueryObjects) * sizeof(uint64_t);

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Alignment = 0;
    resDesc.Width = bufferSize;
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc.Count = 1;
    resDesc.SampleDesc.Quality = 0;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = devicePtr->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&_impl->readbackBuffer));

    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool CreateCommittedResource(readback) failed. "
                     "HRESULT=0x%08x, bufferSize=%llu",
                     static_cast<unsigned>(hr),
                     static_cast<unsigned long long>(bufferSize));
        return;
    }

    hr = devicePtr->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_impl->commandAllocator));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool CreateCommandAllocator failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    hr = devicePtr->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        _impl->commandAllocator.Get(), nullptr,
        IID_PPV_ARGS(&_impl->commandList));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool CreateCommandList failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }
    hr = _impl->commandList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool initial command list close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    hr = devicePtr->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_impl->fence));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool CreateFence failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }
    _impl->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!_impl->fenceEvent) {
        CC_LOG_ERROR("D3D12QueryPool CreateEvent failed.");
        return;
    }

    CC_D3D12_DIAGNOSTIC_LOG("D3D12 QueryPool initialized: type=%u, maxQueries=%u", static_cast<unsigned>(_type), _maxQueryObjects);
}

void CCD3D12QueryPool::doDestroy() {
    // A ResolveQueryData batch may still be executing on the queue; its
    // command list, heap and readback buffer must stay alive until the
    // completion fence is signaled. If the wait times out (or the fence
    // cannot be armed), the GPU may still be accessing them: deliberately
    // leak those objects by detaching them from the owning ComPtrs (they
    // are reclaimed at process exit) instead of releasing them out from
    // under the hardware.
    bool safeToRelease = true;
    if (_impl->fence && _impl->fenceEvent &&
        _impl->fence->GetCompletedValue() < _impl->fenceValue) {
        if (SUCCEEDED(_impl->fence->SetEventOnCompletion(_impl->fenceValue, _impl->fenceEvent))) {
            const DWORD waitResult = WaitForSingleObject(_impl->fenceEvent, 5000);
            if (waitResult != WAIT_OBJECT_0) {
                CC_LOG_ERROR("D3D12QueryPool::doDestroy timed out waiting for pending query readback. waitResult=%lu",
                             static_cast<unsigned long>(waitResult));
                safeToRelease = false;
            }
        } else {
            CC_LOG_ERROR("D3D12QueryPool::doDestroy failed to arm the readback completion fence.");
            safeToRelease = false;
        }
    }
    if (!safeToRelease) {
        // Keep the event handle alive as well: the armed completion may
        // still fire and would reference a closed handle otherwise.
        _impl->fenceEvent = nullptr;
        _impl->fence.Detach();
        _impl->commandList.Detach();
        _impl->commandAllocator.Detach();
        _impl->readbackBuffer.Detach();
        _impl->queryHeap.Detach();
        return;
    }
    if (_impl->fenceEvent) {
        CloseHandle(_impl->fenceEvent);
        _impl->fenceEvent = nullptr;
    }
    _impl->fence.Reset();
    _impl->commandList.Reset();
    _impl->commandAllocator.Reset();
    _impl->readbackBuffer.Reset();
    _impl->queryHeap.Reset();
}

void *CCD3D12QueryPool::getD3D12QueryHeap() const {
    return _impl ? _impl->queryHeap.Get() : nullptr;
}

uint32_t CCD3D12QueryPool::beginD3D12Query(uint32_t id) {
    if (!_impl || !_impl->queryHeap) {
        return INVALID_D3D12_QUERY_INDEX;
    }

    auto existing = _impl->activeIndices.find(id);
    if (existing != _impl->activeIndices.end()) {
        return existing->second;
    }

    const uint32_t queryIndex = static_cast<uint32_t>(_impl->completedIds.size() + _impl->activeIndices.size());
    if (queryIndex >= _maxQueryObjects) {
        CC_LOG_WARNING("D3D12QueryPool: query id %u ignored because max query count %u was reached.", id, _maxQueryObjects);
        return INVALID_D3D12_QUERY_INDEX;
    }

    _impl->activeIndices[id] = queryIndex;
    return queryIndex;
}

uint32_t CCD3D12QueryPool::endD3D12Query(uint32_t id) {
    if (!_impl || !_impl->queryHeap) {
        return INVALID_D3D12_QUERY_INDEX;
    }

    auto iter = _impl->activeIndices.find(id);
    if (iter == _impl->activeIndices.end()) {
        CC_LOG_WARNING("D3D12QueryPool: endQuery for id %u has no matching beginQuery.", id);
        return INVALID_D3D12_QUERY_INDEX;
    }

    const uint32_t queryIndex = iter->second;
    _impl->activeIndices.erase(iter);
    _impl->completedIds.push_back(id);
    return queryIndex;
}

void CCD3D12QueryPool::resetD3D12Queries() {
    if (!_impl) {
        return;
    }

    _impl->completedIds.clear();
    _impl->activeIndices.clear();
    std::lock_guard<std::mutex> lock(_mutex);
    _results.clear();
}

void CCD3D12QueryPool::fetchResults() {
    if (!_impl->queryHeap || !_impl->readbackBuffer || !_impl->commandAllocator ||
        !_impl->commandList || !_impl->fence || !_impl->fenceEvent) {
        return;
    }

    if (_impl->completedIds.empty()) {
        std::lock_guard<std::mutex> lock(_mutex);
        _results.clear();
        return;
    }

    auto *queuePtr = static_cast<ID3D12CommandQueue *>(CCD3D12Device::getInstance()->getGraphicsQueueHandle());
    if (!queuePtr) {
        return;
    }

    HRESULT hr = _impl->commandAllocator->Reset();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool fetchResults: command allocator reset failed.");
        return;
    }

    hr = _impl->commandList->Reset(_impl->commandAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool fetchResults: command list reset failed.");
        return;
    }

    const D3D12_QUERY_TYPE queryType = _type == QueryType::TIMESTAMP
                                           ? D3D12_QUERY_TYPE_TIMESTAMP
                                           : D3D12_QUERY_TYPE_OCCLUSION;

    const uint32_t queryCount = static_cast<uint32_t>(_impl->completedIds.size());

    _impl->commandList->ResolveQueryData(
        _impl->queryHeap.Get(),
        queryType,
        0, // start index
        queryCount,
        _impl->readbackBuffer.Get(),
        0); // aligned offset

    hr = _impl->commandList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool fetchResults: Close failed.");
        return;
    }

    ID3D12CommandList *lists[] = {_impl->commandList.Get()};
    queuePtr->ExecuteCommandLists(1, lists);

    ++_impl->fenceValue;
    hr = queuePtr->Signal(_impl->fence.Get(), _impl->fenceValue);
    if (FAILED(hr)) {
        return;
    }

    if (_impl->fence->GetCompletedValue() < _impl->fenceValue) {
        hr = _impl->fence->SetEventOnCompletion(_impl->fenceValue, _impl->fenceEvent);
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12QueryPool fetchResults: SetEventOnCompletion failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return;
        }
        const DWORD waitResult = WaitForSingleObject(_impl->fenceEvent, 5000);
        if (waitResult != WAIT_OBJECT_0) {
            CC_LOG_ERROR("D3D12QueryPool fetchResults: timed out or failed while waiting for query results. waitResult=%lu",
                         static_cast<unsigned long>(waitResult));
            return;
        }
    }

    // Map readback buffer and read results
    void *mappedData = nullptr;
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(queryCount) * sizeof(uint64_t)};
    hr = _impl->readbackBuffer->Map(0, &readRange, &mappedData);
    if (FAILED(hr) || !mappedData) {
        CC_LOG_ERROR("D3D12QueryPool fetchResults: Map readback buffer failed.");
        return;
    }

    const auto *results = static_cast<const uint64_t *>(mappedData);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _results.clear();
        for (uint32_t i = 0; i < queryCount; ++i) {
            _results[_impl->completedIds[i]] = results[i];
        }
    }

    D3D12_RANGE writeRange{0, 0};
    _impl->readbackBuffer->Unmap(0, &writeRange);

}

} // namespace gfx
} // namespace cc
