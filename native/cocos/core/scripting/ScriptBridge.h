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

#include "base/Macros.h"
#include "base/std/container/string.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"

namespace se {
class Object;
class Value;
} // namespace se

namespace cc {

class ScriptComponent;
class Asset;

/**
 * @en ScriptBridge is the core bridge between C++ engine and JS user scripts.
 * It manages script type registration, script instance lifecycle, and batch
 * JS lifecycle callback execution. (M6-S1)
 * @zh ScriptBridge 是 C++ 引擎与 JS 用户脚本之间的核心桥梁。
 * 负责脚本类型注册、脚本实例生命周期管理、以及批量 JS 生命周期回调执行。(M6-S1)
 */
class ScriptBridge final {
public:
    static ScriptBridge &getInstance();

    // === Initialization ===
    void init(se::Object *globalObj);
    void shutdown();

    // === Lifecycle callbacks (batch invocation) ===
    void invokeStartBatch(const ccstd::vector<uint32_t> &compIds);
    void invokeUpdateBatch(const ccstd::vector<uint32_t> &compIds, float dt);
    void invokeLateUpdateBatch(const ccstd::vector<uint32_t> &compIds, float dt);

    // Single invocation
    void invokeOnDestroy(uint32_t compId);
    void invokeOnEnable(uint32_t compId);
    void invokeOnDisable(uint32_t compId);
    void invokeOnLoad(uint32_t compId);

    // === Script class registration ===
    uint32_t registerScriptClass(const ccstd::string &className,
                                 bool hasStart, bool hasUpdate,
                                 bool hasLateUpdate, bool hasOnLoad,
                                 bool hasOnDestroy, bool hasOnEnable,
                                 bool hasOnDisable,
                                 int32_t executionOrder,
                                 uint32_t requireComponent,
                                 bool disallowMultiple);

    // === Script component instance registration ===
    uint32_t registerScriptInstance(se::Object *jsComp, ScriptComponent *scriptComp,
                                    const ccstd::string &className);
    uint32_t registerScriptInstance(uint32_t compId, se::Object *jsComp,
                                    const ccstd::string &className,
                                    ScriptComponent *scriptComp = nullptr);
    void unregisterScriptInstance(uint32_t compId);

    // === Asset reference collection ===
    ccstd::vector<Asset *> collectAssetRefs(uint32_t compId);

    // === Batch collect asset refs from multiple components ===
    ccstd::vector<Asset *> collectAssetRefsBatch(const ccstd::vector<uint32_t> &compIds);

    // === instanceof check ===
    bool isInstanceOf(uint32_t compId, const ccstd::string &className);
    uint32_t getInstanceCount() const { return static_cast<uint32_t>(_instances.size()); }

private:
    ScriptBridge() = default;
    ~ScriptBridge() = default;

    struct ScriptInstanceInfo {
        se::Object *jsObject{nullptr};
        ScriptComponent *scriptComp{nullptr};
        uint32_t typeId{0};
        ccstd::string className;
    };

    ccstd::unordered_map<uint32_t, ScriptInstanceInfo> _instances;
    uint32_t _nextCompId{1};

    se::Object *_globalObj{nullptr};

    // Cached JS function references
    se::Object *_batchCallFn{nullptr};       // JS side batch executor
    se::Object *_collectRefsFn{nullptr};     // JS side asset ref collector

    // Internal helpers
    void callJSMethod(uint32_t compId, const char *method);
    void callJSBatchMethod(const ccstd::vector<uint32_t> &compIds, const char *method, float dt = 0.0f);

    CC_DISALLOW_COPY_MOVE_ASSIGN(ScriptBridge);
};

} // namespace cc
