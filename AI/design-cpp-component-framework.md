# C++ Component 框架设计方案

> 版本: v2.0 (统一修订版)
> 日期: 2026-04-25
> 基于: Cocos Creator v3.8.8 现有 C++ 架构
> 输入文档: AI/analysis-component-framework.md, AI/analysis-script-system.md
> 总体规范: AI/design-cpp-master-spec.md v2.0
> 修订项: I-1 (TypeRegistry合并), I-2 (双层三桶调度), I-3 (ScriptComponent统一)

---

## 0. 现有 C++ 基础设施分析

### 0.1 已有基类

| C++ 类 | 头文件 | 说明 |
|--------|--------|------|
| `cc::RefCounted` | `native/cocos/base/RefCounted.h` | 引用计数基类，`addRef()`/`release()` |
| `cc::CCObject` | `native/cocos/core/data/Object.h` | 继承 RefCounted，含 `_objFlags` 标志位、`destroy()` 延迟销毁、`_scriptObject` JS 绑定 |
| `cc::Node` | `native/cocos/core/scene-graph/Node.h` | 继承 CCObject，完整变换/层级/事件，**组件代码已注释** |
| `cc::Scheduler` | `native/cocos/base/Scheduler.h` | 定时器调度，`schedule()`/`unschedule()` |

### 0.2 CCObject::Flags (C++ 侧已定义)

```cpp
enum class Flags : FlagBits {
    DESTROYED = 1 << 0,
    REAL_DESTROYED = 1 << 1,
    TO_DESTROY = 1 << 2,
    DONT_SAVE = 1 << 3,
    EDITOR_ONLY = 1 << 4,
    DIRTY = 1 << 5,
    DONT_DESTROY = 1 << 6,
    DESTROYING = 1 << 7,
    DEACTIVATING = 1 << 8,
    // ... 与 TS 版本一一对应
    IS_ON_ENABLE_CALLED = 1 << 11,
    IS_PRELOAD_STARTED = 1 << 13,
    IS_ON_LOAD_CALLED = 1 << 14,
    IS_ON_LOAD_STARTED = 1 << 15,
    IS_START_CALLED = 1 << 16,
};
```

### 0.3 Node 中注释掉的组件代码

```cpp
// native/cocos/core/scene-graph/Node.h 第 508-594 行
// 全部被注释，包含:
// - addComponent<T>()
// - removeComponent<T>()
// - getComponent<T>()
// - getComponents<T>()
// - _components: ccstd::vector<Component*>
```

**结论**: C++ 侧已预留了组件系统的接口位置，但实现为空。本方案的核心任务就是**补全这部分实现**。

---

## 1. 类注册与反射系统 (I-1 修订)

> **I-1 裁决**: 合并本文档 v1.0 的 `ComponentTypeRegistry` 与 Scene 系统的 `TypeRegistry` 为**统一 `TypeRegistry`**。
> 完整定义见 `AI/design-cpp-master-spec.md` §2.1。
> 本节仅描述组件框架对 TypeRegistry 的使用方式。

### 1.1 TypeRegistry 概要 (引用)

```cpp
// 完整定义: native/cocos/core/serialization/TypeRegistry.h
// 参见 design-cpp-master-spec.md §2.1

class TypeRegistry {
public:
    static TypeRegistry& getInstance();

    // === 统一 TypeInfo ===
    struct TypeInfo {
        uint32_t classId;              // 唯一类型 ID
        ccstd::string className;       // "cc.Sprite", "cc.Camera"
        Constructor constructor;
        BinaryDeserializer deserializer;
        AssetPropertiesGetter assetGetter;

        // 组件专用字段
        bool isComponent{false};
        bool isBuiltin{false};         // true=C++ 内置, false=JS 脚本
        bool hasUpdate{false};
        bool hasLateUpdate{false};
        int32_t executionOrder{0};
        uint32_t requireComponent{0};
        bool disallowMultiple{false};
    };

    void registerType(const TypeInfo& info);
    uint32_t registerScriptType(const ccstd::string& className,
                                 bool hasUpdate, bool hasLateUpdate,
                                 int32_t executionOrder);
    const TypeInfo* getTypeInfo(uint32_t classId) const;
    uint32_t getClassIdByName(const ccstd::string& className) const;
    CCObject* create(uint32_t classId) const;
};

// 内置组件注册宏 (统一版)
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
```

### 1.2 内置组件 TypeId 枚举

```cpp
// core/component/BuiltinTypeIds.h
namespace cc {

// 内置组件的 classId 通过枚举分配 (编译时确定)
// 这些 ID 同时用于 TypeRegistry 和二进制序列化
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
    // ... 按需扩展
    BUILTIN_COUNT,
    // 用户脚本 TypeId 从 10000 开始 (见 TypeRegistry::_nextScriptClassId)
};

} // namespace cc
```

