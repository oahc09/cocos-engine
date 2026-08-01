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
#include "D3D12DebugOptimization.h"
#include "base/Log.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <algorithm>
    #include <chrono>
    #include <d3d12.h>
    #include <wrl/client.h>

namespace {

} // namespace

namespace cc {
namespace gfx {

struct CCD3D12Queue::Impl {
    using RetireCallback = void (*)(const std::shared_ptr<void> &);

    struct RetainedContext {
        std::shared_ptr<void> context;
        RetireCallback retire{nullptr};
    };

    struct InFlightContext {
        uint64_t fenceValue{0};
        RetainedContext retained;
    };

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent{nullptr};
    uint64_t fenceValue{0};
    ccstd::vector<InFlightContext> inFlightContexts;
    ccstd::vector<RetainedContext> untrackedContexts;
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

    CC_D3D12_DIAGNOSTIC_LOG("D3D12Queue initialized.");
}

void CCD3D12Queue::doDestroy() {
    retireSubmittedContexts(true);
    if (_impl->fenceEvent) {
        CloseHandle(_impl->fenceEvent);
        _impl->fenceEvent = nullptr;
    }
    _impl->fence.Reset();
}

void CCD3D12Queue::retireSubmittedContexts(bool allCompleted) {
    if (!_impl) {
        return;
    }
    if (allCompleted) {
        for (const auto &entry : _impl->inFlightContexts) {
            if (entry.retained.retire) {
                entry.retained.retire(entry.retained.context);
            }
        }
        for (const auto &retained : _impl->untrackedContexts) {
            if (retained.retire) {
                retained.retire(retained.context);
            }
        }
        _impl->inFlightContexts.clear();
        _impl->untrackedContexts.clear();
        return;
    }
    if (!_impl->fence || _impl->inFlightContexts.empty()) {
        return;
    }
    const uint64_t completedValue = _impl->fence->GetCompletedValue();
    _impl->inFlightContexts.erase(
        std::remove_if(
            _impl->inFlightContexts.begin(), _impl->inFlightContexts.end(),
            [completedValue](const Impl::InFlightContext &entry) {
                if (entry.fenceValue > completedValue) {
                    return false;
                }
                if (entry.retained.retire) {
                    entry.retained.retire(entry.retained.context);
                }
                return true;
            }),
        _impl->inFlightContexts.end());
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

    retireSubmittedContexts(false);
    device->flushDeferredCubeUploads();

    struct Submission {
        CCD3D12CommandBuffer *commandBuffer{nullptr};
        ID3D12CommandList *commandList{nullptr};
        std::shared_ptr<void> context;
    };
    struct StateFixupContext {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    };
    ccstd::vector<Submission> submissions;
    submissions.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        if (!cmdBuffs[i]) continue;
        auto *d3d12CmdBuf = static_cast<CCD3D12CommandBuffer *>(cmdBuffs[i]);
        if (d3d12CmdBuf->getType() != CommandBufferType::PRIMARY) {
            continue;
        }
        auto *cmdList = static_cast<ID3D12CommandList *>(d3d12CmdBuf->getD3D12CommandList());
        if (cmdList) {
            submissions.push_back({
                d3d12CmdBuf,
                cmdList,
                d3d12CmdBuf->getD3D12CommandRecordingContext(),
            });
        }
    }

    if (submissions.empty()) return;
    if (!d3dDevice || !_impl->fence) {
        CC_LOG_ERROR("D3D12Queue::submit - device or submission fence is unavailable.");
        for (const auto &submission : submissions) {
            submission.commandBuffer->notifySubmissionFailed();
        }
        return;
    }

    std::vector<D3D12ResourceStateSnapshot> stateSnapshots;
    for (const auto &submission : submissions) {
        submission.commandBuffer->captureSubmissionResourceStates(stateSnapshots);
    }
    auto restoreResourceStates = [&stateSnapshots]() {
        for (const auto &snapshot : stateSnapshots) {
            if (snapshot.backing) {
                snapshot.backing->states = snapshot.states;
            }
        }
    };

