/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

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

#pragma once

#include "core/components/ThreeBucketArray.h"

namespace cc {

class Component;

/**
 * @en ComponentScheduler manages the execution order of components.
 * Uses ThreeBucketArray for efficient scheduling based on executionOrder.
 * @zh 组件调度器，使用 ThreeBucketArray 按 executionOrder 高效调度组件执行。
 */
class ComponentScheduler final {
public:
    ComponentScheduler() = default;
    ~ComponentScheduler() = default;

    // ---- Start phase ----
    void addStart(Component *comp);
    void removeStart(Component *comp);
    void invokeStart();

    // ---- Update phase ----
    void addUpdate(Component *comp, int32_t executionOrder = 0);
    void removeUpdate(Component *comp);
    void invokeUpdate(float dt);

    // ---- Late update phase ----
    void addLateUpdate(Component *comp, int32_t executionOrder = 0);
    void removeLateUpdate(Component *comp);
    void invokeLateUpdate(float dt);

    // ---- Lifecycle registration helpers ----
    void registerComponent(Component *comp);
    void unregisterComponent(Component *comp);

    // ---- Clear all ----
    void clear();

private:
    ThreeBucketArray<Component> _startList;
    ThreeBucketArray<Component> _updateList;
    ThreeBucketArray<Component> _lateUpdateList;

    bool _invokingStart{false};
    bool _invokingUpdate{false};
    bool _invokingLateUpdate{false};
};

} // namespace cc
