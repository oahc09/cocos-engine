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

#include "D3D12CommandBuffer.h"
#include "D3D12Device.h"
#include "D3D12Queue.h"
#include "base/Log.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <wrl/client.h>

namespace {
#ifndef NDEBUG
void dumpQueueDebugMessages(ID3D12Device *device, const char *checkpoint) {
    if (!device) {
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
        return;
    }
    const UINT64 msgCount = infoQueue->GetNumStoredMessages();
    if (msgCount == 0) {
        return;
    }
    CC_LOG_INFO("[QUEUE-DIAG] %s: %llu pending messages", checkpoint, static_cast<unsigned long long>(msgCount));
    for (UINT64 i = 0; i < msgCount; ++i) {
        SIZE_T msgSize = 0;
        infoQueue->GetMessage(i, nullptr, &msgSize);
        if (msgSize == 0) {
            continue;
        }
        ccstd::vector<uint8_t> storage(msgSize);
        auto *msg = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (SUCCEEDED(infoQueue->GetMessage(i, msg, &msgSize))) {
            CC_LOG_INFO("[QUEUE-DIAG]   ID=%u severity=%d: %.*s",
                        static_cast<unsigned>(msg->ID),
                        static_cast<int>(msg->Severity),
                        static_cast<int>(msg->DescriptionByteLength),
                        msg->pDescription);
        }
    }
    infoQueue->ClearStoredMessages();
}
#else
void dumpQueueDebugMessages(ID3D12Device *, const char *) {}
#endif
} // namespace

namespace cc {
namespace gfx {

struct CCD3D12Queue::Impl {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent{nullptr};
    uint64_t fenceValue{0};
};

CCD3D12Queue::CCD3D12Queue()
: _impl(std::make_unique<Impl>()) {
}

CCD3D12Queue::~CCD3D12Queue() = default;

void CCD3D12Queue::doInit(const QueueInfo &info) {
    (void)info;
    auto *device = CCD3D12Device::getInstance();
    if (!device) {
        CC_LOG_ERROR("D3D12Queue: device not available.");
        return;
    }

    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12Queue: D3D12 device handle is null.");
        return;
    }

    HRESULT hr = d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_impl->fence));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12Queue: CreateFence failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    _impl->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!_impl->fenceEvent) {
        CC_LOG_ERROR("D3D12Queue: CreateEvent failed.");
        return;
    }

    _impl->fenceValue = 0;

    CC_LOG_INFO("D3D12Queue initialized.");
}

void CCD3D12Queue::doDestroy() {
    if (_impl->fenceEvent) {
        CloseHandle(_impl->fenceEvent);
        _impl->fenceEvent = nullptr;
    }
    _impl->fence.Reset();
}

void CCD3D12Queue::submit(CommandBuffer *const *cmdBuffs, uint32_t count) {
    auto *device = CCD3D12Device::getInstance();
    if (!device) {
        CC_LOG_ERROR("D3D12Queue::submit - device is null.");
        return;
    }

    auto *graphicsQueue = static_cast<ID3D12CommandQueue *>(device->getGraphicsQueueHandle());
    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    if (!graphicsQueue) {
        CC_LOG_ERROR("D3D12Queue::submit - graphics queue is null.");
        return;
    }

    if (count == 0 || !cmdBuffs) return;

    // Collect command lists from the command buffers
    ccstd::vector<ID3D12CommandList *> commandLists;
    commandLists.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        if (!cmdBuffs[i]) continue;
        auto *d3d12CmdBuf = static_cast<CCD3D12CommandBuffer *>(cmdBuffs[i]);
        auto *cmdList = static_cast<ID3D12CommandList *>(d3d12CmdBuf->getD3D12CommandList());
        if (cmdList) {
            commandLists.push_back(cmdList);
        }
    }

    if (commandLists.empty()) return;

    // Execute command lists
    graphicsQueue->ExecuteCommandLists(static_cast<UINT>(commandLists.size()), commandLists.data());
    dumpQueueDebugMessages(d3dDevice, "after-execute");

    // Signal fence and wait (synchronous submit for PoC)
    if (_impl->fence && _impl->fenceEvent) {
        ++_impl->fenceValue;
        HRESULT hr = graphicsQueue->Signal(_impl->fence.Get(), _impl->fenceValue);
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12Queue::submit - Signal failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return;
        }

        if (_impl->fence->GetCompletedValue() < _impl->fenceValue) {
            hr = _impl->fence->SetEventOnCompletion(_impl->fenceValue, _impl->fenceEvent);
            if (SUCCEEDED(hr)) {
                DWORD waitResult = WaitForSingleObject(_impl->fenceEvent, 5000);
                if (waitResult == WAIT_TIMEOUT) {
                    CC_LOG_ERROR("D3D12Queue::submit - fence wait timed out (5s). GPU may be hung.");
                    if (d3dDevice) {
                        HRESULT deviceHR = d3dDevice->GetDeviceRemovedReason();
                        CC_LOG_ERROR("D3D12 device removed reason: 0x%08x", static_cast<unsigned>(deviceHR));
                    }
                } else if (waitResult == WAIT_FAILED) {
                    CC_LOG_ERROR("D3D12Queue::submit - WaitForSingleObject failed. errno=%u", static_cast<unsigned>(GetLastError()));
                }
            }
        }
        dumpQueueDebugMessages(d3dDevice, "after-fence-wait");
    }
}

} // namespace gfx
} // namespace cc
