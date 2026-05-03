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
#include "D3D12Device.h"
#include "base/Log.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <cstring>
    #include <d3d12.h>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

struct CCD3D12QueryPool::Impl {
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
};

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

    // --- DIAG: Pre-flight check ---
    {
        HRESULT drr = devicePtr->GetDeviceRemovedReason();
        CC_LOG_INFO("[DIAG] QueryPool::doInit START: DeviceRemovedReason=0x%08x (%s)",
                     static_cast<unsigned>(drr), SUCCEEDED(drr) ? "OK" : "HUNG!");
    }

    // Determine query type
    D3D12_QUERY_HEAP_TYPE heapType = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    D3D12_QUERY_TYPE queryType = D3D12_QUERY_TYPE_OCCLUSION;

    if (_type == QueryType::OCCLUSION) {
        heapType = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        queryType = D3D12_QUERY_TYPE_OCCLUSION;
    }
    // Pipeline statistics or timestamp queries can be added later.

    // Create query heap
    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Count = _maxQueryObjects;
    heapDesc.NodeMask = 0;

    if (heapType == D3D12_QUERY_HEAP_TYPE_OCCLUSION) {
        heapDesc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    }

    HRESULT hr = devicePtr->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&_impl->queryHeap));
    if (FAILED(hr)) {
        CC_LOG_ERROR("[DIAG] D3D12QueryPool CreateQueryHeap FAILED. HRESULT=0x%08x, count=%u",
                     static_cast<unsigned>(hr), _maxQueryObjects);
        return;
    }
    CC_LOG_INFO("[DIAG] QueryPool CreateQueryHeap OK (count=%u).", _maxQueryObjects);

    // --- DIAG: Check after query heap creation ---
    {
        HRESULT drr = devicePtr->GetDeviceRemovedReason();
        CC_LOG_INFO("[DIAG] QueryPool after CreateQueryHeap: DeviceRemovedReason=0x%08x (%s)",
                     static_cast<unsigned>(drr), SUCCEEDED(drr) ? "OK" : "HUNG!");
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
        HRESULT drr = devicePtr->GetDeviceRemovedReason();
        CC_LOG_ERROR("[DIAG] D3D12QueryPool CreateCommittedResource(readback) FAILED. "
                     "HRESULT=0x%08x, DeviceRemovedReason=0x%08x, bufferSize=%llu",
                     static_cast<unsigned>(hr), static_cast<unsigned>(drr),
                     static_cast<unsigned long long>(bufferSize));
        return;
    }

    CC_LOG_INFO("[DIAG] QueryPool readback buffer OK (size=%llu bytes).",
                 static_cast<unsigned long long>(bufferSize));

    // --- DIAG: Check after readback buffer creation ---
    {
        HRESULT drr = devicePtr->GetDeviceRemovedReason();
        CC_LOG_INFO("[DIAG] QueryPool after readback buffer: DeviceRemovedReason=0x%08x (%s)",
                     static_cast<unsigned>(drr), SUCCEEDED(drr) ? "OK" : "HUNG!");
    }

    CC_LOG_INFO("D3D12 QueryPool initialized: type=%u, maxQueries=%u", static_cast<unsigned>(_type), _maxQueryObjects);
}

void CCD3D12QueryPool::doDestroy() {
    _impl->readbackBuffer.Reset();
    _impl->queryHeap.Reset();
}

void *CCD3D12QueryPool::getD3D12QueryHeap() const {
    return _impl ? _impl->queryHeap.Get() : nullptr;
}

void CCD3D12QueryPool::fetchResults() {
    if (!_impl->queryHeap || !_impl->readbackBuffer) {
        return;
    }

    auto *devicePtr = static_cast<ID3D12Device *>(CCD3D12Device::getInstance()->getD3D12DeviceHandle());
    auto *queuePtr = static_cast<ID3D12CommandQueue *>(CCD3D12Device::getInstance()->getGraphicsQueueHandle());
    if (!devicePtr || !queuePtr) {
        return;
    }

    // Resolve query data to readback buffer
    // This must be called after EndQuery has been recorded and the command list executed.
    // For simplicity we use a separate command list here.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    HRESULT hr = devicePtr->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool fetchResults: CreateCommandAllocator failed.");
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList;
    hr = devicePtr->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(), nullptr,
        IID_PPV_ARGS(&cmdList));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool fetchResults: CreateCommandList failed.");
        return;
    }

    D3D12_QUERY_TYPE queryType = (_type == QueryType::OCCLUSION)
                                     ? D3D12_QUERY_TYPE_OCCLUSION
                                     : D3D12_QUERY_TYPE_OCCLUSION; // extend for other types

    cmdList->ResolveQueryData(
        _impl->queryHeap.Get(),
        queryType,
        0, // start index
        _maxQueryObjects,
        _impl->readbackBuffer.Get(),
        0); // aligned offset

    hr = cmdList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool fetchResults: Close failed.");
        return;
    }

    ID3D12CommandList *lists[] = {cmdList.Get()};
    queuePtr->ExecuteCommandLists(1, lists);

    // Wait for GPU to finish
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    hr = devicePtr->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12QueryPool fetchResults: CreateFence failed.");
        return;
    }

    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent) {
        CC_LOG_ERROR("D3D12QueryPool fetchResults: CreateEvent failed.");
        return;
    }

    hr = queuePtr->Signal(fence.Get(), 1);
    if (FAILED(hr)) {
        CloseHandle(fenceEvent);
        return;
    }

    if (fence->GetCompletedValue() < 1) {
        hr = fence->SetEventOnCompletion(1, fenceEvent);
        if (SUCCEEDED(hr)) {
            WaitForSingleObject(fenceEvent, 5000); // 5s timeout
        }
    }
    CloseHandle(fenceEvent);

    // Map readback buffer and read results
    void *mappedData = nullptr;
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(_maxQueryObjects) * sizeof(uint64_t)};
    hr = _impl->readbackBuffer->Map(0, &readRange, &mappedData);
    if (FAILED(hr) || !mappedData) {
        CC_LOG_ERROR("D3D12QueryPool fetchResults: Map readback buffer failed.");
        return;
    }

    const auto *results = static_cast<const uint64_t *>(mappedData);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _results.clear();
        for (uint32_t i = 0; i < _maxQueryObjects; ++i) {
            if (results[i] != 0) { // Only store non-zero results (query was used)
                _results[i] = results[i];
            }
        }
    }

    D3D12_RANGE writeRange{0, 0};
    _impl->readbackBuffer->Unmap(0, &writeRange);

}

} // namespace gfx
} // namespace cc
