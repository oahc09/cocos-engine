# Cocos Creator v3.8.8 C++ 化实施计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 Cocos Creator v3.8.8 引擎的核心路径 C++ 化，实现双轨制运行时（JS_ONLY 编辑器模式 + NATIVE_FAST 发布模式），目标性能提升 4×（场景加载）和 98% JSB 桥接减少。

**Architecture:** 双轨制架构 — 编辑器保持 JS_ONLY 模式（JSON 反序列化 + JS 反射 + 热重载），运行时走 NATIVE_FAST 模式（二进制反序列化 + TypeRegistry 直接创建 + C++ 调度）。核心不变量：用户脚本不可 C++ 化、引用计数 C++ 权威源、双模式运行时不可切换。四大子系统（Component/Scene/Asset/Script）通过统一 TypeRegistry 和双层 ComponentScheduler 协同工作。

**Tech Stack:** C++17, TypeScript 4.9.5, V8/JSCore JS 引擎, se::Object JSB 框架, CMake + Ninja, ccstd 容器库

**权威设计文档:**
- `AI/design-cpp-master-spec.md` v2.0 (948行, 总体规范)
- `AI/design-cpp-component-framework.md` v2.0 (组件框架)
- `AI/design-cpp-scene-system.md` v2.0 (场景系统)
- `AI/design-cpp-asset-system.md` v2.0 (资产系统)
- `AI/design-cpp-script-system.md` v2.0 (脚本系统)

---

## 模块总览

### 模块依赖 DAG

```
M1:TypeRegistry ──────────┬──────────────┬──────────────┐
                          │              │              │
                          ▼              ▼              ▼
M2:Component ──────┐  M7:Asset(基础)   M4:Serialization
                    │                       │
                    ▼                       │
M3:SceneGraph ─────┤                       │
                    │                       │
                    ▼                       ▼
M5:Director ───────┤──────────────────────►│
                    │                       │
                    ▼                       ▼
M6:ScriptSystem ───┤               M4:Serialization(集成)
                    │                       │
                    ▼                       ▼
M7:Asset(完整) ◄───┤               M3:SceneGraph(完整)
                    │                       │
                    ▼                       │
M8:BuiltinComp ◄───┘                       │
                    │                       │
                    ▼                       ▼
M9:BridgeOptim ◄───┴───────────────────────┘
                    │
                    ▼
M10:Verification
```

### 模块清单

| 模块 | 代号 | 子系统 | 新增 C++ 文件 | 修改 C++ 文件 | 修改 TS 文件 |
|------|------|--------|--------------|-------------|-------------|
| TypeRegistry | M1 | 序列化 | 4 | 1 | 0 |
| Component | M2 | 组件 | 8 | 1 | 2 |
| SceneGraph | M3 | 场景 | 7 | 2 | 1 |
| Serialization | M4 | 序列化 | 2 | 1 | 1 |
| Director | M5 | 主循环 | 2 | 2 | 0 |
| ScriptSystem | M6 | 脚本 | 2 | 0 | 1 |
| Asset | M7 | 资产 | 10 | 1 | 2 |
| BuiltinComp | M8 | 组件 | 6+ | 0 | 6+ |
| BridgeOptim | M9 | 桥接 | 2 | 3 | 2 |
| Verification | M10 | 测试 | 0 | 0 | 0 |

### 阶段定义

| 阶段 | 代号 | 含义 | 产出 | 门控标准 |
|------|------|------|------|---------|
| S0 | 基础 | 数据结构 + 接口定义 | .h 文件 | 编译通过 |
| S1 | 核心 | 关键逻辑实现 | .cpp 文件 | 单元测试通过 |
| S2 | 集成 | 跨模块连接 | 修改现有文件 | 集成测试通过 |
| S3 | 优化 | 性能调优 + 边界加固 | 补丁/测试 | 性能基准达标 |

---

## M1: TypeRegistry — 统一类型注册表

> **前置依赖**: 无
> **关联裁决**: I-1 (合并组件注册表与序列化注册表)

### M1-S0: TypeRegistry 基础结构

**Files:**
- Create: `native/cocos/core/serialization/TypeRegistry.h`
- Create: `native/cocos/core/component/BuiltinTypeIds.h`
- Create: `native/cocos/core/component/ComponentMacros.h`
- Modify: `native/CMakeLists.txt`

**Step 1: 创建 BuiltinTypeIds 枚举**

```cpp
// native/cocos/core/component/BuiltinTypeIds.h
#pragma once
#include <cstdint>

namespace cc {

enum BuiltinTypeIds : uint32_t {
    BUILTIN_UNKNOWN = 0,
    BUILTIN_CAMERA = 1,
    BUILTIN_SPRITE = 2,
    BUILTIN_LABEL = 3,
    BUILTIN_MESH_RENDERER = 4,
    BUILTIN_SKINNED_MESH_RENDERER = 5,
    BUILTIN_RIGID_BODY = 6,
    BUILTIN_RIGID_BODY_3D = 7,
    BUILTIN_COLLIDER = 8,
    BUILTIN_AUDIO_SOURCE = 9,
    BUILTIN_PARTICLE_SYSTEM = 10,
    BUILTIN_ANIMATION = 11,
    BUILTIN_UI_TRANSFORM = 12,
    BUILTIN_LAYOUT = 13,
    BUILTIN_BUTTON = 14,
    BUILTIN_LIGHT = 15,
    BUILTIN_TRANSFORM = 16,
    BUILTIN_WIDGET = 17,
    BUILTIN_CANVAS = 18,
    BUILTIN_COUNT,
    // 用户脚本 TypeId 从 10000 开始
};

} // namespace cc
```

**Step 2: 创建 ComponentMacros.h**

```cpp
// native/cocos/core/component/ComponentMacros.h
#pragma once
#include "core/component/BuiltinTypeIds.h"

// 声明内置组件类
#define CC_COMPONENT_DECLARE(className, builtinId)             \
public:                                                        \
    static constexpr uint32_t COMPONENT_TYPE_ID = builtinId;   \
    uint32_t getComponentTypeId() const override { return builtinId; } \
    bool isBuiltin() const override { return true; }
```

**Step 3: 创建 TypeRegistry.h 主头文件**