### 1.3 内置组件声明宏 (简化版)

```cpp
// core/component/ComponentMacros.h

// 声明内置组件类
#define CC_COMPONENT_DECLARE(className, builtinId)             \
public:                                                        \
    static constexpr uint32_t COMPONENT_TYPE_ID = builtinId;   \
    uint32_t getComponentTypeId() const override { return builtinId; } \
    bool isBuiltin() const override { return true; }
```

---

## 2. Component 基类

### 2.1 类设计

```cpp
// core/component/Component.h
namespace cc {

class Node;
class ComponentScheduler;

class Component : public CCObject {
public:
    using Super = CCObject;

    Component() = default;
    ~Component() override = default;

    // ============================
    // 核心属性
    // ============================

    // 所属节点
    inline Node* getNode() const { return _node; }
    void setNode(Node* node); // 仅 Node::addComponent 调用

    // 启用状态
    inline bool isEnabled() const { return _enabled; }
    void setEnabled(bool value); // 联动 ComponentScheduler

    inline bool isEnabledInHierarchy() const {
        return _enabled && _node && _node->isActiveInHierarchy();
    }

    // 唯一 ID
    inline const ccstd::string& getId() const { return _id; }

    // ============================
    // 类型信息
    // ============================
    virtual uint32_t getComponentTypeId() const = 0;
    virtual bool isBuiltin() const { return true; }

    // ============================
    // 生命周期方法 (子类覆写)
    // ============================
protected:
    virtual void __preload() {}       // 内部初始化
    virtual void onLoad() {}          // 节点激活时
    virtual void start() {}           // 第一次 update 前
    virtual void update(float dt) {}  // 每帧
    virtual void lateUpdate(float dt) {} // 每帧（update 后）
    virtual void onEnable() {}        // 组件启用 + 节点激活
    virtual void onDisable() {}       // 组件禁用 / 节点停用
    virtual void onDestroy() {}       // 销毁时

public:
    // 生命周期方法检测 (调度器使用)
    // 内置组件由子类覆写返回 true/false
    // 用户脚本由 ScriptComponent 根据注册信息返回
    virtual bool hasUpdateMethod() const { return false; }
    virtual bool hasLateUpdateMethod() const { return false; }
    virtual bool hasStartMethod() const { return false; }

    // ============================
    // 序列化 (参见 master spec §4)
    // ============================
    virtual void deserializeBinary(const uint8_t* data, uint32_t size) {}
    virtual ccstd::vector<Asset*> getAssetProperties() const { return {}; }

    // ============================
    // 销毁
    // ============================
    bool destroy() override;
    void destruct() override;

protected:
    bool onPreDestroy() override;

    // ============================
    // 定时器代理 (G-3 补全)
    // ============================
public:
    void schedule(const ccSchedulerFunc& callback, float interval,
                  unsigned int repeat, float delay) {
        auto* scheduler = Director::getInstance()->getScheduler();
        scheduler->schedule(callback, this, interval, repeat, delay, !_enabled);
    }
    void scheduleOnce(const ccSchedulerFunc& callback, float delay);
    void unschedule(const ccstd::string& key) {
        auto* scheduler = Director::getInstance()->getScheduler();
        scheduler->unschedule(key, this);
    }
    void unscheduleAllCallbacks();

private:
    Node* _node{nullptr};
    bool _enabled{true};
    ccstd::string _id; // 由 IDGenerator 生成

    friend class Node;
    friend class ComponentScheduler;
    friend class NodeActivator;
};

} // namespace cc
```

### 2.2 内置组件示例

```cpp
// 场景: Sprite 组件的 C++ 实现
namespace cc {

class Sprite : public Component {
    CC_COMPONENT_DECLARE(Sprite, BUILTIN_SPRITE)
public:
    Sprite() = default;
    ~Sprite() override = default;

    // 属性
    IntrusivePtr<SpriteFrame> spriteFrame_{nullptr};
    SpriteType type_{SpriteType::SIMPLE};
    SpriteSizeMode sizeMode_{SpriteSizeMode::TRIMMED};

    // 生命周期
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;

    // 生命周期检测
    bool hasUpdateMethod() const override { return _needsUpdate; }

    // 渲染相关
    void markForUpdate();
    RenderHandle* getRenderHandle() const { return _renderHandle.get(); }

    // 序列化
    void deserializeBinary(const uint8_t* data, uint32_t size) override;
    ccstd::vector<Asset*> getAssetProperties() const override {
        ccstd::vector<Asset*> assets;
        if (spriteFrame_) assets.push_back(spriteFrame_.get());
        return assets;
    }

private:
    bool _needsUpdate{false};
    std::unique_ptr<RenderHandle> _renderHandle;
};

} // namespace cc

// 注册 (静态初始化, 使用统一 TypeRegistry)
CC_REGISTER_BUILTIN(cc::Sprite, "cc.Sprite", cc::BUILTIN_SPRITE)
```

