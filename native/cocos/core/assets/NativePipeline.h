/****************************************************************************
 Copyright (c) 2021-2023 Xiamen Yaji Software Co., Ltd.

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

#pragma once

#include <functional>

#include "base/Macros.h"
#include "base/RefCounted.h"
#include "base/Ptr.h"
#include "base/std/container/string.h"
#include "base/std/container/vector.h"
#include "base/std/container/unordered_map.h"

namespace cc {

class Asset;

/**
 * @en Pipeline task for asset loading.
 * @zh 资产加载管线任务。
 */
struct PipelineTask {
    ccstd::string taskId;           // Unique task identifier
    ccstd::string path;             // Asset path
    ccstd::string uuid;             // Asset UUID
    ccstd::vector<uint8_t> data;    // Downloaded raw data
    IntrusivePtr<Asset> output;     // Output asset
    bool isComplete{false};
    bool hasError{false};
    ccstd::string errorMessage;
};

/**
 * @en A single processing step in the pipeline.
 * @zh 管线中的单个处理步骤。
 */
class PipelineHandler : public RefCounted {
public:
    /**
     * Process the task. Return true to continue, false to abort.
     */
    virtual bool handle(PipelineTask &task) = 0;
    virtual ~PipelineHandler() = default;
};

/**
 * @en NativePipeline provides C++ implementation of the asset loading pipeline.
 * Supports both synchronous and asynchronous execution modes.
 * @zh NativePipeline 提供 C++ 资产加载管线实现。支持同步和异步执行模式。
 *
 * Design: Dual-mode routing
 * - JS_ONLY mode: delegates to JS Pipeline (callJSLoad)
 * - NATIVE_FAST mode: executes C++ handlers directly (loadNativeAsset)
 */
class NativePipeline final : public RefCounted {
public:
    static NativePipeline *getInstance();

    // === Pipeline construction ===

    /**
     * Insert a handler at the specified position.
     * @param name Handler name (unique identifier)
     * @param handler The processing step
     * @param index Position in the pipeline (-1 = append)
     */
    void insert(const ccstd::string &name, IntrusivePtr<PipelineHandler> handler, int32_t index = -1);

    /**
     * Remove a handler by name.
     */
    void removeHandler(const ccstd::string &name);

    // === Synchronous execution ===

    /**
     * Execute pipeline synchronously. Blocks until all handlers complete.
     * @param task The task to process
     * @return true if all handlers succeeded
     */
    bool executeSync(PipelineTask &task);

    // === Asynchronous execution ===

    using CompleteCallback = std::function<void(PipelineTask &)>;

    /**
     * Execute pipeline asynchronously.
     * @param task The task to process
     * @param onComplete Called when pipeline completes (success or failure)
     */
    void executeAsync(PipelineTask &task, CompleteCallback onComplete);

    // === Mode switching ===

    enum class Mode : uint8_t {
        JS_ONLY = 0,       // Editor mode: delegate to JS Pipeline
        NATIVE_FAST = 1,   // Runtime mode: C++ fast path
    };

    void setMode(Mode mode);
    Mode getMode() const { return _mode; }

    // === Handler access ===

    uint32_t getHandlerCount() const;
    bool hasHandler(const ccstd::string &name) const;

private:
    NativePipeline() = default;
    ~NativePipeline() = default;

    struct HandlerEntry {
        ccstd::string name;
        IntrusivePtr<PipelineHandler> handler;
    };

    ccstd::vector<HandlerEntry> _handlers;
    Mode _mode{Mode::JS_ONLY};

    static NativePipeline *s_instance;

    CC_DISALLOW_COPY_MOVE_ASSIGN(NativePipeline);
};

} // namespace cc