```cpp
// native/cocos/core/serialization/TypeRegistry.h
#pragma once
#include <functional>
#include <cstdint>
#include "base/std/container/unordered_map.h"
#include "base/std/container/string.h"
#include "base/std/container/vector.h"

namespace cc {

class CCObject;
class Asset;

class TypeRegistry {
public:
    static TypeRegistry& getInstance();

    using Constructor = std::function<CCObject*()>;
    using BinaryDeserializer = std::function<void(CCObject*, const uint8_t*, uint32_t)>;
    using AssetPropertiesGetter = std::function<ccstd::vector<Asset*>(CCObject*)>;
    using MigrationFunc = std::function<void(uint8_t*&, uint32_t&, uint16_t fromVersion)>;

    struct TypeInfo {
        uint32_t classId{0};
        ccstd::string className;
        Constructor constructor;
        BinaryDeserializer deserializer;
        AssetPropertiesGetter assetGetter;

        // 组件专用
        bool isComponent{false};
        bool isBuiltin{false};
        bool hasUpdate{false};
        bool hasLateUpdate{false};
        int32_t executionOrder{0};
        uint32_t requireComponent{0};
        bool disallowMultiple{false};

        // 版本迁移 (G-4)
        uint16_t binaryVersion{0};
        MigrationFunc migrationFunc;
    };

    // 注册内置类型
    void registerType(const TypeInfo& info);

    // 注册用户脚本类型 (运行时)
    uint32_t registerScriptType(const ccstd::string& className,
                                 bool hasUpdate, bool hasLateUpdate,
                                 int32_t executionOrder);

    // 查询
    const TypeInfo* getTypeInfo(uint32_t classId) const;
    uint32_t getClassIdByName(const ccstd::string& className) const;
    bool hasType(uint32_t classId) const;

    // 创建实例
    CCObject* create(uint32_t classId) const;

    // 二进制反序列化
    void deserialize(uint32_t classId, CCObject* obj,
                     const uint8_t* data, uint32_t size) const;

    // 版本迁移 (G-4)
    void registerMigration(uint32_t classId, uint16_t fromVersion,
                           uint16_t toVersion, MigrationFunc func);

private:
    TypeRegistry() = default;
    ccstd::unordered_map<uint32_t, TypeInfo> _types;
    ccstd::unordered_map<ccstd::string, uint32_t> _nameToId;
    uint32_t _nextScriptClassId{10000};
};

// 内置组件注册宏
#define CC_REGISTER_BUILTIN(ClassName, StringName, BuiltinId)            \
    static bool _reg_##ClassName = []() {                                \
        cc::TypeRegistry::TypeInfo info;                                  \
        info.classId = BuiltinId;                                         \
        info.className = StringName;                                      \
        info.isComponent = true;                                          \
        info.isBuiltin = true;                                            \
        info.constructor = []() -> cc::CCObject* {                       \
            return ccnew ClassName();                                     \
        };                                                                \
        info.deserializer = [](cc::CCObject* obj, const uint8_t* d, uint32_t s) { \
            static_cast<ClassName*>(obj)->deserializeBinary(d, s);        \
        };                                                                \
        info.assetGetter = [](cc::CCObject* obj) -> ccstd::vector<cc::Asset*> { \
            return static_cast<ClassName*>(obj)->getAssetProperties();    \
        };                                                                \
        cc::TypeRegistry::getInstance().registerType(info);               \
        return true;                                                      \
    }()

} // namespace cc
```

**Step 4: 更新 CMakeLists.txt**

在 `native/CMakeLists.txt` 中找到 `cc_core` target 的源文件列表，添加：
```cmake
core/serialization/TypeRegistry.cpp
```

**门控: 编译通过**

---

### M1-S1: TypeRegistry 核心实现

**Files:**
- Create: `native/cocos/core/serialization/TypeRegistry.cpp`

```cpp
// native/cocos/core/serialization/TypeRegistry.cpp
#include "core/serialization/TypeRegistry.h"
#include "core/data/Object.h"

namespace cc {

TypeRegistry& TypeRegistry::getInstance() {
    static TypeRegistry instance;
    return instance;
}

void TypeRegistry::registerType(const TypeInfo& info) {
    _types[info.classId] = info;
    if (!info.className.empty()) {
        _nameToId[info.className] = info.classId;
    }
}

uint32_t TypeRegistry::registerScriptType(const ccstd::string& className,
                                           bool hasUpdate, bool hasLateUpdate,
                                           int32_t executionOrder) {
    uint32_t classId = _nextScriptClassId++;
    TypeInfo info;
    info.classId = classId;
    info.className = className;
    info.isComponent = true;
    info.isBuiltin = false;
    info.hasUpdate = hasUpdate;
    info.hasLateUpdate = hasLateUpdate;
    info.executionOrder = executionOrder;
    registerType(info);
    return classId;
}

const TypeRegistry::TypeInfo* TypeRegistry::getTypeInfo(uint32_t classId) const {
    auto it = _types.find(classId);
    return it != _types.end() ? &it->second : nullptr;
}

uint32_t TypeRegistry::getClassIdByName(const ccstd::string& className) const {
    auto it = _nameToId.find(className);
    return it != _nameToId.end() ? it->second : 0;
}

bool TypeRegistry::hasType(uint32_t classId) const {
    return _types.find(classId) != _types.end();
}

CCObject* TypeRegistry::create(uint32_t classId) const {
    auto* info = getTypeInfo(classId);
    if (info && info->constructor) {
        return info->constructor();
    }
    return nullptr;
}

void TypeRegistry::deserialize(uint32_t classId, CCObject* obj,
                                const uint8_t* data, uint32_t size) const {
    auto* info = getTypeInfo(classId);
    if (info && info->deserializer) {
        // 版本迁移 (G-4)
        if (info->migrationFunc && info->binaryVersion > 0) {
            auto* mutableInfo = const_cast<TypeInfo*>(info);
            uint8_t* mutData = const_cast<uint8_t*>(data);
            uint32_t mutSize = size;
            info->migrationFunc(mutData, mutSize, info->binaryVersion);
        }
        info->deserializer(obj, data, size);
    }
}

void TypeRegistry::registerMigration(uint32_t classId, uint16_t fromVersion,
                                      uint16_t toVersion, MigrationFunc func) {
    auto* info = getTypeInfo(classId);
    if (info) {
        auto* mutableInfo = const_cast<TypeInfo*>(info);
        mutableInfo->migrationFunc = func;
        mutableInfo->binaryVersion = fromVersion;
    }
}

} // namespace cc
```

**门控: TypeRegistry 注册/查询/创建 单元测试通过**

---

### M1-S1: AssetRefManager 引用计数

**Files:**
- Create: `native/cocos/core/assets/AssetRefManager.h`
- Create: `native/cocos/core/assets/AssetRefManager.cpp`
- Modify: `native/CMakeLists.txt`

```cpp
// native/cocos/core/assets/AssetRefManager.h
#pragma once
#include <functional>
#include "base/std/container/unordered_map.h"
#include "base/std/container/string.h"
#include "base/std/container/vector.h"

namespace cc {
class Asset;

class AssetRefManager {
public:
    static AssetRefManager& getInstance();

    // 统一引用计数操作（权威源）
    void addRef(Asset* asset);
    void decRef(Asset* asset, bool autoRelease = true);
    uint32_t getRefCount(Asset* asset) const;

    // 批量操作
    void addRefBatch(const ccstd::vector<Asset*>& assets);
    void decRefBatch(const ccstd::vector<Asset*>& assets, bool autoRelease = true);

    // 引用计数变化回调（通知 JS 侧）
    using RefCountChangedCallback = std::function<void(Asset*, uint32_t oldCount, uint32_t newCount)>;
    void setRefCountChangedCallback(const RefCountChangedCallback& cb);

private:
    AssetRefManager() = default;
    ccstd::unordered_map<ccstd::string, uint32_t> _refCounts;
    RefCountChangedCallback _callback;
};

} // namespace cc
```