### 2.3 ScriptComponent — 统一占位类 (I-3 修订)

> **I-3 裁决**: 合并 v1.0 的 `ScriptComponent` (组件框架) 与 Scene 系统的 `ScriptComponentPlaceholder` (延迟绑定) 为统一的 `ScriptComponent`。
> 完整定义见 `design-cpp-master-spec.md` §2.4。

```cpp
// core/component/ScriptComponent.h
namespace cc {

// 统一的 JS 脚本组件 C++ 代理
// 合并了原 ScriptComponent (生命周期代理) 和 ScriptComponentPlaceholder (反序列化占位)
class ScriptComponent : public Component {
public:
    // === Component 接口 ===
    uint32_t getComponentTypeId() const override { return _typeId; }
    bool isBuiltin() const override { return false; }

    // 生命周期代理 → ScriptBridge
    void __preload() override;
    void onLoad() override;
    void start() override;
    void update(float dt) override;
    void lateUpdate(float dt) override;
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;

    // 生命周期检测
    bool hasUpdateMethod() const override { return _hasUpdate; }
    bool hasLateUpdateMethod() const override { return _hasLateUpdate; }
    bool hasStartMethod() const override { return _hasStart; }

    // === 反序列化支持 (原 ScriptComponentPlaceholder 功能) ===
    void setScriptClassPath(const ccstd::string& path) { _scriptClassPath = path; }
    const ccstd::string& getScriptClassPath() const { return _scriptClassPath; }
    void setSerializedProps(const uint8_t* data, uint32_t size);
    const ccstd::vector<uint8_t>& getSerializedProps() const { return _serializedProps; }

    // === JS 对象绑定 ===
    void bindScriptObject(se::Object* jsObj);
    se::Object* getScriptObject() const { return _jsObject; }
    bool isBound() const { return _bound; }

    // === Asset 属性 (通过 JS 桥接获取, 供 ReleaseManager 使用) ===
    ccstd::vector<Asset*> getAssetProperties() const override;

private:
    uint32_t _typeId{0};
    ccstd::string _scriptClassPath;
    ccstd::vector<uint8_t> _serializedProps;
    se::Object* _jsObject{nullptr};
    bool _bound{false};
    bool _hasStart{false};
    bool _hasUpdate{false};
    bool _hasLateUpdate{false};

    // 安全的 JS 回调执行 (G-8 GC 协议)
    template<typename Func>
    void safeCallJS(Func&& fn) {
        if (!_jsObject || !_bound) return;
        se::AutoHandleScope scope;
        if (_jsObject->isDead()) {
            _bound = false;
            return;
        }
        fn(_jsObject);
    }
};

} // namespace cc
```

---

## 3. Node 组件管理 (补全注释掉的代码)

### 3.1 修改 Node.h

```cpp
// 在 native/cocos/core/scene-graph/Node.h 中取消注释并完善:

class Node : public CCObject {
    // ... 现有代码 ...

public:
    // ================================
    // 组件管理 API (补全注释掉的代码)
    // ================================

    // 按类型添加组件 (C++ 模板版)
    template <typename T, typename = std::enable_if_t<std::is_base_of_v<Component, T>>>
    T* addComponent() {
        auto* comp = cc::make_intrusive<T>();
        return static_cast<T*>(addComponentInternal(comp.get(), T::getTypeId()));
    }

    // 按类名添加组件 (字符串版, 用于反序列化和 JS 桥接)
    Component* addComponent(const ccstd::string& className);
    Component* addComponent(ComponentTypeId typeId);

    // 按类型获取组件
    template <typename T, typename = std::enable_if_t<std::is_base_of_v<Component, T>>>
    T* getComponent() const {
        ComponentTypeId tid = T::getTypeId();
        for (auto* comp : _components) {
            if (comp->getComponentTypeId() == tid) {
                return static_cast<T*>(comp);
            }
        }
        return nullptr;
    }

    // 按类名获取组件 (兼容 JS, 使用 instanceof 语义)
    Component* getComponent(const ccstd::string& className) const;
    Component* getComponent(ComponentTypeId typeId) const;

    // 获取所有匹配组件
    template <typename T>
    ccstd::vector<T*> getComponents() const {
        ccstd::vector<T*> result;
        ComponentTypeId tid = T::getTypeId();
        for (auto* comp : _components) {
            if (comp->getComponentTypeId() == tid) {
                result.push_back(static_cast<T*>(comp));
            }
        }
        return result;
    }

    // 在子节点中查找
    template <typename T>
    T* getComponentInChildren() const;
    template <typename T>
    ccstd::vector<T*> getComponentsInChildren() const;

    // 移除组件
    void removeComponent(Component* comp);

    // 获取所有组件 (只读)
    inline const ccstd::vector<IntrusivePtr<Component>>& getComponents() const { return _components; }

private:
    // 组件添加的内部实现
    Component* addComponentInternal(Component* comp, ComponentTypeId typeId);

    // 从数组中移除
    void removeComponentFromArray(Component* comp);

    // 依赖组件检查与自动添加
    void checkRequireComponent(ComponentTypeId typeId);

    // 双查找策略 (对齐 TS 的 _findComponent)
    Component* findComponentByTypeId(ComponentTypeId typeId) const;
    Component* findComponentByClassName(const ccstd::string& className) const;

    // 组件存储
    ccstd::vector<IntrusivePtr<Component>> _components;

    friend class Component;
    friend class ComponentScheduler;
    friend class NodeActivator;
};
```

