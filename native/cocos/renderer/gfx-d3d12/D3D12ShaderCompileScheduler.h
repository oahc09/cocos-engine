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
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace cc {
namespace gfx {
namespace detail {

// Fixed background workers prevent glslang oversubscription. A demanded task can
// be claimed from the queue and executed by one foreground caller, reserving the
// fourth compile slot for the render-critical path.
class D3D12ShaderCompileScheduler final {
public:
    using TaskFunction = std::function<bool(bool demanded)>;

private:
    enum class TaskStatus : uint8_t {
        QUEUED,
        RUNNING,
        COMPLETED,
        CANCELLED,
    };

    struct TaskState final {
        explicit TaskState(TaskFunction &&taskIn)
        : task(std::move(taskIn)) {}

        TaskFunction task;
        std::condition_variable completedCondition;
        TaskStatus status{TaskStatus::QUEUED};
        bool result{false};
    };

public:
    using Handle = std::shared_ptr<TaskState>;

    explicit D3D12ShaderCompileScheduler(uint32_t backgroundWorkerCount) {
        const uint32_t workerCount = std::max(1U, backgroundWorkerCount);
        _workers.reserve(workerCount);
        for (uint32_t i = 0; i < workerCount; ++i) {
            _workers.emplace_back([this]() { workerLoop(); });
        }
    }

    ~D3D12ShaderCompileScheduler() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stopping = true;
            for (const auto &task : _queue) {
                if (task->status == TaskStatus::QUEUED) {
                    task->status = TaskStatus::CANCELLED;
                    task->task = {};
                    task->completedCondition.notify_all();
                }
            }
            _queue.clear();
        }
        _workCondition.notify_all();
        for (auto &worker : _workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    D3D12ShaderCompileScheduler(const D3D12ShaderCompileScheduler &) = delete;
    D3D12ShaderCompileScheduler(D3D12ShaderCompileScheduler &&) = delete;
    D3D12ShaderCompileScheduler &operator=(const D3D12ShaderCompileScheduler &) = delete;
    D3D12ShaderCompileScheduler &operator=(D3D12ShaderCompileScheduler &&) = delete;

    Handle submit(TaskFunction taskFunction) {
        auto task = std::make_shared<TaskState>(std::move(taskFunction));
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_stopping) {
                task->status = TaskStatus::CANCELLED;
                return task;
            }
            _queue.emplace_back(task);
        }
        _workCondition.notify_one();
        return task;
    }

    bool runNowOrWait(const Handle &task, bool &ranInline) {
        ranInline = false;
        if (!task) {
            return false;
        }

        // A task already owned by a worker (or already completed) must never wait
        // behind an unrelated foreground compile.
        {
            std::unique_lock<std::mutex> lock(_mutex);
            if (task->status == TaskStatus::COMPLETED) {
                return task->result;
            }
            if (task->status == TaskStatus::CANCELLED) {
                return false;
            }
            if (task->status == TaskStatus::RUNNING) {
                task->completedCondition.wait(lock, [&task]() {
                    return task->status == TaskStatus::COMPLETED || task->status == TaskStatus::CANCELLED;
                });
                return task->status == TaskStatus::COMPLETED && task->result;
            }
        }

        // Only one queued demand may supplement the fixed background pool. Do
        // not block on this slot: if it is busy, promote the task so a worker can
        // service its cache probe while the unrelated foreground compile runs.
        std::unique_lock<std::mutex> foregroundLock(_foregroundMutex, std::try_to_lock);
        bool promoted = false;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (task->status == TaskStatus::COMPLETED) {
                return task->result;
            }
            if (task->status == TaskStatus::CANCELLED) {
                return false;
            }
            if (task->status == TaskStatus::QUEUED) {
                const auto iter = std::find(_queue.begin(), _queue.end(), task);
                if (iter != _queue.end()) {
                    _queue.erase(iter);
                    if (foregroundLock.owns_lock()) {
                        task->status = TaskStatus::RUNNING;
                        ranInline = true;
                    } else {
                        _queue.emplace_front(task);
                        promoted = true;
                    }
                }
            }
        }

        if (ranInline) {
            executeTask(task, true);
            std::lock_guard<std::mutex> lock(_mutex);
            return task->result;
        }

        if (foregroundLock.owns_lock()) {
            foregroundLock.unlock();
        }
        if (promoted) {
            _workCondition.notify_one();
        }
        std::unique_lock<std::mutex> lock(_mutex);
        task->completedCondition.wait(lock, [&task]() {
            return task->status == TaskStatus::COMPLETED || task->status == TaskStatus::CANCELLED;
        });
        return task->status == TaskStatus::COMPLETED && task->result;
    }

    void cancelAndWait(const Handle &task) {
        if (!task) {
            return;
        }

        std::unique_lock<std::mutex> lock(_mutex);
        if (task->status == TaskStatus::QUEUED) {
            const auto iter = std::find(_queue.begin(), _queue.end(), task);
            if (iter != _queue.end()) {
                _queue.erase(iter);
            }
            task->status = TaskStatus::CANCELLED;
            task->task = {};
            task->completedCondition.notify_all();
            return;
        }
        task->completedCondition.wait(lock, [&task]() {
            return task->status == TaskStatus::COMPLETED || task->status == TaskStatus::CANCELLED;
        });
    }

private:
    void workerLoop() {
        for (;;) {
            Handle task;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _workCondition.wait(lock, [this]() { return _stopping || !_queue.empty(); });
                if (_stopping && _queue.empty()) {
                    return;
                }
                task = _queue.front();
                _queue.pop_front();
                if (task->status != TaskStatus::QUEUED) {
                    continue;
                }
                task->status = TaskStatus::RUNNING;
            }
            executeTask(task, false);
        }
    }

    void executeTask(const Handle &task, bool demanded) {
        bool result = false;
        try {
            result = task->task(demanded);
        } catch (...) {
            result = false;
        }

        {
            std::lock_guard<std::mutex> lock(_mutex);
            task->result = result;
            task->status = TaskStatus::COMPLETED;
            task->task = {};
        }
        task->completedCondition.notify_all();
    }

    std::mutex _mutex;
    std::mutex _foregroundMutex;
    std::condition_variable _workCondition;
    std::deque<Handle> _queue;
    std::vector<std::thread> _workers;
    bool _stopping{false};
};

} // namespace detail
} // namespace gfx
} // namespace cc