```cpp
// native/cocos/core/assets/AssetRefManager.cpp
#include "core/assets/AssetRefManager.h"
#include "core/assets/Asset.h"

namespace cc {

AssetRefManager& AssetRefManager::getInstance() {
    static AssetRefManager instance;
    return instance;
}

void AssetRefManager::addRef(Asset* asset) {
    if (!asset) return;
    auto& count = _refCounts[asset->getUuid()];
    uint32_t oldCount = count++;
    asset->_assetRefCount = count;
    if (_callback) _callback(asset, oldCount, count);
}

void AssetRefManager::decRef(Asset* asset, bool autoRelease) {
    if (!asset) return;
    auto it = _refCounts.find(asset->getUuid());
    if (it == _refCounts.end() || it->second == 0) return;

    uint32_t oldCount = it->second--;
    asset->_assetRefCount = it->second;

    // autoRelease 逻辑暂不实现，M7 (Asset) 补全 NativeReleaseManager 后接入
    if (autoRelease && it->second == 0) {
        // TODO: NativeReleaseManager::getInstance().tryRelease(asset);
    }

    if (_callback) _callback(asset, oldCount, it->second);
}

uint32_t AssetRefManager::getRefCount(Asset* asset) const {
    if (!asset) return 0;
    auto it = _refCounts.find(asset->getUuid());
    return it != _refCounts.end() ? it->second : 0;
}

void AssetRefManager::addRefBatch(const ccstd::vector<Asset*>& assets) {
    for (auto* asset : assets) addRef(asset);
}

void AssetRefManager::decRefBatch(const ccstd::vector<Asset*>& assets, bool autoRelease) {
    for (auto* asset : assets) decRef(asset, autoRelease);
}

void AssetRefManager::setRefCountChangedCallback(const RefCountChangedCallback& cb) {
    _callback = cb;
}

} // namespace cc
```

**门控: addRef/decRef 计数平衡验证**

---

### M1-S2: JSB 桥接 — AssetRefManager 与 JS 同步

**Files:**
- Create: `native/cocos/bindings/manual/jsb_asset_ref_manual.cpp`
- Create: `native/cocos/bindings/manual/jsb_asset_ref_manual.h`
- Modify: `native/cocos/bindings/manual/jsb_module_register.cpp`

```cpp
// native/cocos/bindings/manual/jsb_asset_ref_manual.h
#pragma once
namespace se { class Object; }
bool register_asset_ref_manager(se::Object* obj);
```

```cpp
// native/cocos/bindings/manual/jsb_asset_ref_manual.cpp
#include "jsb_asset_ref_manual.h"
#include "bindings/jswrapper/SeApi.h"
#include "core/assets/AssetRefManager.h"
#include "core/assets/Asset.h"

static bool js_asset_ref_manager_addRef(se::State& s) {
    auto* asset = static_cast<cc::Asset*>(s.nativeThisObject());
    cc::AssetRefManager::getInstance().addRef(asset);
    return true;
}

static bool js_asset_ref_manager_decRef(se::State& s) {
    auto* asset = static_cast<cc::Asset*>(s.nativeThisObject());
    bool autoRelease = true;
    if (s.args().size() > 0) autoRelease = s.args()[0].toBoolean();
    cc::AssetRefManager::getInstance().decRef(asset, autoRelease);
    return true;
}

static bool js_asset_ref_manager_getRefCount(se::State& s) {
    auto* asset = static_cast<cc::Asset*>(s.nativeThisObject());
    uint32_t refCount = cc::AssetRefManager::getInstance().getRefCount(asset);
    s.rval().setUint32(refCount);
    return true;
}

bool register_asset_ref_manager(se::Object* obj) {
    // 注册为 Asset.prototype.addRef/decRef/getRefCount 的覆盖
    // 或者注册为全局对象 cc.AssetRefManager
    // 具体实现取决于 JS 侧调用约定
    return true;
}
```

在 `jsb_module_register.cpp` 中添加 `register_asset_ref_manager` 调用。

**门控: JS 侧可通过 bridge 调用 addRef/decRef**

---

### M1-S3: TypeRegistry + AssetRefManager 基准测试

**Files:**
- Create: `native/tests/benchmarks/bench_type_registry.cpp`
- Create: `native/tests/benchmarks/bench_ref_manager.cpp`
- Create: `native/tests/benchmarks/CMakeLists.txt`

```cmake
# native/tests/benchmarks/CMakeLists.txt
add_executable(bench_type_registry bench_type_registry.cpp)
target_link_libraries(bench_type_registry cc_core)

add_executable(bench_ref_manager bench_ref_manager.cpp)
target_link_libraries(bench_ref_manager cc_core)
```

```cpp
// native/tests/benchmarks/bench_type_registry.cpp
#include "core/serialization/TypeRegistry.h"
#include <chrono>
#include <cstdio>

int main() {
    auto& registry = cc::TypeRegistry::getInstance();

    // 测试 1: 注册性能
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 10000; ++i) {
        cc::TypeRegistry::TypeInfo info;
        info.classId = 20000 + i;
        info.className = "TestScript" + std::to_string(i);
        info.isComponent = true;
        info.isBuiltin = false;
        registry.registerType(info);
    }
    auto end = std::chrono::high_resolution_clock::now();
    printf("Register 10000 types: %lld us\n",
           std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());

    // 测试 2: 查询性能
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100000; ++i) {
        registry.getTypeInfo(20000 + (i % 10000));
    }
    end = std::chrono::high_resolution_clock::now();
    printf("Query 100000 types: %lld us\n",
           std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());

    return 0;
}
```

**门控: 查询性能 < 1μs/op (G-15)**

---

## M2: Component — 组件框架

> **前置依赖**: M1 (TypeRegistry + BuiltinTypeIds + ComponentMacros)
> **关联裁决**: I-2 (双层调度器), I-3 (统一 ScriptComponent)

### M2-S0: Component 基类接口

**Files:**
- Create: `native/cocos/core/component/Component.h`
- Modify: `native/CMakeLists.txt`

```cpp
// native/cocos/core/component/Component.h
#pragma once
#include "core/data/Object.h"
#include "core/component/BuiltinTypeIds.h"
#include "base/std/container/vector.h"
#include "base/std/container/string.h"

namespace cc {

class Node;
class Asset;
class Director;

class Component : public CCObject {
    CC_COMPONENT_DECLARE(Component, BUILTIN_UNKNOWN)
    friend class Node;
    friend class ComponentScheduler;
    friend class NodeActivator;

public:
    Component() = default;
    ~Component() override = default;

    // ---- 核心属性 ----
    Node* getNode() const { return _node; }
    void setNode(Node* node) { _node = node; }

    bool isEnabled() const { return _enabled; }
    bool isEnabledInHierarchy() const { return _enabledInHierarchy; }
    void setEnabled(bool enabled);

    uint32_t getComponentTypeId() const { return BUILTIN_UNKNOWN; }
    virtual bool isBuiltin() const { return true; }

    // ---- 生命周期虚函数 ----
    virtual void __preload() {}
    virtual void onLoad() {}
    virtual void start() {}
    virtual void update(float dt) {}
    virtual void lateUpdate(float dt) {}
    virtual void onEnable() {}
    virtual void onDisable() {}
    virtual void onDestroy() {}

    // ---- 生命周期检测 (替代函数指针) ----
    virtual bool hasUpdateMethod() const { return false; }
    virtual bool hasLateUpdateMethod() const { return false; }
    virtual bool hasStartMethod() const { return false; }

    // ---- 序列化接口 ----
    virtual void deserializeBinary(const uint8_t* data, uint32_t size) {}
    virtual ccstd::vector<Asset*> getAssetProperties() { return {}; }

    // ---- 定时器代理 ----
    void schedule(const std::function<void(float)>& callback, float interval,
                  unsigned int repeat = 0, float delay = 0.f, bool paused = false);
    void unschedule(const std::function<void(float)>& callback);

    // ---- 销毁 ----
    void destroy() override;
    void destruct() override;

    // ---- JS 回退机制 (G-14) ----
    void setFallbackToJS(bool fallback) { _fallbackToJS = fallback; }
    bool isFallbackToJS() const { return _fallbackToJS; }

protected:
    Node* _node{nullptr};
    bool _enabled{true};
    bool _enabledInHierarchy{false};
    bool _fallbackToJS{false};

    // 调度器注册状态
    bool _registeredToScheduler{false};
};

} // namespace cc
```