### 3.2 addComponent 完整流程 (C++ 版)

```
Node::addComponent<T>()
    ↓
addComponentInternal(comp, typeId)
    ↓
[Scene 特殊检查] Scene 禁止 addComponent → 抛出错误
    ↓
[disallowMultiple 检查] 同类型组件是否已存在
    ↓
[requireComponent 递归] 自动添加依赖组件
    ↓
comp->setNode(this)              ← 双向关联
comp->addRef()                   ← 引用计数 +1
_components.push_back(comp)      ← 加入数组
    ↓
emit ComponentAdded 事件
    ↓
[节点已激活?] isActiveInHierarchy()
    ├── 是 → NodeActivator::activateComp(comp)
    │          ├── __preload()
    │          ├── onLoad()
    │          ├── onEnable()
    │          └── [调度注册] start/update/lateUpdate
    └── 否 → 延迟到节点激活时
```

### 3.3 getComponent 双查找策略 (C++ 版)

```cpp
Component* Node::findComponentByTypeId(ComponentTypeId typeId) const {
    // 快速路径: 精确类型 ID 匹配 (对齐 TS 的 _sealed 查找)
    for (auto* comp : _components) {
        if (comp->getComponentTypeId() == typeId) {
            return comp;
        }
    }
    return nullptr;
}

Component* Node::findComponentByClassName(const ccstd::string& className) const {
    // 慢速路径: 类名匹配 + 继承链查找
    // 用于 JS 脚本组件的 instanceof 语义
    ComponentTypeId typeId = TypeRegistry::getInstance().getClassIdByName(className);
    if (typeId != 0) {
        return findComponentByTypeId(typeId);
    }
    // 如果是用户脚本类名, 需要遍历 + 桥接检查
    for (auto* comp : _components) {
        if (auto* script = dynamic_cast<ScriptComponent*>(comp)) {
            // 通过 JS 桥接检查 instanceof
            if (ScriptComponentBridge::isInstanceOf(script, className)) {
                return comp;
            }
        }
    }
    return nullptr;
}
```

---

## 4. ComponentScheduler — 双层三桶合并 (I-2 修订)

> **I-2 裁决**: 合并 v1.0 的 `ThreeBucketArray` + `Invoker` 模式与脚本系统的 `vector<Component*>` + `vector<uint32_t>` 分离模式为**统一的双层三桶调度**。
> 完整定义见 `design-cpp-master-spec.md` §2.3。

### 4.1 整体架构

```
ComponentScheduler
    ├── BuiltinSchedule (内置组件: 三桶排序, 保持 executionOrder 语义)
    │   ├── ThreeBucketArray start
    │   ├── ThreeBucketArray update
    │   └── ThreeBucketArray lateUpdate
    ├── ScriptSchedule (用户脚本: 批量调用, 不区分 executionOrder)
    │   ├── vector<uint32_t> start
    │   ├── vector<uint32_t> update
    │   └── vector<uint32_t> lateUpdate
    ├── _deferredComps: vector<Component*> ← 帧中新增组件缓冲
    └── _updating: bool                 ← 是否在帧循环中
```

### 4.2 三桶排序实现 (内置组件)

