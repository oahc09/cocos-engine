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

#include "core/components/ScriptComponent.h"
#include "bindings/jswrapper/SeApi.h"
#include "core/scripting/ScriptBridge.h"

namespace cc {

ScriptComponent::~ScriptComponent() {
    if (_jsObject) {
        _jsObject->unroot();
        _jsObject = nullptr;
    }
    _dead = true;
}

void ScriptComponent::bindJSObject(se::Object *jsObj) {
    if (_jsObject) {
        _jsObject->unroot();
    }
    _jsObject = jsObj;
    if (_jsObject) {
        _jsObject->root();
    }
    _bound = (_jsObject != nullptr);
}

template <typename Fn>
void ScriptComponent::safeCallJS(Fn &&fn) {
    if (_dead || !_bound || !_jsObject) return;
    // GC safety: check if JS object is still alive (G-8)
    fn(_jsObject);
}

void ScriptComponent::__preload() {
    safeCallJS([](se::Object *jsObj) {
        se::Value fn;
        if (jsObj->getProperty("__preload", &fn) && fn.isObject() && fn.toObject()->isFunction()) {
            se::ValueArray args;
            fn.toObject()->call(args, jsObj);
        }
    });
}

void ScriptComponent::onLoad() {
    safeCallJS([](se::Object *jsObj) {
        se::Value fn;
        if (jsObj->getProperty("onLoad", &fn) && fn.isObject() && fn.toObject()->isFunction()) {
            se::ValueArray args;
            fn.toObject()->call(args, jsObj);
        }
    });
}

void ScriptComponent::start() {
    safeCallJS([](se::Object *jsObj) {
        se::Value fn;
        if (jsObj->getProperty("start", &fn) && fn.isObject() && fn.toObject()->isFunction()) {
            se::ValueArray args;
            fn.toObject()->call(args, jsObj);
        }
    });
}

void ScriptComponent::update(float dt) {
    safeCallJS([dt](se::Object *jsObj) {
        se::Value fn;
        if (jsObj->getProperty("update", &fn) && fn.isObject() && fn.toObject()->isFunction()) {
            se::ValueArray args;
            args.push_back(se::Value(dt));
            fn.toObject()->call(args, jsObj);
        }
    });
}

void ScriptComponent::lateUpdate(float dt) {
    safeCallJS([dt](se::Object *jsObj) {
        se::Value fn;
        if (jsObj->getProperty("lateUpdate", &fn) && fn.isObject() && fn.toObject()->isFunction()) {
            se::ValueArray args;
            args.push_back(se::Value(dt));
            fn.toObject()->call(args, jsObj);
        }
    });
}

void ScriptComponent::onEnable() {
    safeCallJS([](se::Object *jsObj) {
        se::Value fn;
        if (jsObj->getProperty("onEnable", &fn) && fn.isObject() && fn.toObject()->isFunction()) {
            se::ValueArray args;
            fn.toObject()->call(args, jsObj);
        }
    });
}

void ScriptComponent::onDisable() {
    safeCallJS([](se::Object *jsObj) {
        se::Value fn;
        if (jsObj->getProperty("onDisable", &fn) && fn.isObject() && fn.toObject()->isFunction()) {
            se::ValueArray args;
            fn.toObject()->call(args, jsObj);
        }
    });
}

void ScriptComponent::onDestroy() {
    safeCallJS([](se::Object *jsObj) {
        se::Value fn;
        if (jsObj->getProperty("onDestroy", &fn) && fn.isObject() && fn.toObject()->isFunction()) {
            se::ValueArray args;
            fn.toObject()->call(args, jsObj);
        }
    });
    _dead = true;

    // Unregister from ScriptBridge if we have a valid compId (C1)
    if (_compId != 0) {
        ScriptBridge::getInstance().unregisterScriptInstance(_compId);
        _compId = 0;
    }
}

void ScriptComponent::deserializeBinary(const uint8_t *data, uint32_t size) {
    // Script components deserialize via JS bridge, not binary
    // The _serializedProps JSON string is used instead
}

ccstd::vector<Asset *> ScriptComponent::getAssetProperties() {
    // Script component asset references are tracked on the JS side
    // This will be expanded in M6 (ScriptBridge)
    return {};
}

} // namespace cc
