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

#include <cstdint>
#include <functional>
#include "base/std/container/string.h"
#include "base/std/container/vector.h"
#include "core/components/BuiltinTypeIds.h"
#include "core/components/ComponentMacros.h"
#include "core/data/Object.h"

namespace cc {

class Node;
class ComponentScheduler;
class NodeActivator;
class Asset;
class Director;

/**
 * @en Component is the base class for everything which can be attached to a Node.
 * @zh 组件是所有可以附加到节点上的对象的基类。
 */
class Component : public CCObject {
    friend class Node;
    friend class ComponentScheduler;
    friend class NodeActivator;

public:
    using Super = CCObject;

    Component() = default;
    ~Component() override;

    // ---- Identity (virtual, overridden by CC_COMPONENT_DECLARE in subclasses) ----
    virtual bool isBuiltin() const { return false; }
    virtual uint32_t getComponentTypeId() const { return BUILTIN_UNKNOWN; }

    // ---- Node access ----
    Node *getNode() const { return _node; }

    // ---- Enabled state ----
    inline bool isEnabled() const { return _enabled; }
    void setEnabled(bool value);

    inline bool isEnabledInHierarchy() const { return _enabledInHierarchy; }

    // ---- Lifecycle virtual functions ----
    virtual void __preload();
    virtual void onLoad();
    virtual void start();
    virtual void update(float dt);
    virtual void lateUpdate(float dt);
    virtual void onEnable();
    virtual void onDisable();
    virtual void onDestroy();

    // ---- Lifecycle method detection (替代函数指针) ----
    virtual bool hasUpdateMethod() const { return false; }
    virtual bool hasLateUpdateMethod() const { return false; }
    virtual bool hasStartMethod() const { return false; }

    // ---- Serialization ----
    virtual void deserializeBinary(const uint8_t *data, uint32_t size);
    virtual ccstd::vector<Asset *> getAssetProperties();

    // ---- Timer proxy ----
    void schedule(const std::function<void(float)> &callback, float interval,
                  unsigned int repeat = 0, float delay = 0.0F, bool paused = false);
    void unschedule(const std::function<void(float)> &callback);

    // ---- Destroy/destruct overrides ----
    bool destroy() override;
    void destruct() override;

    // ---- JS fallback mechanism (G-14) ----
    void setFallbackToJS(bool fallback) { _fallbackToJS = fallback; }
    bool isFallbackToJS() const { return _fallbackToJS; }

protected:
    Node *_node{nullptr};
    bool _enabled{true};
    bool _enabledInHierarchy{false};
    bool _fallbackToJS{false};
    bool _preloaded{false};
    bool _loaded{false};
    bool _started{false};

    // Scheduler registration state
    bool _registeredToScheduler{false};
};

} // namespace cc