```cpp
// core/component/ThreeBucketArray.h
namespace cc {

// 三桶容器 — 保持 executionOrder 语义
// 对齐 TS 的 _neg / _zero / _pos 分桶
template <typename T>
class ThreeBucketArray {
public:
    void add(T* item, int32_t executionOrder) {
        if (executionOrder < 0) {
            _neg.insert(sortedIndex(_neg, executionOrder), item);
        } else if (executionOrder > 0) {
            _pos.insert(sortedIndex(_pos, executionOrder), item);
        } else {
            _zero.push_back(item);
        }
    }

    void remove(T* item) {
        removeFromSorted(_neg, item);
        removeFromSorted(_zero, item);
        removeFromSorted(_pos, item);
    }

    // 按顺序遍历: neg → zero → pos
    template <typename Func>
    void forEach(Func&& fn) {
        for (auto* c : _neg) fn(c);
        for (auto* c : _zero) fn(c);
        for (auto* c : _pos) fn(c);
    }

    void clear() { _neg.clear(); _zero.clear(); _pos.clear(); }
    bool empty() const { return _neg.empty() && _zero.empty() && _pos.empty(); }

private:
    ccstd::vector<T*> _neg;   // executionOrder < 0
    ccstd::vector<T*> _zero;  // executionOrder == 0
    ccstd::vector<T*> _pos;   // executionOrder > 0
};

} // namespace cc
```

### 4.3 ComponentScheduler 核心实现

```cpp
// core/component/ComponentScheduler.h
namespace cc {

class ComponentScheduler {
public:
    ComponentScheduler();
    ~ComponentScheduler() = default;

    // ====== 帧循环入口 ======
    void startPhase();
    void updatePhase(float dt);
    void lateUpdatePhase(float dt);

    // ====== 内置组件注册 ======
    void scheduleBuiltin(Component* comp);
    void unscheduleBuiltin(Component* comp);

    // ====== 用户脚本注册 ======
    void scheduleScript(uint32_t compId, bool hasStart, bool hasUpdate, bool hasLateUpdate);
    void unscheduleScript(uint32_t compId);

    // ====== 组件启用/禁用 ======
    void enableComp(Component* comp);
    void disableComp(Component* comp);

    // ====== 销毁辅助 ======
    void destroyComp(Component* comp);

private:
    // 内置组件: 三桶排序 (保持 executionOrder 语义)
    struct BuiltinSchedule {
        ThreeBucketArray<Component> start;     // executionOrder < 0 / == 0 / > 0
        ThreeBucketArray<Component> update;
        ThreeBucketArray<Component> lateUpdate;
    } _builtin;

    // 用户脚本: 批量调用 (不区分 executionOrder, JS 侧自行排序)
    struct ScriptSchedule {
        ccstd::vector<uint32_t> start;
        ccstd::vector<uint32_t> update;
        ccstd::vector<uint32_t> lateUpdate;
    } _script;

    // 帧中缓冲
    ccstd::vector<Component*> _deferredComps;
    bool _updating{false};

    void processDeferred();
};

// ====== 帧调度执行 ======

inline void ComponentScheduler::startPhase() {
    _updating = true;

    // 1. 内置组件 start (三桶排序遍历)
    _builtin.start.forEach([](Component* comp) {
        comp->start();
    });
    _builtin.start.clear();

    // 2. 用户脚本 start (批量 JS 调用)
    if (!_script.start.empty()) {
        ScriptBridge::getInstance().invokeStartBatch(_script.start);
        _script.start.clear();
    }

    _updating = false;
    processDeferred();
}

inline void ComponentScheduler::updatePhase(float dt) {
    _updating = true;

    // 1. 内置组件 update (三桶排序遍历)
    _builtin.update.forEach([dt](Component* comp) {
        comp->update(dt);
    });

    // 2. 用户脚本 update (批量 JS 调用)
    if (!_script.update.empty()) {
        ScriptBridge::getInstance().invokeUpdateBatch(_script.update, dt);
    }

    _updating = false;
    processDeferred();
}

inline void ComponentScheduler::lateUpdatePhase(float dt) {
    _updating = true;

    // 1. 内置组件 lateUpdate
    _builtin.lateUpdate.forEach([dt](Component* comp) {
        comp->lateUpdate(dt);
    });

    // 2. 用户脚本 lateUpdate
    if (!_script.lateUpdate.empty()) {
        ScriptBridge::getInstance().invokeLateUpdateBatch(_script.lateUpdate, dt);
        _script.lateUpdate.clear();
    }

    _updating = false;
    processDeferred();
}

// ====== 内置组件调度注册 ======

inline void ComponentScheduler::scheduleBuiltin(Component* comp) {
    auto* info = TypeRegistry::getInstance().getTypeInfo(comp->getComponentTypeId());
    if (!info) return;

    int32_t order = info->executionOrder;

    // start (仅一次)
    if (comp->hasStartMethod()) {
        if (!(comp->_objFlags & CCObject::Flags::IS_START_CALLED)) {
            _builtin.start.add(comp, order);
        }
    }
    // update (每帧)
    if (comp->hasUpdateMethod()) {
        _builtin.update.add(comp, order);
    }
    // lateUpdate (每帧)
    if (comp->hasLateUpdateMethod()) {
        _builtin.lateUpdate.add(comp, order);
    }
}

} // namespace cc
```