**门控: 编译通过**

---

### M2-S1: Component 核心实现

**Files:**
- Create: `native/cocos/core/component/Component.cpp`

实现:
- `Component::setEnabled()` — 联动 ComponentScheduler 注册/注销
- `Component::destroy()` — 延迟销毁流程
- `Component::destruct()` — 清理资源
- `Component::schedule/unschedule` — 代理到 `Director::getInstance()->getScheduler()`

**门控: Component 创建/销毁/启用/禁用 单元测试通过**

---

### M2-S0: ScriptComponent 统一占位类接口

> **关联裁决**: I-3 (合并生命周期代理与反序列化占位)

**Files:**
- Create: `native/cocos/core/component/ScriptComponent.h`

```cpp
// native/cocos/core/component/ScriptComponent.h
#pragma once
#include "core/component/Component.h"
#include "bindings/jswrapper/SeApi.h"

namespace cc {

class ScriptComponent final : public Component {
public:
    ScriptComponent() = default;
    ~ScriptComponent() override;

    bool isBuiltin() const override { return false; }
    uint32_t getComponentTypeId() const override { return _scriptClassId; }

    // ---- 生命周期代理 → safeCallJS ----
    void __preload() override;
    void onLoad() override;
    void start() override;
    void update(float dt) override;
    void lateUpdate(float dt) override;
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;

    // 生命周期检测 (运行时动态判断)
    bool hasUpdateMethod() const override { return _hasUpdate; }
    bool hasLateUpdateMethod() const override { return _hasLateUpdate; }
    bool hasStartMethod() const override { return _hasStart; }

    // ---- 反序列化占位 ----
    void setScriptClassPath(const ccstd::string& path) { _scriptClassPath = path; }
    const ccstd::string& getScriptClassPath() const { return _scriptClassPath; }
    void setSerializedProps(const ccstd::string& json) { _serializedProps = json; }

    // ---- JS 对象绑定 ----
    void bindJSObject(se::Object* jsObj);
    se::Object* getJSObject() const { return _jsObject; }
    bool isBound() const { return _bound; }

    // ---- GC 安全 ----
    bool isDead() const { return _dead; }

    // ---- 序列化 ----
    void deserializeBinary(const uint8_t* data, uint32_t size) override;
    ccstd::vector<Asset*> getAssetProperties() override;

private:
    template<typename Fn>
    void safeCallJS(Fn&& fn);

    uint32_t _scriptClassId{0};
    ccstd::string _scriptClassPath;
    ccstd::string _serializedProps;

    se::Object* _jsObject{nullptr};
    bool _bound{false};
    bool _dead{false};
    bool _hasUpdate{false};
    bool _hasLateUpdate{false};
    bool _hasStart{false};
};

} // namespace cc
```

**门控: 编译通过**

---

### M2-S1: ScriptComponent 核心实现

**Files:**
- Create: `native/cocos/core/component/ScriptComponent.cpp`

实现所有生命周期代理方法:
```cpp
void ScriptComponent::update(float dt) {
    safeCallJS([dt](se::Object* jsObj) {
        se::Value fn;
        if (jsObj->getProperty("update", &fn) && fn.isFunction()) {
            se::Value args[1] = { se::Value(dt) };
            se::Value result;
            fn.toObject()->call(args, 1, &result);
        }
    });
}
// start, onLoad, onEnable, onDisable, onDestroy 类似
```

实现 `safeCallJS` — 检查 `isDead()` 再调用，GC 安全 (G-8)

**门控: ScriptComponent 生命周期代理正确触发 JS 回调**

---

### M2-S0: ThreeBucketArray 模板接口

**Files:**
- Create: `native/cocos/core/component/ThreeBucketArray.h`

参考 `AI/design-cpp-component-framework.md` §4.2。核心接口：
- `add(T* item, int32_t executionOrder)` — 按 executionOrder 分桶（neg/zero/pos）
- `remove(T* item)` — 三桶查找并移除
- `forEach(Callable&& fn)` — 按 neg→zero→pos 顺序遍历
- `clear()` — 清空所有桶

纯头文件模板，无需 .cpp。

**门控: 编译通过**

---

### M2-S1: ComponentScheduler 双层三桶

> **关联裁决**: I-2 (双层调度器)

**Files:**
- Create: `native/cocos/core/component/ComponentScheduler.h`
- Create: `native/cocos/core/component/ComponentScheduler.cpp`

核心结构：
- `BuiltinSchedule` — 3 个 `ThreeBucketArray<Component>` (start/update/lateUpdate)
- `ScriptSchedule` — 3 个 `ccstd::vector<uint32_t>` (start/update/lateUpdate)
- 帧循环入口: `startPhase()`, `updatePhase(dt)`, `lateUpdatePhase(dt)`
- 注册/注销: `scheduleBuiltin`, `unscheduleBuiltin`, `scheduleScript`, `unscheduleScript`
- 启用/禁用: `enableComp`, `disableComp`

帧调度逻辑:
- `startPhase()` — builtin.start.forEach() + ScriptBridge::invokeStartBatch()
- `updatePhase(dt)` — builtin.update.forEach() + ScriptBridge::invokeUpdateBatch()
- `lateUpdatePhase(dt)` — builtin.late.forEach() + ScriptBridge::invokeLateUpdateBatch()
- `processDeferred()` — 帧中缓冲处理

**门控: ThreeBucketArray 排序正确，双层调度时序 start→update→lateUpdate 一致**

---

### M2-S2: Component JSB 绑定

**Files:**
- Create: `native/cocos/bindings/manual/jsb_component_manual.cpp`
- Create: `native/cocos/bindings/manual/jsb_component_manual.h`
- Modify: `cocos/scene-graph/component.jsb.ts` (脚本注册入口)

**门控: TS 侧可通过 JSB 创建/查询 C++ Component**

---

## M3: SceneGraph — 场景图

> **前置依赖**: M2 (Component 基类 + ComponentScheduler)
> **关联裁决**: I-4 (常驻节点归 Director)

### M3-S1: Node 组件管理补全

**Files:**
- Modify: `native/cocos/core/scene-graph/Node.h` (取消注释 + 完善)
- Modify: `native/cocos/core/scene-graph/Node.cpp`

在 Node.h 中取消注释并完善组件管理代码（位置: 第 508-594 行）:
- `addComponent<T>()` — 模板版
- `addComponent(className)` — 字符串版 → TypeRegistry::create()
- `getComponent<T>()` / `getComponent(className)`
- `getComponents<T>()`
- `removeComponent(Component*)`
- `_components: ccstd::vector<IntrusivePtr<Component>>`

核心流程:
```
addComponent → disallowMultiple 检查 → requireComponent 递归
  → comp->setNode(this) → _components.push_back → emit ComponentAdded
  → [如果已激活] NodeActivator::activateComp(comp)
```