    ccstd::vector<ID3D12CommandList *> commandLists;
    commandLists.reserve(submissions.size() * 2);
    ccstd::vector<std::shared_ptr<StateFixupContext>> stateFixupContexts;
    stateFixupContexts.reserve(submissions.size());
    for (const auto &submission : submissions) {
        std::vector<D3D12_RESOURCE_BARRIER> fixupBarriers;
        if (!submission.commandBuffer->appendSubmissionStateFixupBarriers(fixupBarriers)) {
            CC_LOG_ERROR("D3D12Queue::submit - resource state journal no longer matches its native resource.");
            restoreResourceStates();
            for (const auto &failedSubmission : submissions) {
                failedSubmission.commandBuffer->notifySubmissionFailed();
            }
            return;
        }

        if (!fixupBarriers.empty()) {
            auto fixupContext = std::make_shared<StateFixupContext>();
            HRESULT hr = d3dDevice->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&fixupContext->commandAllocator));
            if (SUCCEEDED(hr)) {
                hr = d3dDevice->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    fixupContext->commandAllocator.Get(), nullptr,
                    IID_PPV_ARGS(&fixupContext->commandList));
            }
            if (SUCCEEDED(hr)) {
                fixupContext->commandList->ResourceBarrier(
                    static_cast<UINT>(fixupBarriers.size()), fixupBarriers.data());
                hr = fixupContext->commandList->Close();
            }
            if (FAILED(hr)) {
                CC_LOG_ERROR("D3D12Queue::submit - failed to record resource-state fixup. HRESULT=0x%08x",
                             static_cast<unsigned>(hr));
                restoreResourceStates();
                for (const auto &failedSubmission : submissions) {
                    failedSubmission.commandBuffer->notifySubmissionFailed();
                }
                return;
            }
            commandLists.push_back(fixupContext->commandList.Get());
            stateFixupContexts.emplace_back(std::move(fixupContext));
        }

        commandLists.push_back(submission.commandList);
        if (!submission.commandBuffer->commitSubmissionResourceStates()) {
            CC_LOG_ERROR("D3D12Queue::submit - failed to commit resource-state journal.");
            restoreResourceStates();
            for (const auto &failedSubmission : submissions) {
                failedSubmission.commandBuffer->notifySubmissionFailed();
            }
            return;
        }
    }

    // Execute command lists
    graphicsQueue->ExecuteCommandLists(static_cast<UINT>(commandLists.size()), commandLists.data());
    // Every buffer decays to COMMON when this ExecuteCommandLists operation
    // completes. All lists in this call share one operation and one epoch.
    device->advanceBufferStateEpoch();

    // Signal completion for allocator/resource reuse. Reuse waits happen lazily
    // in CommandBuffer::begin() and Device::retireFrameResources(), not here.
    if (_impl->fence) {
        ++_impl->fenceValue;
        HRESULT hr = graphicsQueue->Signal(_impl->fence.Get(), _impl->fenceValue);
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12Queue::submit - Signal failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            if (device->waitIdle()) {
                return;
            }
            for (auto &submission : submissions) {
                submission.commandBuffer->notifySubmissionFailed();
                if (submission.context) {
                    _impl->untrackedContexts.push_back({
                        std::move(submission.context),
                        &CCD3D12CommandBuffer::retireD3D12CommandRecordingContext,
                    });
                }
            }
            for (auto &context : stateFixupContexts) {
                _impl->untrackedContexts.push_back({
                    std::static_pointer_cast<void>(std::move(context)),
                    nullptr,
                });
            }
            return;
        }
        for (auto &retained : _impl->untrackedContexts) {
            _impl->inFlightContexts.push_back(
                {_impl->fenceValue, std::move(retained)});
        }
        _impl->untrackedContexts.clear();
        for (auto &context : stateFixupContexts) {
            _impl->inFlightContexts.push_back(
                {_impl->fenceValue,
                 {std::static_pointer_cast<void>(std::move(context)), nullptr}});
        }
        for (auto &submission : submissions) {
            submission.commandBuffer->notifySubmitted(_impl->fence.Get(), _impl->fenceValue);
            if (submission.context) {
                _impl->inFlightContexts.push_back(
                    {_impl->fenceValue,
                     {std::move(submission.context),
                      &CCD3D12CommandBuffer::retireD3D12CommandRecordingContext}});
            }
        }
        device->notifySubmittedFence(_impl->fence.Get(), _impl->fenceValue);
    }
}

} // namespace gfx
} // namespace cc