### 4.4 异常恢复 (G-6 统一错误传播)

遵循 master spec §1.5 的统一错误传播模型：

```cpp
// C++ 内置组件: 无异常, assert + 跳过
inline void ComponentScheduler::updatePhase(float dt) {
    _updating = true;
    _builtin.update.forEach([dt](Component* comp) {
        // Release: 直接调用, 崩溃即崩溃 (不应崩溃)
        // Debug: CC_ASSERT 守卫关键前提条件
        CC_ASSERT(comp->isValid());
        comp->update(dt);
    });
    // ... 脚本部分由 ScriptBridge 内部 try-catch 处理
}

// JS 用户脚本: try-catch 包裹 (在 ScriptBridge 批量调用中)
// 参见 design-cpp-script-system.md §3.3
```

---

## 5. NodeActivator (C++ 实现)

### 5.1 设计

```cpp
// core/scene-graph/NodeActivator.h
namespace cc {

class NodeActivator {
public:
    // 激活节点 (递归)
    void activateNode(Node* node, bool active);

    // 激活单个组件
    void activateComp(Component* comp, Invoker* preload = nullptr,
                       Invoker* onLoad = nullptr, Invoker* onEnable = nullptr);

    // 销毁组件
    void destroyComp(Component* comp);

private:
    // 递归激活
    void activateNodeRecursively(Node* node, Invoker* preload,
                                   Invoker* onLoad, Invoker* onEnable);

    // 递归反激活
    void deactivateNodeRecursively(Node* node);

    // 对象池复用 ActivateTask
    struct ActivateTask {
        std::unique_ptr<Invoker> preload;  // UnsortedInvoker
        std::unique_ptr<Invoker> onLoad;   // OneOffInvoker
        std::unique_ptr<Invoker> onEnable; // OneOffInvoker
    };

    Pool<ActivateTask, 4> _taskPool;
    ComponentScheduler* _compScheduler{nullptr};

    friend class Director;
};

} // namespace cc
```

### 5.2 激活流程 (C++ 版)

```cpp
void NodeActivator::activateNode(Node* node, bool active) {
    if (active) {
        auto* task = _taskPool.get();
        if (!task) {
            task = new ActivateTask{
                std::make_unique<UnsortedInvoker>(),
                std::make_unique<OneOffInvoker>(),
                std::make_unique<OneOffInvoker>()
            };
        }

        activateNodeRecursively(node,
            task->preload.get(), task->onLoad.get(), task->onEnable.get());

        // 批量调用 (对齐 TS 的先递归遍历、后批量调用)
        task->preload->invoke(0);
        task->onLoad->invoke(0);
        task->onEnable->invoke(0);

        _taskPool.recycle(task);
    } else {
        deactivateNodeRecursively(node);
    }
}

void NodeActivator::activateComp(Component* comp, Invoker* preload,
                                   Invoker* onLoad, Invoker* onEnable) {
    if (!comp->isValid()) return;

    // __preload
    if (!(comp->_objFlags & CCObject::Flags::IS_PRELOAD_STARTED)) {
        comp->_objFlags |= CCObject::Flags::IS_PRELOAD_STARTED;
        if (preload && comp->getInternalOnLoad()) { // 有 __preload
            preload->add(comp);
        }
    }

    // onLoad
    if (!(comp->_objFlags & CCObject::Flags::IS_ON_LOAD_STARTED)) {
        comp->_objFlags |= CCObject::Flags::IS_ON_LOAD_STARTED;
        if (auto fn = comp->getInternalOnLoad()) {
            if (onLoad) onLoad->add(comp);
            else (comp->*fn)();
        }
        comp->_objFlags |= CCObject::Flags::IS_ON_LOAD_CALLED;
    }

    // onEnable
    if (comp->isEnabled() && comp->getNode()->isActiveInHierarchy()) {
        _compScheduler->enableComp(comp, onEnable);
    }
}
```

---

## 6. JS 桥接层

### 6.1 桥接架构