**门控: addComponent/getComponent/removeComponent 功能正确**

---

### M3-S1: NodeActivator 实现

**Files:**
- Create: `native/cocos/core/scene-graph/NodeActivator.h`
- Create: `native/cocos/core/scene-graph/NodeActivator.cpp`
- Modify: `native/cocos/core/scene-graph/Node.cpp` (替换 JS 调用)

核心功能：
- `activateNode(node, active)` — 递归激活/停用
- `activateComp(comp)` — 单组件激活流程
- 批量调用缓存: _preloadComps, _onLoadComps, _onEnableComps

替换 Node.cpp 中的:
```cpp
// 当前: Director::getInstance()->getNodeActivator()->activateNode(this, shouldActiveNow); // TODO(xwx): use TS temporarily
// 替换为: Director::getInstance()->getNodeActivator()->activateNode(this, shouldActiveNow);
```

**门控: 节点激活/停用时组件生命周期正确触发**

---

### M3-S1: Scene::load/activate 完善

**Files:**
- Modify: `native/cocos/core/scene-graph/Scene.h`
- Modify: `native/cocos/core/scene-graph/Scene.cpp`

完善 Scene::load() — 取消 `expandNestedPrefabInstanceNode` 和 `applyTargetOverrides` 注释，接入 M3-S2 的 PrefabUtils 实现。

完善 Scene::activate() — 接入 NodeActivator。

添加 autoReleaseAssets 属性 (I-4 不增加常驻节点)。

**门控: Scene 加载后节点树完整，组件激活链正确**

---

### M3-S2: Prefab C++ 类 + 二进制模板实例化

**Files:**
- Create: `native/cocos/core/scene-graph/Prefab.h`
- Create: `native/cocos/core/scene-graph/Prefab.cpp`
- Create: `native/cocos/core/scene-graph/PrefabInfo.h`
- Create: `native/cocos/core/scene-graph/PrefabUtils.h`
- Create: `native/cocos/core/scene-graph/PrefabUtils.cpp`
- Modify: `native/CMakeLists.txt`

核心接口：
- `initFromBinary()` — 从二进制数据初始化 BinaryTemplate
- `instantiate()` — 选择实例化策略
- `instantiateFromBinary()` — C++ 快速实例化
- OptimizationPolicy: AUTO/SINGLE/MI

实现 `expandNestedPrefabInstanceNode` 和 `applyTargetOverrides` (当前 Scene.cpp 中的 TODO)。

**门控: Prefab 实例化产生正确的节点树，嵌套 Prefab 正确展开**

---

## M4: Serialization — 序列化

> **前置依赖**: M1 (TypeRegistry), M2 (Component), M3 (SceneGraph 基础)
> **关联裁决**: G-4 (版本迁移)

### M4-S0: 二进制场景格式规范

定义二进制格式规范（纯设计，无代码）:
```cpp
struct Header {
    char magic[4];          // "CCSC"
    uint16_t version;       // 格式版本
    uint16_t flags;
    uint32_t stringTableOffset;
    uint32_t instanceTableOffset;
    uint32_t rootNodeOffset;
    uint8_t reserved[12];
};
```

参考 `AI/design-cpp-scene-system.md` §4.2。

---

### M4-S1: BinaryDeserializer 实现

**Files:**
- Create: `native/cocos/core/serialization/BinaryDeserializer.h`
- Create: `native/cocos/core/serialization/BinaryDeserializer.cpp`
- Modify: `native/CMakeLists.txt`

核心接口:
```cpp
struct Result {
    IntrusivePtr<Scene> scene;
    ccstd::vector<IntrusivePtr<Asset>> assets;
};

class BinaryDeserializer {
public:
    static Result deserialize(const uint8_t* data, uint32_t size);
private:
    static Scene* createScene(const uint8_t* data, const Header& header);
    static Node* createNodeTree(const uint8_t* data, uint32_t offset,
                                const Header& header,
                                ccstd::vector<CCObject*>& instances);
    static void resolveReferences(const ccstd::vector<CCObject*>& instances,
                                  const AssetRefTable& refs);
};
```

核心流程：
1. 验证 Header (magic + version)
2. 解析 String Table
3. 解析 Instance Table → TypeRegistry::create()
4. 递归创建 Node 树
5. 解析 Component Data → TypeRegistry::deserialize()
6. 解析 Asset Reference Table
7. resolveReferences() — 解析 UUID 引用

**门控: .scene.bin 可被 C++ 反序列化为完整场景**

---

### M4-S2: 构建时转换脚本

**Files:**
- Create: `cocos/core/serialization/build-scene-binary.ts` (TS 构建脚本)

参考 `AI/design-cpp-scene-system.md` §4.6。核心功能：
1. 解析 JSON 场景文件
2. 为每个类分配 classId（基于 @ccclass 名称哈希）
3. 收集所有字符串 → 字符串表
4. 将节点树递归编码为二进制
5. 内置组件属性直接二进制化
6. 用户脚本组件保留为 JSON 子段
7. 输出 .scene.bin

集成到 `@cocos/ccbuild` 配置，在 native 构建时自动调用此脚本。

**门控: JSON .scene 文件可转换为 .scene.bin 且反序列化后等价**

---

### M4-S2: BinaryDeserializer JSB 绑定

**Files:**
- Create: `native/cocos/bindings/manual/jsb_binary_deser_manual.cpp`
- Create: `native/cocos/bindings/manual/jsb_binary_deser_manual.h`

**门控: TS 侧可调用 BinaryDeserializer.deserialize()**

---

### M4-S3: 版本迁移机制

> **关联裁决**: G-4 (二进制格式前向兼容)

**Files:**
- Modify: `native/cocos/core/serialization/TypeRegistry.h` (已在 M1-S0 包含 MigrationFunc)
- Modify: `native/cocos/core/serialization/TypeRegistry.cpp` (已在 M1-S1 包含 registerMigration)

TypeRegistry 已包含迁移函数注册。此阶段补全 BinaryDeserializer 中的版本检测和迁移调用逻辑。

**门控: 旧版 .scene.bin 可自动迁移到新格式**

---

## M5: Director — 主循环

> **前置依赖**: M2 (ComponentScheduler), M3 (NodeActivator)
> **关联裁决**: G-1 (Director tick 接管), I-4 (常驻节点归 Director)

### M5-S1: Director C++ 实现

**Files:**
- Create: `native/cocos/core/Director.h`
- Create: `native/cocos/core/Director.cpp`
- Modify: `native/CMakeLists.txt`

核心接口：
- `tick(dt)` — 主循环
- `loadScene/runScene/runSceneImmediate` — 场景管理
- `addPersistRootNode/removePersistRootNode/isPersistRootNode` — 常驻节点 (I-4)
- 子系统访问: `getCompScheduler/getNodeActivator/getScheduler`

```cpp
void Director::tick(float dt) {
    // 1. 处理输入事件
    // 2. Scheduler::update(dt)
    _scheduler->update(dt);
    // 3. ComponentScheduler::startPhase()
    _compScheduler->startPhase();
    // 4. ComponentScheduler::updatePhase(dt)
    _compScheduler->updatePhase(dt);
    // 5. ComponentScheduler::lateUpdatePhase(dt)
    _compScheduler->lateUpdatePhase(dt);
    // 6. 物理步进 (if enabled)
    // 7. 动画更新
    // 8. 渲染场景更新
    // 9. 延迟销毁处理
}
```

