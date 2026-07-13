/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do
 so, subject to the following conditions:

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

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace cc {
namespace gfx {
namespace detail {

struct D3D12ShaderCacheLookupPolicy final {
    bool probeLegacyV3{false};
    bool deferLegacyFilePersistence{false};
};

constexpr D3D12ShaderCacheLookupPolicy getD3D12ShaderCacheLookupPolicy(bool cacheOnly) {
    // Speculative work may warm only the current cache. Legacy compatibility
    // is demand-driven so unused variants never read, canonicalize, or migrate v3.
    const bool demanded = !cacheOnly;
    return {demanded, demanded};
}

// File persistence is deliberately separate from shader compilation workers:
// slow write-through I/O must not occupy a compile slot. Destruction drains the
// queue so an accepted migration is never abandoned during shutdown.
class D3D12ShaderCachePersistenceQueue final {
public:
    using TaskFunction = std::function<void()>;

    explicit D3D12ShaderCachePersistenceQueue(uint32_t workerCount = 1) {
        workerCount = std::max(1U, workerCount);
        _workers.reserve(workerCount);
        for (uint32_t i = 0; i < workerCount; ++i) {
            _workers.emplace_back([this]() { workerLoop(); });
        }
    }

    ~D3D12ShaderCachePersistenceQueue() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _accepting = false;
            _stopping = true;
        }
        _workCondition.notify_all();
        for (auto &worker : _workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    D3D12ShaderCachePersistenceQueue(const D3D12ShaderCachePersistenceQueue &) = delete;
    D3D12ShaderCachePersistenceQueue(D3D12ShaderCachePersistenceQueue &&) = delete;
    D3D12ShaderCachePersistenceQueue &operator=(const D3D12ShaderCachePersistenceQueue &) = delete;
    D3D12ShaderCachePersistenceQueue &operator=(D3D12ShaderCachePersistenceQueue &&) = delete;

    bool enqueue(TaskFunction task) {
        if (!task) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_accepting || _stopping) {
                return false;
            }
            _queue.emplace_back(std::move(task));
        }
        _workCondition.notify_one();
        return true;
    }

    void drain() {
        std::unique_lock<std::mutex> lock(_mutex);
        _drainedCondition.wait(lock, [this]() {
            return _queue.empty() && _activeWorkers == 0;
        });
    }

    void closeAndDrain() {
        std::unique_lock<std::mutex> lock(_mutex);
        _accepting = false;
        _drainedCondition.wait(lock, [this]() {
            return _queue.empty() && _activeWorkers == 0;
        });
    }

    void reopen() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_stopping) {
            _accepting = true;
        }
    }

    template <class Rep, class Period>
    bool drainFor(const std::chrono::duration<Rep, Period> &timeout) {
        std::unique_lock<std::mutex> lock(_mutex);
        return _drainedCondition.wait_for(lock, timeout, [this]() {
            return _queue.empty() && _activeWorkers == 0;
        });
    }

private:
    void workerLoop() {
        for (;;) {
            TaskFunction task;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _workCondition.wait(lock, [this]() { return _stopping || !_queue.empty(); });
                if (_queue.empty()) {
                    if (_stopping) {
                        return;
                    }
                    continue;
                }
                task = std::move(_queue.front());
                _queue.pop_front();
                ++_activeWorkers;
            }

            try {
                task();
            } catch (...) {
                // Cache persistence is best-effort; the runtime bytecode remains valid.
            }

            {
                std::lock_guard<std::mutex> lock(_mutex);
                --_activeWorkers;
                if (_queue.empty() && _activeWorkers == 0) {
                    _drainedCondition.notify_all();
                }
            }
        }
    }

    std::mutex _mutex;
    std::condition_variable _workCondition;
    std::condition_variable _drainedCondition;
    std::deque<TaskFunction> _queue;
    std::vector<std::thread> _workers;
    uint32_t _activeWorkers{0};
    bool _accepting{true};
    bool _stopping{false};
};

} // namespace detail
} // namespace gfx
} // namespace cc