```
┌─────────────────────────────────────────────────────────┐
│                   C++ Engine Core                        │
│  ┌──────────────┐ ┌──────────────┐ ┌─────────────────┐ │
│  │ Node (C++)   │ │ Component    │ │ Component       │ │
│  │              │ │ Scheduler    │ │ TypeRegistry    │ │
│  │ _components  │ │ (C++)        │ │ (C++)           │ │
│  └──────┬───────┘ └──────┬───────┘ └───────┬─────────┘ │
│         │                │                  │           │
│  ┌──────┴────────────────┴────────────────┬┴─────────┐ │
│  │         C++/JS Bridge Layer             │          │ │
│  │  ┌───────────────────┐  ┌─────────────┐│          │ │
│  │  │ ScriptComponent   │  │ BridgeFuncs ││          │ │
│  │  │ (C++ proxy)       │  │ (注册/回调) ││          │ │
│  │  └─────────┬─────────┘  └──────┬──────┘│          │ │
│  └────────────┼────────────────────┼──────┘          │ │
└───────────────┼────────────────────┼──────────────────┘
                │                    │
┌───────────────┴────────┐  ┌───────┴───────────────────┐
│ Built-in Components    │  │ User Scripts (JS Engine)  │
│ (Pure C++ path)        │  │                           │
│ Sprite/Camera/RigidBody│  │ @ccclass/@property        │
│ → C++ direct call      │  │ Custom Components         │
│ → Zero JS overhead     │  │ → ScriptComponent proxy   │
└────────────────────────┘  └───────────────────────────┘
```

### 6.2 桥接接口

```cpp
// bridge/ScriptComponentBridge.h
namespace cc {

class ScriptComponentBridge {
public:
    // 注册 JS 脚本组件类型到统一 TypeRegistry (运行时)
    static uint32_t registerScriptClass(
        const ccstd::string& className,
        const ccstd::string& uuid,
        bool hasUpdate, bool hasLateUpdate, bool hasStart,
        int32_t executionOrder,
        uint32_t requireComponent,
        bool disallowMultiple);

    // 创建 JS 脚本组件实例 (返回 ScriptComponent)
    static IntrusivePtr<Component> createScriptComponent(uint32_t classId);

    // 调用 JS 侧生命周期方法 (通过 ScriptBridge 批量调用)
    static void invokeLifecycle(ScriptComponent* comp, const char* methodName);
    static void invokeUpdate(ScriptComponent* comp, float dt);

    // Asset 引用收集 (供 ReleaseManager 使用)
    static ccstd::vector<Asset*> collectAssetRefs(ScriptComponent* comp);

    // instanceof 检查
    static bool isInstanceOf(ScriptComponent* comp, const ccstd::string& className);
};

} // namespace cc
```

### 6.3 JS 侧注册代码 (概念示例)

```javascript
// 引擎初始化时，C++ 侧暴露注册接口到 JS
// jsb.registerScriptClass(info)

// 用户脚本加载后，@ccclass 装饰器触发:
@ccclass('MyScript')
class MyScript extends Component {
    @property(Number)
    speed = 10;

    start() { /* ... */ }
    update(dt) { /* ... */ }
}

// → 底层调用:
jsb.registerScriptClass({
    className: 'MyScript',
    uuid: 'xxxx-yyyy',
    hasUpdate: true,
    hasStart: true,
    executionOrder: 0,
    requireComponent: null,
    disallowMultiple: false,
    properties: [
        { name: 'speed', type: 'Number', default: 10 }
    ]
});
```

---

## 7. 性能对比分析

### 7.1 调度开销对比

| 操作 | TS 版本 | C++ 版本 | 提升 |
|------|---------|----------|------|
| update 调用 (内置组件) | JS→C++ 桥接 + JS 函数调用 | C++ 虚函数调用 | ~10x |
| getComponent (内置) | 遍历 _components + constructor === | 遍历 + TypeId 整数比较 | ~3x |
| addComponent (内置) | new constructor() + JS 属性设置 | make_intrusive + 构造 | ~5x |
| ComponentScheduler.tick | JIT 编译循环 + try-catch | 直接循环 (无异常) | ~2x |
| 对象创建/GC | V8 GC 压力 | RefCounted (无 GC) | ~5x |

### 7.2 内存占用对比

| 对象 | TS 版本 | C++ 版本 | 节省 |
|------|---------|----------|------|
| Component 基类 | ~120 字节 (V8 对象头 + 属性) | ~64 字节 | 47% |
| Sprite 组件 | ~300 字节 | ~128 字节 | 57% |
| Node._components | Array 对象 + 元素 | vector<IntrusivePtr> | ~30% |

### 7.3 桥接调用消除

**当前 TS→C++ 调用链 (每帧)**:
```
JS update() → JS Component.update() → [JSB桥接] → C++ Sprite._updateRenderData()
```
**C++ 化后**:
```
C++ ComponentScheduler → C++ Sprite.update() → C++ Sprite._updateRenderData()
```
**消除**: 1 次 JSB 桥接调用/组件/帧

---

## 8. 统一工程约束 (G-1~G-15 补全)

> 以下内容补全 v1.0 遗漏项，完整定义见 master spec 对应章节。

### 8.1 主游戏循环 (G-1)

组件调度器是 Director.tick 的一部分，完整流程见 master spec §3.1：