实现 `runSceneImmediate()` — 参考 `design-cpp-scene-system.md` §6.2

**门控: Director.tick() 可驱动 ComponentScheduler 和 Scheduler**

---

### M5-S2: 主游戏循环打通

**Files:**
- Modify: `native/cocos/engine/Engine.cpp`
- Modify: `native/cocos/core/Root.cpp`

在 `Engine::tick()` 中接入 Director:
```cpp
// 当前: 通过 JS 调用 director.tick()
// 修改为: Director::getInstance()->tick(dt)
```

**门控: 运行简单场景 (10 Node + 2 ScriptComp) 生命周期正确触发**

---

## M6: ScriptSystem — 脚本系统

> **前置依赖**: M1 (TypeRegistry), M2 (Component + ScriptComponent)
> **关联裁决**: G-6 (异常隔离), G-8 (GC 安全)

### M6-S1: ScriptBridge 核心实现

**Files:**
- Create: `native/cocos/core/scripting/ScriptBridge.h`
- Create: `native/cocos/core/scripting/ScriptBridge.cpp`
- Modify: `native/CMakeLists.txt`

参考 `AI/design-cpp-script-system.md` §3.1。核心接口：
- 批量生命周期调用: `invokeStartBatch/invokeUpdateBatch/invokeLateUpdateBatch`
- 单个调用: `invokeOnDestroy/invokeOnEnable/invokeOnDisable/invokeOnLoad`
- 脚本类型注册: `registerScriptClass` → 调用 TypeRegistry
- 脚本实例注册: `registerScriptInstance/unregisterScriptInstance`
- Asset 引用收集: `collectAssetRefs`
- instanceof 检查: `isInstanceOf`

批量调用优化 (参见 design-cpp-script-system.md §3.3):
- 构建 JS 参数数组
- 单次 JSB 调用处理所有组件
- 每个组件 try-catch 隔离 (G-6)

**门控: ScriptBridge 批量调用比逐个调用减少 90% JSB 次数**

---

### M6-S2: ScriptBridge JSB 绑定 + TS 批量执行器

**Files:**
- Create: `native/cocos/bindings/manual/jsb_script_bridge_manual.cpp`
- Create: `native/cocos/bindings/manual/jsb_script_bridge_manual.h`
- Modify: `cocos/core/scripting/batch-executor.ts`

**门控: TS 侧可通过 ScriptBridge 注册脚本类型和实例**

---

## M7: Asset — 资产系统

> **前置依赖**: M1 (AssetRefManager), M4 (BinaryDeserializer), M5 (Director)
> **注意**: 分两个阶段 — 基础设施 (可与 M2 并行) 和 完整管线 (需 M3+M5)

### M7-S0: NativePipeline 接口

**Files:**
- Create: `native/cocos/core/assets/NativePipeline.h`

参考 `AI/design-cpp-asset-system.md` §4.2。同步/异步管线执行接口定义。

**门控: 编译通过**

---

### M7-S1: NativePipeline 实现

**Files:**
- Create: `native/cocos/core/assets/NativePipeline.cpp`

同步/异步管线执行实现。双模式路由：
- JS_ONLY → callJSLoad()
- NATIVE_FAST → loadNativeAsset()

**门控: Pipeline 可执行同步/异步加载任务**

---

### M7-S1: NativeBundle

**Files:**
- Create: `native/cocos/core/assets/NativeBundle.h`
- Create: `native/cocos/core/assets/NativeBundle.cpp`

参考 `AI/design-cpp-asset-system.md` §5。Bundle 配置解析（rapidjson）、资源路径映射、场景信息。

**门控: 可从 Bundle 配置中获取资源路径列表**

---

### M7-S1: NativeDependUtil

**Files:**
- Create: `native/cocos/core/assets/NativeDependUtil.h`
- Create: `native/cocos/core/assets/NativeDependUtil.cpp`

参考 `AI/design-cpp-asset-system.md` §7。依赖追踪和反向索引。

**门控: 资源加载后依赖关系可查询**

---

### M7-S2: NativeAssetManager

**Files:**
- Create: `native/cocos/core/assets/NativeAssetManager.h`
- Create: `native/cocos/core/assets/NativeAssetManager.cpp`

参考 `AI/design-cpp-asset-system.md` §4。双模式加载路由：
- JS_ONLY → callJSLoad()
- NATIVE_FAST → loadNativeAsset()

关键: Texture2D/Mesh/SceneAsset 的 C++ 完整加载路径。

**门控: Texture2D/Mesh 可通过 NativeAssetManager 全 C++ 路径加载**

---

### M7-S2: NativeReleaseManager

**Files:**
- Create: `native/cocos/core/assets/NativeReleaseManager.h`
- Create: `native/cocos/core/assets/NativeReleaseManager.cpp`

参考 `AI/design-cpp-asset-system.md` §6。场景切换时的批量释放、引用遍历。

**门控: 场景切换后未被引用的资产被正确释放**

---

### M7-S2: AssetRefManager 与 NativeReleaseManager 集成

**Files:**
- Modify: `native/cocos/core/assets/AssetRefManager.cpp` (解除 TODO 注释)

将 M1 保留的 `decRef` 中的 `NativeReleaseManager::tryRelease()` 调用接入。

---

### M7-S2: NativeAssetManager JSB 绑定

**Files:**
- Create: `native/cocos/bindings/manual/jsb_native_asset_mgr_manual.cpp`
- Create: `native/cocos/bindings/manual/jsb_native_asset_mgr_manual.h`

---

## M8: BuiltinComp — 内置组件 C++ 化

> **前置依赖**: M2 (Component 基类), M3 (SceneGraph), M5 (Director), M6 (ScriptBridge)
> **关联裁决**: G-9/G-10/G-11 (渲染/物理/2D 集成), G-14 (JS fallback)

### 组件 C++ 化通用模板

每个内置组件遵循相同流程：
1. 创建 `.h/.cpp`，使用 `CC_COMPONENT_DECLARE` 和 `CC_REGISTER_BUILTIN`
2. 实现 `onEnable/onDisable/onDestroy` — 与 RenderScene/Render2D 集成
3. 实现 `hasUpdateMethod` 和 `update()` — 如需每帧逻辑
4. 实现 `deserializeBinary()` — 固定布局二进制读取
5. 实现 `getAssetProperties()` — 返回引用的 Asset 列表
6. 实现 JS fallback — `setFallbackToJS(true)` 机制 (G-14)
7. 编写属性覆盖测试 — 确保 TS @property 无遗漏

---

### M8-S1: Camera (P0)

**Files:**
- Create: `native/cocos/core/components/Camera.h`
- Create: `native/cocos/core/components/Camera.cpp`

渲染管线集成 (G-9):
- `onEnable()` → RenderScene::createCamera() + activate(true)
- `onDisable()` → activate(false)
- `update()` → 同步变换到 scene::Camera

**门控: Camera onEnable→渲染可见，属性与 TS 版本一致**

---

### M8-S1: MeshRenderer (P0)

**Files:**
- Create: `native/cocos/core/components/MeshRenderer.h`
- Create: `native/cocos/core/components/MeshRenderer.cpp`

