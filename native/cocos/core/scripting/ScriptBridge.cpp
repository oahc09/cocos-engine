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

#include "core/scripting/ScriptBridge.h"

#include "bindings/jswrapper/SeApi.h"
#include "core/assets/Asset.h"
#include "core/components/ScriptComponent.h"
#include "core/serialization/TypeRegistry.h"

namespace cc {

ScriptBridge &ScriptBridge::getInstance() {
    static ScriptBridge instance;
    return instance;
}

// ========================================================================
// Initialization
// ========================================================================

void ScriptBridge::init(se::Object *globalObj) {
    _globalObj = globalObj;
    _nextCompId = 1;
    _instances.clear();

    // Look up the JS-side batch executor function: __scriptBridgeBatchCall
    // This function is expected to be installed by the TS runtime layer.
    // Signature: __scriptBridgeBatchCall(compIds: number[], method: string, dt?: number)
    if (_globalObj) {
        se::Value batchFnVal;
        _globalObj->getProperty("__scriptBridgeBatchCall", &batchFnVal);
        if (batchFnVal.isObject() && batchFnVal.toObject()->isFunction()) {
            _batchCallFn = batchFnVal.toObject();
            // Prevent GC from collecting the function reference
            _globalObj->attachObject(_batchCallFn);
        }
    }
}

void ScriptBridge::shutdown() {
    _instances.clear();
    if (_batchCallFn && _globalObj) {
        _globalObj->detachObject(_batchCallFn);
    }
    _batchCallFn = nullptr;
    _globalObj = nullptr;
}

// ========================================================================
// Lifecycle callbacks - batch invocation
// ========================================================================

void ScriptBridge::invokeStartBatch(const ccstd::vector<uint32_t> &compIds) {
    callJSBatchMethod(compIds, "start");
}

void ScriptBridge::invokeUpdateBatch(const ccstd::vector<uint32_t> &compIds, float dt) {
    callJSBatchMethod(compIds, "update", dt);
}

void ScriptBridge::invokeLateUpdateBatch(const ccstd::vector<uint32_t> &compIds, float dt) {
    callJSBatchMethod(compIds, "lateUpdate", dt);
}

// ========================================================================
// Lifecycle callbacks - single invocation
// ========================================================================

void ScriptBridge::invokeOnDestroy(uint32_t compId) {
    callJSMethod(compId, "onDestroy");
}

void ScriptBridge::invokeOnEnable(uint32_t compId) {
    callJSMethod(compId, "onEnable");
}

void ScriptBridge::invokeOnDisable(uint32_t compId) {
    callJSMethod(compId, "onDisable");
}

void ScriptBridge::invokeOnLoad(uint32_t compId) {
    callJSMethod(compId, "onLoad");
}

// ========================================================================
// Script class registration
// ========================================================================

uint32_t ScriptBridge::registerScriptClass(const ccstd::string &className,
                                           bool hasStart, bool hasUpdate,
                                           bool hasLateUpdate, bool hasOnLoad,
                                           bool hasOnDestroy, bool hasOnEnable,
                                           bool hasOnDisable,
                                           int32_t executionOrder,
                                           uint32_t requireComponent,
                                           bool disallowMultiple) {
    // Delegate to TypeRegistry's registerScriptType
    // Note: TypeRegistry::registerScriptType currently only stores hasUpdate/hasLateUpdate/executionOrder.
    // Extended fields (hasStart, hasOnLoad, etc.) are stored in the TypeInfo and will be
    // populated once TypeRegistry is extended (post M6-S1).
    auto classId = TypeRegistry::getInstance().registerScriptType(
        className, hasUpdate, hasLateUpdate, executionOrder);

    // Update the TypeInfo with additional lifecycle flags
    auto *typeInfo = TypeRegistry::getInstance().getTypeInfo(classId);
    if (typeInfo) {
        // These fields exist in TypeInfo but registerScriptType doesn't set them yet.
        // We patch them here for completeness.
        const_cast<TypeRegistry::TypeInfo *>(typeInfo)->hasUpdate = hasUpdate;
        const_cast<TypeRegistry::TypeInfo *>(typeInfo)->hasLateUpdate = hasLateUpdate;
        const_cast<TypeRegistry::TypeInfo *>(typeInfo)->executionOrder = executionOrder;
        const_cast<TypeRegistry::TypeInfo *>(typeInfo)->requireComponent = requireComponent;
        const_cast<TypeRegistry::TypeInfo *>(typeInfo)->disallowMultiple = disallowMultiple;
        // hasStart/hasOnLoad/hasOnDestroy/hasOnEnable/hasOnDisable are not in TypeInfo yet.
        // They will be added in a follow-up phase. For now, ScriptComponent reads these
        // from its own flags set by the JS layer.
    }

    return classId;
}

// ========================================================================
// Script component instance registration
// ========================================================================

uint32_t ScriptBridge::registerScriptInstance(se::Object *jsComp, ScriptComponent *scriptComp,
                                              const ccstd::string &className) {
    uint32_t compId = _nextCompId++;

    ScriptInstanceInfo info;
    info.jsObject = jsComp;
    info.scriptComp = scriptComp;
    info.className = className;

    // Look up the type ID from TypeRegistry
    info.typeId = TypeRegistry::getInstance().getClassIdByName(className);

    _instances[compId] = info;

    // Prevent GC from collecting the JS component object while it's registered
    if (jsComp && _globalObj) {
        _globalObj->attachObject(jsComp);
    }

    return compId;
}

void ScriptBridge::unregisterScriptInstance(uint32_t compId) {
    auto it = _instances.find(compId);
    if (it == _instances.end()) {
        return;
    }

    // Allow GC to collect the JS component object
    if (it->second.jsObject && _globalObj) {
        _globalObj->detachObject(it->second.jsObject);
    }

    _instances.erase(it);
}

// ========================================================================
// Asset reference collection
// ========================================================================

ccstd::vector<Asset *> ScriptBridge::collectAssetRefs(uint32_t compId) {
    auto it = _instances.find(compId);
    if (it == _instances.end() || !it->second.scriptComp) {
        return {};
    }

    // Delegate to ScriptComponent's getAssetProperties if available
    if (!it->second.scriptComp->isDead()) {
        return it->second.scriptComp->getAssetProperties();
    }

    // TODO: Call JS-side __collectAssetRefs(compId) for user-script properties
    // once the TS runtime layer implements it. (G-8 safeCallJS pattern)
    return {};
}

// ========================================================================
// instanceof check
// ========================================================================

bool ScriptBridge::isInstanceOf(uint32_t compId, const ccstd::string &className) {
    auto it = _instances.find(compId);
    if (it == _instances.end()) {
        return false;
    }

    // Direct class name comparison
    if (it->second.className == className) {
        return true;
    }

    // Walk up the TypeRegistry inheritance chain (if available)
    auto *typeInfo = TypeRegistry::getInstance().getTypeInfo(it->second.typeId);
    if (typeInfo && typeInfo->className == className) {
        return true;
    }

    // TODO: Full prototype chain check via JS instanceof.
    // For now, only exact class name match is supported.
    return false;
}

// ========================================================================
// Internal helpers
// ========================================================================

void ScriptBridge::callJSMethod(uint32_t compId, const char *method) {
    auto it = _instances.find(compId);
    if (it == _instances.end()) {
        return;
    }

    se::Object *jsObj = it->second.jsObject;
    if (!jsObj) {
        // JS object was garbage collected or never bound (G-8)
        return;
    }

    se::Value methodVal;
    jsObj->getProperty(method, &methodVal);
    if (!methodVal.isObject() || !methodVal.toObject()->isFunction()) {
        // Method doesn't exist on this JS object — not an error for optional lifecycle methods
        return;
    }

    se::ValueArray args;
    se::Value rval;
    methodVal.toObject()->call(args, jsObj, &rval);
}

void ScriptBridge::callJSBatchMethod(const ccstd::vector<uint32_t> &compIds, const char *method, float dt) {
    if (compIds.empty()) {
        return;
    }

    if (!_batchCallFn) {
        // Fallback: call each instance individually
        for (uint32_t compId : compIds) {
            if (strcmp(method, "start") == 0) {
                callJSMethod(compId, "start");
            } else if (strcmp(method, "update") == 0) {
                // For update/lateUpdate, we need to pass dt — use direct JS call
                auto it = _instances.find(compId);
                if (it == _instances.end() || !it->second.jsObject) continue;

                se::Value methodVal;
                it->second.jsObject->getProperty(method, &methodVal);
                if (!methodVal.isObject() || !methodVal.toObject()->isFunction()) continue;

                se::ValueArray args;
                args.emplace_back(dt);
                methodVal.toObject()->call(args, it->second.jsObject);
            } else if (strcmp(method, "lateUpdate") == 0) {
                auto it = _instances.find(compId);
                if (it == _instances.end() || !it->second.jsObject) continue;

                se::Value methodVal;
                it->second.jsObject->getProperty(method, &methodVal);
                if (!methodVal.isObject() || !methodVal.toObject()->isFunction()) continue;

                se::ValueArray args;
                args.emplace_back(dt);
                methodVal.toObject()->call(args, it->second.jsObject);
            }
        }
        return;
    }

    // Build the arguments array for the batch executor:
    // __scriptBridgeBatchCall(compIds: number[], method: string, dt?: number)
    se::ValueArray args;

    // arg0: compIds as a JS array
    se::HandleObject compIdsArray(se::Object::createArrayObject(static_cast<uint32_t>(compIds.size())));
    for (size_t i = 0; i < compIds.size(); ++i) {
        compIdsArray->setArrayElement(static_cast<uint32_t>(i), se::Value(compIds[i]));
    }
    args.emplace_back(se::Value(compIdsArray.get(), true));

    // arg1: method name
    args.emplace_back(se::Value(method));

    // arg2: dt (only for update/lateUpdate)
    if (dt != 0.0f) {
        args.emplace_back(se::Value(dt));
    }

    se::Value rval;
    _batchCallFn->call(args, nullptr, &rval);
}

} // namespace cc
