/****************************************************************************
 Copyright (c) 2021-2023 Xiamen Yaji Software Co., Ltd.

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

#include "core/assets/NativePipeline.h"

#include "core/assets/Asset.h"

namespace cc {

NativePipeline *NativePipeline::s_instance = nullptr;

NativePipeline *NativePipeline::getInstance() {
    if (s_instance == nullptr) {
        s_instance = ccnew NativePipeline();
    }
    return s_instance;
}

void NativePipeline::insert(const ccstd::string &name, IntrusivePtr<PipelineHandler> handler, int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= _handlers.size()) {
        _handlers.emplace_back(HandlerEntry{name, handler});
    } else {
        _handlers.emplace(_handlers.begin() + index, HandlerEntry{name, handler});
    }
}

void NativePipeline::removeHandler(const ccstd::string &name) {
    for (auto it = _handlers.begin(); it != _handlers.end(); ++it) {
        if (it->name == name) {
            _handlers.erase(it);
            return;
        }
    }
}

bool NativePipeline::executeSync(PipelineTask &task) {
    for (auto &entry : _handlers) {
        if (!entry.handler->handle(task)) {
            task.hasError = true;
            return false;
        }
    }
    task.isComplete = true;
    return true;
}

void NativePipeline::executeAsync(PipelineTask &task, CompleteCallback onComplete) {
    // TODO: Use thread pool for true async execution in the future.
    // For now, execute synchronously and invoke callback on the same thread.
    executeSync(task);
    if (onComplete) {
        onComplete(task);
    }
}

void NativePipeline::setMode(Mode mode) {
    _mode = mode;
}

uint32_t NativePipeline::getHandlerCount() const {
    return static_cast<uint32_t>(_handlers.size());
}

bool NativePipeline::hasHandler(const ccstd::string &name) const {
    for (const auto &entry : _handlers) {
        if (entry.name == name) {
            return true;
        }
    }
    return false;
}

} // namespace cc