渲染管线集成:
- `onEnable()` → 注册到 RenderScene 的模型列表
- `getAssetProperties()` → 返回 _mesh + _materials[]

**门控: MeshRenderer 可渲染 Mesh，材质切换正确**

---

### M8-S1: Light (P0)

**Files:**
- Create: `native/cocos/core/components/DirectionalLight.h`
- Create: `native/cocos/core/components/PointLight.h`
- Create: `native/cocos/core/components/SpotLight.h`

复用已有 `native/cocos/scene/` 中的 Light C++ 实现。

**门控: 光照效果与 TS 版本一致**

---

### M8-S2: SkeletonAnimation (P1)

**Files:**
- Create: `native/cocos/core/components/SkeletonAnimation.h`
- Create: `native/cocos/core/components/SkeletonAnimation.cpp`

复用已有 `native/cocos/3d/skeletal-animation/` 和 `native/cocos/core/animation/` 的 C++ 工具。

**门控: 骨骼动画播放与 TS 版本一致**

---

### M8-S2: RigidBody (P1)

**Files:**
- Create: `native/cocos/core/components/RigidBody.h`
- Create: `native/cocos/core/components/RigidBody.cpp`

物理系统集成 (G-10):
- `onEnable()` → PhysicsWorld::addBody()
- `update()` → 同步变换到物理引擎
- `onDisable()` → PhysicsWorld::removeBody()

**门控: 物理模拟行为与 TS 版本一致**

---

### M8-S2: Sprite (P1)

**Files:**
- Create: `native/cocos/core/components/Sprite.h`
- Create: `native/cocos/core/components/Sprite.cpp`

2D 渲染集成 (G-11): 依赖 Render2D 系统。如果 Render2D 尚未 C++ 化，则 Sprite 仅做属性 C++ 化，渲染仍走 JS。

**门控: Sprite 显示与 TS 版本一致，spriteFrame 切换正确**

---

## M9: BridgeOptim — 桥接优化

> **前置依赖**: M5 (Director), M6 (ScriptBridge), M8 (BuiltinComp)

### M9-S1: _tempFloatArray 消除

**Files:**
- Modify: `cocos/core/scene-graph/node.jsb.ts` (TS 侧)
- Modify: `native/cocos/core/scene-graph/Node.h` (C++ 侧)

参考 `AI/design-cpp-script-system.md` §6。将 JS 的 `_tempFloatArray` 中转替换为 JSB 直接属性绑定。

**门控: 节点变换更新不再经过 _tempFloatArray**

---

### M9-S2: EngineProfiler 调试钩子

> **关联裁决**: G-12 (EngineProfiler 钩子)

**Files:**
- Create: `native/cocos/core/debug/Profiler.h`
- Create: `native/cocos/core/debug/Profiler.cpp`

FrameStats 结构:
- tickTime, schedulerTime, builtinUpdateTime, scriptUpdateTime, renderTime, gcTime
- builtinCompCount, scriptCompCount, jsbCallCount

集成到 ComponentScheduler 和 ScriptBridge 的 beginSection/endSection。

**门控: 每帧可获取各阶段耗时统计**

---

### M9-S2: 回滚机制验证

> **关联裁决**: G-14 (回滚策略)

**Files:**
- Modify: `native/cocos/core/component/Component.h` (添加 setFallbackToJS — 已在 M2-S0 包含)
- 修改各内置组件的 onEnable/onDisable — 失败时回退到 JS

验证矩阵:

| 场景 | 回滚方案 | 测试 |
|------|---------|------|
| Camera C++ onEnable 崩溃 | fallbackToJS → JS Camera | ✓ |
| BinaryDeserializer 解析失败 | 降级到 JSON + JS 反序列化 | ✓ |
| NativeAssetManager 加载失败 | callJSLoad 兜底 | ✓ |
| 整体回退 | NativeAssetManager::setMode(JS_ONLY) | ✓ |

**门控: 每种回滚场景测试通过**

---

## M10: Verification — 验证

> **前置依赖**: 所有模块完成

### M10-S1: 全面回归测试

测试清单:
1. 空场景加载 (JS_ONLY vs NATIVE_FAST)
2. 1000 节点场景加载 (二进制 vs JSON)
3. Prefab 实例化 (100 节点 × 10 次)
4. 资产加载/释放循环 (Texture2D + Mesh)
5. 组件 add/remove/get 循环
6. 常驻节点场景切换
7. 用户脚本 start/update/lateUpdate 正确触发
8. GC 安全 — 销毁后不再调用 JS

---

### M10-S2: 性能基准 A/B 对比

**Files:**
- Create: `native/tests/benchmarks/bench-results/js-baseline.json`
- Create: `native/tests/benchmarks/bench-results/native-fast.json`

| 基准项 | JS_ONLY 目标 | NATIVE_FAST 目标 | 验收 |
|--------|-------------|-----------------|------|
| 1000 节点场景加载 | 58ms | ≤15ms (4×) | |
| Prefab 实例化 (100节点) | 12ms | ≤4ms (3×) | |
| 每帧 JSB 桥接次数 | 250 | ≤5 (98%↓) | |
| 引用计数同步延迟 | — | ≤1帧 | |
| 内存峰值 (场景加载) | — | ≤80% JS_ONLY | |

---

### M10-S3: 内存泄漏/压力测试

1. 场景加载/卸载 100 次循环 — 无内存增长
2. Prefab 实例化/销毁 1000 次 — 无泄漏
3. AssetRefManager addRef/decRef 平衡验证
4. ScriptComponent _jsObject 弱引用释放验证

---

## 风险缓解策略速查

| 风险 | 缓解 | 涉及模块 |
|------|------|---------|
| 二进制格式与 JSON 不兼容 | 编辑器始终 JS_ONLY | M4 |
| TypeRegistry 与 TS CCClass 不一致 | classId 基于 @ccclass 名称哈希 | M1 |
| 内置组件遗漏属性导致 JS 侧断链 | 自动化属性覆盖测试 | M8 |
| ScriptBridge 批量调用失败 | 分批 + 异常隔离 + 单个 try-catch | M6 |
| 双层调度时序不一致 | 严格保持 start→update→lateUpdate | M2 |
| 渲染管线集成断裂 | 复用已有 RenderScene C++ API | M8 |
| Web 平台不支持 NATIVE_FAST | 构建时跳过 .scene.bin | M4 |
| C++ 内置组件生产环境 bug | 每个组件有 JS fallback | M8 |
| 大规模重构导致回归 | 渐进式替换 + 每模块回归测试 | All |

---

## 关键文件清单

### 新增 C++ 文件 (按模块)

