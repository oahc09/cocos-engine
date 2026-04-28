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

#include <functional>
#include "base/std/container/string.h"
#include "base/std/container/vector.h"
#include "core/components/Component.h"

namespace se {
class Object;
}

namespace cc {

/**
 * @en Unified script component placeholder for user scripts. (I-3)
 * Merges ScriptComponent and ScriptComponentPlaceholder into one class.
 * @zh 统一脚本组件占位类，合并生命周期代理与反序列化占位。(I-3)
 */
class ScriptComponent final : public Component {
public:
    ScriptComponent() = default;
    ~ScriptComponent() override;

    bool isBuiltin() const override { return false; }
    uint32_t getComponentTypeId() const override { return _scriptClassId; }

    // ---- Lifecycle proxy → safeCallJS ----
    void __preload() override;
    void onLoad() override;
    void start() override;
    void update(float dt) override;
    void lateUpdate(float dt) override;
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;

    // Lifecycle detection (runtime dynamic)
    bool hasUpdateMethod() const override { return _hasUpdate; }
    bool hasLateUpdateMethod() const override { return _hasLateUpdate; }
    bool hasStartMethod() const override { return _hasStart; }

    // ---- Deserialization placeholder ----
    void setScriptClassPath(const ccstd::string &path) { _scriptClassPath = path; }
    const ccstd::string &getScriptClassPath() const { return _scriptClassPath; }
    void setSerializedProps(const ccstd::string &json) { _serializedProps = json; }
    const ccstd::string &getSerializedProps() const { return _serializedProps; }

    // ---- Script class ID ----
    void setScriptClassId(uint32_t id) { _scriptClassId = id; }
    uint32_t getScriptClassId() const { return _scriptClassId; }

    // ---- ScriptBridge instance ID (set by ScriptBridge::registerScriptInstance) ----
    void setScriptBridgeCompId(uint32_t compId) { _compId = compId; }
    uint32_t getScriptBridgeCompId() const { return _compId; }

    // ---- JS object binding ----
    void bindJSObject(se::Object *jsObj);
    se::Object *getJSObject() const { return _jsObject; }
    bool isBound() const { return _bound; }

    // ---- GC safety ----
    bool isDead() const { return _dead; }

    // ---- Serialization ----
    void deserializeBinary(const uint8_t *data, uint32_t size) override;
    ccstd::vector<Asset *> getAssetProperties() override;

private:
    template <typename Fn>
    void safeCallJS(Fn &&fn);

    uint32_t _scriptClassId{0};
    uint32_t _compId{0}; // ScriptBridge-assigned instance ID (0 = not registered)
    ccstd::string _scriptClassPath;
    ccstd::string _serializedProps;

    se::Object *_jsObject{nullptr};
    bool _bound{false};
    bool _dead{false};
    bool _hasUpdate{false};
    bool _hasLateUpdate{false};
    bool _hasStart{false};
};

} // namespace cc