```
Game::tick(dt)
    └── Director::tick(dt)
        ├── 1. 处理输入事件
        ├── 2. Scheduler::update(dt)           ← 定时器
        ├── 3. ComponentScheduler::startPhase() ← 首次 start
        ├── 4. ComponentScheduler::updatePhase(dt)
        ├── 5. ComponentScheduler::lateUpdatePhase(dt)
        ├── 6. 物理步进 / 动画更新 / 渲染
        └── 7. 延迟销毁处理
```

### 8.2 线程模型 (G-5)

- **主线程独占**: ComponentScheduler 仅在主线程执行，内部数据无需锁
- **Worker 线程限制**: 不触碰 CCObject/Node/Component 树
- 详见 master spec §1.4

### 8.3 错误传播 (G-6)

| 错误源 | 处理方式 |
|--------|---------|
| C++ 内置组件崩溃 | `CC_ASSERT` (Debug) / 静默跳过 (Release) |
| C++ 生命周期异常 | 返回 `bool` / 错误码，不使用 C++ 异常 |
| JS 用户脚本异常 | `try-catch` 包裹 (ScriptBridge 内部) |
详见 master spec §1.5

### 8.4 GC 交互协议 (G-8)

- `CCObject::_scriptObject` 是弱引用，不阻止 GC
- `ScriptComponent::_jsObject` 是弱引用，GC 后 `isBound()` 返回 false
- 安全调用模板: `safeCallJS()` 检查 `isDead()` 后再执行
详见 master spec §3.6

### 8.5 CMake 构建 (G-7)

```cmake
# native/cocos/core/CMakeLists.txt — 新增
target_sources(cc_core PRIVATE
    component/Component.cpp
    component/ScriptComponent.cpp
    component/ComponentScheduler.cpp
    component/ThreeBucketArray.cpp
    scene-graph/NodeActivator.cpp
    serialization/TypeRegistry.cpp
)
```

### 8.6 回滚策略 (G-14)

| 场景 | 回滚方案 |
|------|---------|
| C++ 内置组件有 bug | `Component::setFallbackToJS(true)` → 回退 JS 组件 |
| 双层调度时序异常 | 切换到全 JS 调度 (降级模式) |
| TypeRegistry 注册不一致 | Debug 模式校验 + 重置功能 |

详见 master spec §7.4

### 8.7 Profiler 钩子 (G-12)

```cpp
// ComponentScheduler 帧时间记录
void ComponentScheduler::updatePhase(float dt) {
    EngineProfiler::beginSection("builtin_update");
    _builtin.update.forEach([dt](Component* comp) { comp->update(dt); });
    EngineProfiler::endSection();

    EngineProfiler::beginSection("script_update");
    if (!_script.update.empty())
        ScriptBridge::getInstance().invokeUpdateBatch(_script.update, dt);
    EngineProfiler::endSection();
}
```

详见 master spec §7.3

### 8.8 基准测试 (G-15)

```
test/benchmarks/
    └── bench-scheduler/         ← 调度器基准
        ├── 100-comp-builtin.scene
        ├── 100-comp-script.scene
        ├── 1000-comp-mixed.scene
        └── bench.cpp
```
详见 master spec §7.5

---

## 9. 统一实施时间线 (I-5 修订)

> **I-5 裁决**: 取消本文档 v1.0 的独立 P0-P5 时间线，统一到 master spec §8 的 26 周时间线。

| 阶段 | 对应 Master Phase | 组件框架工作 |
|------|------------------|------------|
| 基础设施 | Phase 0 (Week 1-3) | TypeRegistry 统一注册表 |
| 组件框架 | Phase 1 (Week 4-7) | Component 基类 + ScriptComponent + ComponentScheduler 双层三桶 + NodeActivator |
| 内置组件 | Phase 4 (Week 17-22) | Camera/Sprite/RigidBody C++ 化 + 渲染/物理集成 |
| 桥接优化 | Phase 5 (Week 23-26) | _tempFloatArray 消除 + 回滚验证 + 性能基准 |

---

## 10. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| TypeRegistry 与 TS CCClass 注册不一致 | 高 | classId 基于 @ccclass 名称哈希 (master spec §4) |
| 内置组件遗漏属性导致 JS 侧断链 | 高 | 自动化属性覆盖测试 |
| 双层调度时序与原 JS 调度不一致 | 高 | 严格保持 start→update→lateUpdate (master spec §9) |
| ScriptBridge 批量调用失败 | 高 | 分批 + 异常隔离 + 单个 try-catch |
| 内存布局不兼容 | 中 | 严格测试共享内存偏移 |
| 循环引用 | 中 | Component 持有 Node 原始指针，Node 持有 Component IntrusivePtr |