| 模块 | 文件路径 | 说明 |
|------|---------|------|
| M1 | `core/serialization/TypeRegistry.h/.cpp` | 统一类型注册表 (I-1) |
| M1 | `core/component/BuiltinTypeIds.h` | 内置组件 TypeId 枚举 |
| M1 | `core/component/ComponentMacros.h` | CC_COMPONENT_DECLARE 宏 |
| M1 | `core/assets/AssetRefManager.h/.cpp` | 引用计数统一 |
| M2 | `core/component/Component.h/.cpp` | 组件基类 |
| M2 | `core/component/ScriptComponent.h/.cpp` | 统一脚本组件 (I-3) |
| M2 | `core/component/ThreeBucketArray.h` | 三桶排序模板 |
| M2 | `core/component/ComponentScheduler.h/.cpp` | 双层调度器 (I-2) |
| M3 | `core/scene-graph/NodeActivator.h/.cpp` | 节点激活器 |
| M3 | `core/scene-graph/Prefab.h/.cpp` | Prefab C++ 类 |
| M3 | `core/scene-graph/PrefabInfo.h` | Prefab 信息结构 |
| M3 | `core/scene-graph/PrefabUtils.h/.cpp` | Prefab 工具函数 |
| M4 | `core/serialization/BinaryDeserializer.h/.cpp` | 二进制反序列化 |
| M5 | `core/Director.h/.cpp` | 主循环 + 常驻节点 (G-1, I-4) |
| M6 | `core/scripting/ScriptBridge.h/.cpp` | JS 批量调用桥 |
| M7 | `core/assets/NativePipeline.h/.cpp` | 加载管线 |
| M7 | `core/assets/NativeBundle.h/.cpp` | Bundle 管理 |
| M7 | `core/assets/NativeDependUtil.h/.cpp` | 依赖追踪 |
| M7 | `core/assets/NativeAssetManager.h/.cpp` | 资产管理器 |
| M7 | `core/assets/NativeReleaseManager.h/.cpp` | 释放管理器 |
| M8 | `core/components/Camera.h/.cpp` | Camera 组件 |
| M8 | `core/components/MeshRenderer.h/.cpp` | MeshRenderer 组件 |
| M8 | `core/components/*Light.h/.cpp` | Light 组件 |
| M8 | `core/components/SkeletonAnimation.h/.cpp` | 骨骼动画 |
| M8 | `core/components/RigidBody.h/.cpp` | 刚体组件 |
| M8 | `core/components/Sprite.h/.cpp` | Sprite 组件 |
| M9 | `core/debug/Profiler.h/.cpp` | EngineProfiler (G-12) |

### 修改的现有 C++ 文件

| 模块 | 文件路径 | 修改内容 |
|------|---------|---------|
| M3 | `core/scene-graph/Node.h` | 取消注释组件管理代码 |
| M3 | `core/scene-graph/Node.cpp` | 实现组件管理 + NodeActivator 接入 |
| M3 | `core/scene-graph/Scene.h` | 添加 autoReleaseAssets |
| M3 | `core/scene-graph/Scene.cpp` | 完善 load/activate，接入 PrefabUtils |
| M5 | `engine/Engine.cpp` | 接入 Director 主循环 |
| M7 | `core/assets/AssetRefManager.cpp` | 解除 NativeReleaseManager TODO |
| M9 | `core/scene-graph/Node.h` | _tempFloatArray 消除 |

### 新增 JSB 绑定文件

| 模块 | 文件路径 | 说明 |
|------|---------|------|
| M1 | `bindings/manual/jsb_asset_ref_manual.cpp/.h` | AssetRefManager 绑定 |
| M2 | `bindings/manual/jsb_component_manual.cpp/.h` | Component 绑定 |
| M4 | `bindings/manual/jsb_binary_deser_manual.cpp/.h` | BinaryDeserializer 绑定 |
| M6 | `bindings/manual/jsb_script_bridge_manual.cpp/.h` | ScriptBridge 绑定 |
| M7 | `bindings/manual/jsb_native_asset_mgr_manual.cpp/.h` | NativeAssetManager 绑定 |

### 新增/修改的 TS 文件

| 模块 | 文件路径 | 说明 |
|------|---------|------|
| M2 | `cocos/scene-graph/component.jsb.ts` | 脚本注册入口 |
| M4 | `cocos/core/serialization/build-scene-binary.ts` | 构建脚本 |
| M6 | `cocos/core/scripting/batch-executor.ts` | 批量执行器 |
| M9 | `cocos/core/scene-graph/node.jsb.ts` | _tempFloatArray 消除 |

---

## 执行指南

### 模块执行顺序

```
M1 (无依赖) ──────────────────────────────────────────────────────┐
                                                                    │
M2 (依赖 M1) ───────────────────────────────────────────┐          │
                                                          │          │
M3 (依赖 M2) ──────┐                                     │          │
                     │                                     │          │
M4 (依赖 M1,M2) ───┤                                     │          │
                     │                                     │          │
M5 (依赖 M2,M3) ───┤                                     │          │
                     │                                     │          │
M6 (依赖 M1,M2) ───┤                                     │          │
                     │                                     │          │
M7 (依赖 M1,M4,M5) ┤                                     │          │
                     │                                     │          │
M8 (依赖 M2,M3,M5,M6)                                      │          │
                     │                                     │          │
M9 (依赖 M5,M6,M8) ┤                                     │          │
                     │                                     │          │
M10 (依赖全部) ◄────┴─────────────────────────────────────┴──────────┘
```

**可并行模块组:**
- **第 1 层**: M1 (无依赖，必须先完成)
- **第 2 层**: M2 (依赖 M1)
- **第 3 层**: M3 + M4 + M6 (均依赖 M1+M2，三者互不依赖，可并行)
- **第 4 层**: M5 (依赖 M2+M3) + M7 (依赖 M1+M4)
- **第 5 层**: M8 (依赖 M2+M3+M5+M6)
- **第 6 层**: M9 (依赖 M5+M6+M8)
- **第 7 层**: M10 (依赖全部)

### 开发环境准备

```bash
# 1. 确认构建工具
cmake --version  # >= 3.20
ninja --version  # 推荐
python --version # >= 3.8 (基准测试脚本)

# 2. 生成构建文件 (Windows)
cd native
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# 3. 首次编译验证
cmake --build build --config Debug
```

### 每个 Task 的标准流程

1. **创建文件** — 按计划中的代码创建新文件
2. **编译验证** — `cmake --build build --target cc_core`
3. **单元测试** — 验证新增逻辑
4. **提交** — 一组相关文件一个 commit，消息格式: `feat(cpp): <描述> (M<模块号>, <裁决/补全编号>)`
5. **更新进度** — 在本文档的任务后标注 ✓/✗

### 模块阶段门控

| 模块 | S0 门控 | S1 门控 | S2 门控 | S3 门控 |
|------|---------|---------|---------|---------|
| M1 | 编译通过 | 注册/查询/创建测试通过 | JS bridge 可调用 | 查询 <1μs/op |
| M2 | 编译通过 | Component 生命周期正确 | TS 可创建/查询 C++ Component | 调度时序一致 |
| M3 | — | addComponent/getComponent 正确 | Prefab 实例化正确 | — |
| M4 | 格式定义完成 | .scene.bin 反序列化成功 | JSON↔Binary 转换等价 | 版本迁移正确 |
| M5 | — | Director.tick 驱动调度 | 主循环贯通 | — |
| M6 | — | 批量调用减少 90% JSB | TS 可注册脚本 | — |
| M7 | 接口编译通过 | Pipeline+Bundle 可用 | Texture/Mesh 全 C++ 加载 | 释放无泄漏 |
| M8 | — | P0 组件渲染可见 | P1 组件功能正确 | JS fallback 验证 |
| M9 | — | _tempFloatArray 消除 | 回滚验证通过 | 性能基准达标 |
| M10 | — | 回归测试通过 | A/B 对比达标 | 压力测试通过 |

---

_本计划基于 v2.0 统一修订版设计文档生成。如有设计变更，应同步更新本计划。_
