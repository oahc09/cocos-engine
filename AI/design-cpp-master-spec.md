# Cocos Creator v3.8.8 — C++ 化总体设计规范

> 版本: v2.0 (统一修订版)
> 日期: 2026-04-25
> 分支: v3.8.8_custome
> 状态: **Review Required**
> 取代: AI/design-cpp-overview.md (v1.0)

---

## 0. 审阅发现：不一致与遗漏

### 0.1 跨文档不一致（必须统一）

| # | 问题 | 文档 A | 文档 B | 裁决 |
|---|------|--------|--------|------|
| I-1 | **类型注册表重复定义** | Component 框架定义 `ComponentTypeRegistry` (TypeId 枚举 + 运行时注册) | Scene 系统定义 `TypeRegistry` (classId→构造函数) | **统一为单一 `TypeRegistry`**，见 §2.1 |
| I-2 | **ComponentScheduler 重复设计** | Component 框架用 `ThreeBucketArray` + `Invoker` (对齐 TS) | 脚本系统用 `vector<Component*>` + `vector<uint32_t>` 分离 (双层) | **合并为双层 ThreeBucketArray**，见 §2.3 |
| I-3 | **脚本组件占位类名不统一** | Component 框架用 `ScriptComponent` (代理生命周期) | Scene 系统用 `ScriptComponentPlaceholder` (延迟绑定) | **统一为 `ScriptComponent`**，见 §2.4 |
| I-4 | **常驻节点管理归属冲突** | Scene 系统在 Scene.h 增加 `_persistRootNodes` | 同文档在 Director.h 也增加 `_persistRootNodes` | **仅在 Director 管理**，Scene 不持有，见 §3.3 |
| I-5 | **Phase 时间线重叠** | 各文档独立定义 Phase 1-4，互相冲突 | 总览试图汇总但未解冲突 | **统一时间线**，见 §8 |

### 0.2 遗漏项（必须补全）

| # | 遗漏 | 严重度 | 补全位置 |
|---|------|--------|---------|
| G-1 | 主游戏循环 (Game.tick → Director.tick) 未设计 | 高 | §3.1 |
| G-2 | C++ 事件系统未设计 (EventTarget/EventBus) | 高 | §3.5 |
| G-3 | 定时器调度 (Scheduler) 与 Component 集成未设计 | 中 | §3.4 |
| G-4 | 二进制序列化版本迁移策略缺失 | 高 | §4.4 |
| G-5 | 线程模型未定义 | 高 | §1.4 |
| G-6 | 统一错误传播模型缺失 (C++ 无异常 vs JS 可抛异常) | 中 | §1.5 |
| G-7 | 构建系统集成 (CMake/JSB 绑定生成) 未设计 | 高 | §7.1 |
| G-8 | GC 交互协议 (C++ 对象 ↔ JS wrapper) 未详细设计 | 高 | §3.6 |
| G-9 | 渲染管线集成 (Camera/MeshRenderer → RenderScene) 未设计 | 高 | §5.2 |
| G-10 | 物理系统集成未设计 | 中 | §5.3 |
| G-11 | 2D 渲染集成 (Sprite → Render2D) 未设计 | 中 | §5.4 |
| G-12 | 调试/性能分析钩子未设计 | 中 | §7.3 |
| G-13 | 多平台考虑 (iOS/Android/Web) 未分析 | 中 | §7.2 |
| G-14 | 回滚策略缺失 | 高 | §7.4 |
| G-15 | 自动化基准测试基础设施未设计 | 中 | §7.5 |

---

## 1. 架构基础约束

### 1.1 不变量 (Invariants)

以下是所有子系统必须遵守的架构不变量，违反即为设计错误：

| # | 不变量 | 说明 |
|---|--------|------|
| INV-1 | **用户脚本不可 C++ 化** | TS/JS 代码必须由 V8/JSCore 执行 |
| INV-2 | **编辑器始终走 JS 路径** | 编辑器模式 (JS_ONLY) 不接受 C++ 快速路径 |
| INV-3 | **C++ 引用计数为权威源** | AssetRefManager 是唯一引用计数管理者 |
| INV-4 | **生命周期顺序不可变** | start → update → lateUpdate → 下一帧 |
| INV-5 | **JS 侧缓存必须与 C++ 同步** | 加载/释放操作双写 |
| INV-6 | **双模式运行时不可切换** | 启动时确定 JS_ONLY 或 NATIVE_FAST，运行中不变 |

### 1.2 双轨制策略

```
┌──────────────────────────────────────────────────────────┐
│                   运行模式 (启动时选择)                     │
│                                                          │
│  JS_ONLY (编辑器/调试)        NATIVE_FAST (发布/性能)     │
│  ├── JSON 反序列化            ├── 二进制反序列化          │
│  ├── JS 反射创建对象          ├── TypeRegistry 直接创建   │
│  ├── JIT 编译 Prefab          ├── BinaryTemplate 实例化  │
│  ├── 全量 JS 调度             ├── 双层 C++/JS 调度       │
│  └── 热重载支持               └── 零热重载               │
│                                                          │
│  共享: C++ 对象层 (Asset/Node/Scene/Component 基类)       │
└──────────────────────────────────────────────────────────┘
```

### 1.3 四大子系统定位

| 子系统 | 可 C++ 化度 | 核心策略 | 设计文档 |
|--------|------------|---------|---------|
| Component 框架 | ★★★★☆ | 内置组件纯 C++，用户脚本 JS 代理 | §2 |
| Scene 系统 | ★★★★★ | 补全 Director/序列化/Prefab | §3, §4 |
| Asset 系统 | ★★★★☆ | 内置资源 C++ 全权，引用计数统一 | §6 |
| 脚本系统 | ★★☆☆☆ | ScriptBridge 双层调度 | §2.3, §5 |

### 1.4 线程模型 (G-5 补全)

**核心原则**: Cocos Creator 引擎是**单线程主循环**架构。

```
主线程 (Game Thread)
    ├── Game.tick()
    │   ├── Director.tick()
    │   │   ├── ComponentScheduler.startPhase()
    │   │   ├── ComponentScheduler.updatePhase(dt)
    │   │   ├── ComponentScheduler.lateUpdatePhase(dt)
    │   │   └── 渲染提交
    │   └── Scheduler (定时器)
    └── 事件处理

渲染线程 (Render Thread, 仅 Native)
    ├── GPU 命令提交
    └── 与主线程通过 CommandBuffer 同步

Worker 线程 (可选)
    ├── 资源下载 (IO)
    ├── 纹理解码
    └── 通过回调/队列通知主线程
```

**规则**:
1. **主线程 C++ 对象不需要锁** — 所有 C++ 引擎对象仅在主线程访问
2. **异步操作通过回调回到主线程** — 与现有 JS 异步模型一致
3. **Worker 线程仅做纯计算/IO** — 不触碰 CCObject/Node/Component 树
4. **AssetRefManager 无锁** — 单线程访问，无需原子操作

### 1.5 统一错误传播模型 (G-6 补全)

| 错误源 | 传播方式 | 处理策略 |
|--------|---------|---------|
| C++ 内置组件崩溃 | `CC_ASSERT` (Debug) / 静默跳过 (Release) | Debug 崩溃定位，Release 容错 |
| C++ 生命周期异常 | 返回 `bool` / 错误码 | 不使用 C++ 异常 (引擎标准做法) |
| JS 用户脚本异常 | `try-catch` 包裹 | 记录日志，标记组件 `IS_START_CALLED`，继续调度 |
| JSB 桥接异常 | `se::Value::isUndefined()` 检查 | 降级到 JS 路径 |
| 异步加载失败 | `onComplete(error, null)` 回调 | 与现有 JS 错误回调模型一致 |

---

## 2. 统一类型注册与组件框架

### 2.1 TypeRegistry — 统一类型注册表 (I-1 裁决)

合并 `ComponentTypeRegistry` 和 `TypeRegistry` 为单一注册表，同时服务组件系统和序列化系统：

```cpp
// native/cocos/core/serialization/TypeRegistry.h

class TypeRegistry {
public:
    static TypeRegistry& getInstance();

    // === 通用类型注册 ===
    using Constructor = std::function<CCObject*()>;
    using BinaryDeserializer = std::function<void(CCObject*, const uint8_t*, uint32_t)>;
    using AssetPropertiesGetter = std::function<ccstd::vector<Asset*>(CCObject*)>;

    struct TypeInfo {
        uint32_t classId;              // 唯一类型 ID
        ccstd::string className;       // "cc.Sprite", "cc.Camera"
        Constructor constructor;
        BinaryDeserializer deserializer;
        AssetPropertiesGetter assetGetter; // 供 ReleaseManager 用

        // 组件专用
        bool isComponent{false};
        bool isBuiltin{false};         // true=C++ 内置, false=JS 脚本
        bool hasUpdate{false};
        bool hasLateUpdate{false};
        int32_t executionOrder{0};
        uint32_t requireComponent{0};
        bool disallowMultiple{false};
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

private:
    ccstd::unordered_map<uint32_t, TypeInfo> _types;
    ccstd::unordered_map<ccstd::string, uint32_t> _nameToId;
    std::atomic<uint32_t> _nextScriptClassId{10000}; // 脚本类型从 10000 开始
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
```

### 2.2 Component 基类 (统一修订)

```cpp
// native/cocos/core/component/Component.h

class Component : public CCObject {
public:
    using Super = CCObject;

    // === 核心属性 ===
    Node* getNode() const { return _node; }
    bool isEnabled() const { return _enabled; }
    void setEnabled(bool value);
    bool isEnabledInHierarchy() const;

    // === 类型信息 ===
    virtual uint32_t getComponentTypeId() const = 0;
    virtual bool isBuiltin() const { return true; }

    // === 生命周期 (子类覆写) ===
protected:
    virtual void __preload() {}
    virtual void onLoad() {}
    virtual void start() {}
    virtual void update(float dt) {}
    virtual void lateUpdate(float dt) {}
    virtual void onEnable() {}
    virtual void onDisable() {}
    virtual void onDestroy() {}

public:
    // 内部访问器 (调度器检测用)
    virtual bool hasUpdateMethod() const { return false; }
    virtual bool hasLateUpdateMethod() const { return false; }
    virtual bool hasStartMethod() const { return false; }

    // === 序列化 ===
    virtual void deserializeBinary(const uint8_t* data, uint32_t size) {}
    virtual ccstd::vector<Asset*> getAssetProperties() const { return {}; }

    // === 销毁 ===
    bool destroy() override;
    void destruct() override;

private:
    Node* _node{nullptr};
    bool _enabled{true};
    friend class Node;
    friend class ComponentScheduler;
    friend class NodeActivator;
};
```

### 2.3 ComponentScheduler — 双层三桶合并 (I-2 裁决)

```cpp
// native/cocos/core/component/ComponentScheduler.h

class ComponentScheduler {
public:
    // === 帧循环入口 ===
    void startPhase();
    void updatePhase(float dt);
    void lateUpdatePhase(float dt);

    // === 内置组件注册 ===
    void scheduleBuiltin(Component* comp);
    void unscheduleBuiltin(Component* comp);

    // === 用户脚本注册 ===
    void scheduleScript(uint32_t compId, bool hasStart, bool hasUpdate, bool hasLateUpdate);
    void unscheduleScript(uint32_t compId);

    // === 组件启用/禁用 ===
    void enableComp(Component* comp);
    void disableComp(Component* comp);

private:
    // 内置组件: 三桶排序 (保持 executionOrder 语义)
    struct BuiltinSchedule {
        ThreeBucketArray start;     // executionOrder < 0 / == 0 / > 0
        ThreeBucketArray update;
        ThreeBucketArray lateUpdate;
    } _builtin;

    // 用户脚本: 批量调用 (不区分 executionOrder，JS 侧自行排序)
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
```

### 2.4 ScriptComponent — 统一占位类 (I-3 裁决)

```cpp
// native/cocos/core/component/ScriptComponent.h

// 合并 ScriptComponent (Component 框架) 和 ScriptComponentPlaceholder (Scene 系统)
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

    // === Asset 属性 (通过 JS 桥接获取) ===
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
};
```

---

## 3. 主循环与场景管理

### 3.1 主游戏循环 (G-1 补全)

```
Game::tick(dt)
    ├── Director::tick(dt)
    │   ├── 1. 处理输入事件
    │   ├── 2. Scheduler::update(dt)           ← 定时器
    │   ├── 3. ComponentScheduler::startPhase() ← 首次 start
    │   ├── 4. ComponentScheduler::updatePhase(dt)
    │   ├── 5. ComponentScheduler::lateUpdatePhase(dt)
    │   ├── 6. 物理步进 (if physics enabled)
    │   ├── 7. 动画更新
    │   ├── 8. 渲染场景更新
    │   │   ├── RenderScene::update(dt)
    │   │   ├── Pipeline::render()
    │   │   └── Root::frameMove()
    │   └── 9. 延迟销毁处理
    │       └── CCObject::deferredDestroy()
    └── emit END_FRAME
```

```cpp
// native/cocos/core/Director.h

class Director : public RefCounted {
public:
    static Director* getInstance();

    // === 主循环 ===
    void tick(float dt);

    // === 场景管理 ===
    void loadScene(const ccstd::string& name, /* callbacks */);
    void runScene(Scene* scene, /* callbacks */);
    void runSceneImmediate(Scene* scene, /* callbacks */);
    Scene* getScene() const { return _scene.get(); }

    // === 常驻节点 (I-4 裁决: 仅 Director 管理) ===
    void addPersistRootNode(Node* node);
    void removePersistRootNode(Node* node);
    bool isPersistRootNode(Node* node) const;

    // === 子系统访问 ===
    ComponentScheduler* getCompScheduler() const { return _compScheduler.get(); }
    NodeActivator* getNodeActivator() const { return _nodeActivator.get(); }
    Scheduler* getScheduler() const { return _scheduler.get(); }

private:
    IntrusivePtr<Scene> _scene;
    ccstd::string _loadingScene;
    ccstd::unordered_map<ccstd::string, IntrusivePtr<Node>> _persistRootNodes;
    IntrusivePtr<ComponentScheduler> _compScheduler;
    IntrusivePtr<NodeActivator> _nodeActivator;
    IntrusivePtr<Scheduler> _scheduler;
};
```

### 3.2 常驻节点管理 (I-4 裁决)

**裁决**: 常驻节点**仅由 Director 管理**。Scene.h **不增加** `_persistRootNodes`。

理由：
- TS 源码中 `addPersistRootNode/removePersistRootNode` 是 Director 的方法
- 常驻节点跨场景存活，逻辑上属于 Director 而非任何 Scene
- 避免双份数据不一致

### 3.3 NodeActivator (C++ 实现)

```cpp
// native/cocos/core/scene-graph/NodeActivator.h

class NodeActivator {
public:
    // 激活节点 (递归，批量调用)
    void activateNode(Node* node, bool active);

    // 激活单个组件
    void activateComp(Component* comp);

private:
    void activateNodeRecursively(Node* node);
    void deactivateNodeRecursively(Node* node);

    // 批量调用缓存 (对齐 TS: 先遍历后批量)
    ccstd::vector<Component*> _preloadComps;
    ccstd::vector<Component*> _onLoadComps;
    ccstd::vector<Component*> _onEnableComps;

    ComponentScheduler* _compScheduler{nullptr};
};
```

### 3.4 定时器调度集成 (G-3 补全)

```cpp
// Component 的 schedule/unschedule 代理到全局 Scheduler
void Component::schedule(const ccSchedulerFunc& callback, float interval,
                          unsigned int repeat, float delay) {
    auto* scheduler = Director::getInstance()->getScheduler();
    scheduler->schedule(callback, this, interval, repeat, delay, !_enabled);
}

void Component::unschedule(const ccstd::string& key) {
    auto* scheduler = Director::getInstance()->getScheduler();
    scheduler->unschedule(key, this);
}
```

### 3.5 C++ 事件系统 (G-2 补全)

已有的 C++ 事件基础设施：

| 组件 | 文件 | 说明 |
|------|------|------|
| `EventTarget` | `native/cocos/core/event/EventTarget.h` (30.9KB) | **已有完整实现** |
| `EventBus` | `native/cocos/core/event/EventBus.h` (8.6KB) | **已有完整实现** |
| `Event` | `native/cocos/core/event/Event.h` | 基础事件类 |

**结论**: 事件系统**无需重新设计**，直接使用现有 C++ 实现。新增工作仅为：
1. Component 继承 EventTarget（已有，CCObject → EventTarget 链路完整）
2. Node 事件 (emit/on) 已有 C++ 实现
3. 新增场景加载事件 (`SceneLoad`, `AfterSceneLaunch`) 使用 EventBus

### 3.6 GC 交互协议 (G-8 补全)

```
C++ 对象生命周期                      JS 包装器生命周期
─────────────────                    ───────────────────

CCObject 构造                         se::Object 创建
    │                                     │
    │ ← setScriptObject(seObj) ───────────┘
    │                                     │
    │ (C++ 引用计数管理)                   │ (V8 GC 管理)
    │                                     │
CCObject::destroy()                   se::Object 弱引用失效
    │                                     │
    ├── _scriptObject = nullptr           │
    ├── destruct() 清理 C++ 资源          │
    │                                     │
CCObject 析构                         V8 GC 回收 se::Object
```

**关键协议**:
1. **C++ → JS**: `CCObject::_scriptObject` 是弱引用，不阻止 GC
2. **JS → C++**: `se::Object::getNativeObject()` 持有 C++ 原始指针
3. **销毁时序**: C++ `destroy()` 先执行 → 设 `_scriptObject = nullptr` → JS 侧 `isValid()` 返回 false
4. **ScriptComponent 特殊**: `_jsObject` 是弱引用，JS GC 后 C++ 侧 `isBound()` 返回 false

```cpp
// 安全的 JS 回调执行
template<typename Func>
void ScriptComponent::safeCallJS(Func&& fn) {
    if (!_jsObject || !_bound) return;
    se::AutoHandleScope scope;
    if (_jsObject->isDead()) {
        _bound = false;
        return;
    }
    fn(_jsObject);
}
```

---

## 4. 二进制序列化系统

### 4.1 二进制场景格式 (.scene.bin)

(沿用 design-cpp-scene-system.md §4.2 的完整格式设计，此处不重复)

### 4.2 TypeRegistry 集成

序列化的类型查找统一使用 §2.1 定义的 `TypeRegistry`:
- 内置组件: `TypeRegistry::create(classId)` → C++ 直接创建
- 用户脚本: `TypeRegistry::create(classId)` → 返回 `ScriptComponent` 占位

### 4.3 构建时转换

```
编辑器导出 .scene (JSON)
         ↓
  build-scene-binary.js (构建脚本)
  ├── 解析 JSON
  ├── 为每个类查 TypeRegistry 获取 classId
  ├── 内置组件属性 → 固定布局二进制
  ├── 用户脚本属性 → JSON 子段保留
  └── 输出 .scene.bin
         ↓
  打包到 Bundle 中
```

### 4.4 版本迁移策略 (G-4 补全)

```
Header.magic = "CCSC"
Header.version = uint16
```

| 版本 | 说明 | 迁移 |
|------|------|------|
| 1 | 初始版本 | — |
| 2+ | 格式变更 | 构建脚本检测 version → 应用迁移函数 |

**迁移原则**:
1. **向前兼容**: 旧版本 .scene.bin 必须可被新引擎读取
2. **向后不兼容**: 新版本 .scene.bin 不需要被旧引擎读取（构建时重新生成）
3. **版本检测**: `BinaryDeserializer::deserialize()` 首先读取 Header.version，按版本选择解析逻辑
4. **迁移函数注册**: `TypeRegistry::registerMigration(fromVersion, toVersion, migrateFunc)`

---

## 5. 内置组件 C++ 化详细设计

### 5.1 组件 C++ 化模板

```cpp
// native/cocos/core/components/Camera.h

class Camera : public Component {
public:
    // === Component 接口 ===
    uint32_t getComponentTypeId() const override { return BUILTIN_CAMERA; }
    bool isBuiltin() const override { return true; }

    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;

    bool hasUpdateMethod() const override { return true; }
    void update(float dt) override;

    // === Camera 专属 API ===
    // ... (所有 @property 对应的 C++ 属性)

    // === 序列化 ===
    void deserializeBinary(const uint8_t* data, uint32_t size) override;
    ccstd::vector<Asset*> getAssetProperties() const override;

private:
    // 所有 TS @property 的 C++ 镜像
    float _fov{45.0f};
    float _near{1.0f};
    float _far{1000.0f};
    int32_t _priority{0};
    // ...
    IntrusivePtr<scene::Camera> _renderCamera; // 渲染层引用
};

CC_REGISTER_BUILTIN(Camera, "cc.Camera", BUILTIN_CAMERA)
```

### 5.2 渲染管线集成 (G-9 补全)

**核心问题**: C++ Camera/MeshRenderer/Light 需要与 `RenderScene` 交互。

**已有基础**:
- `RenderScene` (C++) 已有完整的模型/光源/相机管理
- `scene::Camera`, `scene::Light` (C++) 已有实现
- `PipelineSceneData` (C++) 已有场景渲染数据

**集成方案**: 内置组件的 `onEnable/onDisable` 直接操作 RenderScene:

```cpp
void Camera::onEnable() {
    if (!_renderCamera) {
        _renderCamera = getNode()->getScene()->getRenderScene()->createCamera();
    }
    _renderCamera->setNode(getNode());
    _renderCamera->activate(true);
}

void Camera::onDisable() {
    if (_renderCamera) {
        _renderCamera->activate(false);
    }
}
```

**MeshRenderer** 类似: `onEnable` → 注册到 RenderScene 的模型列表。

### 5.3 物理系统集成 (G-10 补全)

**策略**: 物理组件 (RigidBody/Collider) 的 C++ 化依赖物理后端 (Cannon/PhysX/Ammo)。

```
RigidBody (C++)
    │
    ├── onEnable() → 调用 PhysicsWorld::addBody()
    ├── update(dt) → 同步变换到物理引擎
    └── onDisable() → 调用 PhysicsWorld::removeBody()
```

**物理后端已有 C++ 基础**: PhysX 和 Ammo 本身是 C++ 库，C++ 化可消除 JS→C++ 物理步进开销。

### 5.4 2D 渲染集成 (G-11 补全)

```
Sprite (C++)
    │
    ├── onEnable() → 注册到 Render2D 系统
    │   ├── 创建 RenderEntity
    │   └── 设置材质/纹理
    ├── update(dt) → 更新顶点数据 (如果脏标记)
    └── onDisable() → 从 Render2D 系统移除
```

**关键**: 2D 渲染的 C++ 化需要 `Render2D` 系统先行 C++ 化，否则 Sprite C++ 化意义有限。建议 Sprite C++ 化延后到 2D 渲染管线 C++ 化完成后。

---

## 6. 资产系统

(沿用 design-cpp-asset-system.md 的 AssetRefManager / NativeAssetManager / NativeBundle / NativeReleaseManager / NativeDependUtil 设计)

### 6.1 与 TypeRegistry 集成

NativeAssetManager 的加载路由使用统一 TypeRegistry:

```cpp
void NativeAssetManager::load(const ccstd::string& uuid, /* ... */) {
    if (_mode == AssetManagerMode::JS_ONLY) {
        callJSLoad(uuid, onComplete);
        return;
    }

    auto* config = findAssetConfig(uuid);
    if (!config || !TypeRegistry::getInstance()->hasType(config->classId)) {
        callJSLoad(uuid, onComplete);  // 降级到 JS
        return;
    }

    loadNativeAsset(*config, onComplete);  // C++ 快速路径
}
```

### 6.2 引用计数与场景切换

```
场景切换 (Director::runSceneImmediate)
    ↓
1. NativeReleaseManager::collectSceneAssetRefs(oldScene)
   ├── 遍历旧场景所有 Node/Component
   ├── 内置组件 → getAssetProperties() (C++)
   └── 用户脚本 → ScriptBridge::collectAssetRefs() (JS 回调)
    ↓
2. 对所有不在新场景引用列表中的 Asset 调用 decRef()
    ↓
3. AssetRefManager 同步引用计数到 JS 侧
```

---

## 7. 工程、调试与运维

### 7.1 构建系统集成 (G-7 补全)

**CMake 集成**:

```cmake
# native/cocos/core/CMakeLists.txt — 新增文件

# 组件系统
target_sources(cc_core PRIVATE
    component/Component.cpp
    component/ScriptComponent.cpp
    component/ComponentScheduler.cpp
    scene-graph/NodeActivator.cpp
    scene-graph/Prefab.cpp
    scene-graph/PrefabUtils.cpp
    serialization/TypeRegistry.cpp
    serialization/BinaryDeserializer.cpp
    assets/AssetRefManager.cpp
    assets/NativeAssetManager.cpp
    assets/NativeBundle.cpp
    assets/NativeReleaseManager.cpp
    assets/NativeDependUtil.cpp
    scripting/ScriptBridge.cpp
    Director.cpp
)
```

**JSB 绑定生成**: 现有 `native/cocos/bindings/auto/` 自动生成机制已支持，新增 C++ 类只需在 `bindings/auto/api/` 中添加 JSB 配置。

### 7.2 多平台考虑 (G-13 补全)

| 平台 | JS 引擎 | 特殊考虑 |
|------|---------|---------|
| Android | V8 | JNI 桥接开销大，C++ 化收益最高 |
| iOS/macOS | JSCore | 已有良好 JSB 支持 |
| Windows | V8 | 同 Android |
| Web | — | **不支持 C++ 快速路径**，仅 JS_ONLY 模式 |
| HarmonyOS | — | 待评估 |

**关键**: Web 平台无 C++ 运行时，`NATIVE_FAST` 模式不适用。构建时需要为 Web 平台跳过 .scene.bin 生成。

### 7.3 调试/性能分析钩子 (G-12 补全)

```cpp
// native/cocos/core/debug/Profiler.h

class EngineProfiler {
public:
    // 帧时间分解
    struct FrameStats {
        float tickTime;           // Director.tick 总时间
        float schedulerTime;      // 定时器时间
        float builtinUpdateTime;  // 内置组件 update 时间
        float scriptUpdateTime;   // 用户脚本 update 时间
        float renderTime;         // 渲染时间
        float gcTime;             // JS GC 时间
        uint32_t builtinCompCount;  // 内置组件数
        uint32_t scriptCompCount;   // 脚本组件数
        uint32_t jsbCallCount;      // JSB 桥接调用次数
    };

    static void beginFrame();
    static void endFrame();
    static FrameStats getLastFrameStats();
    static void enable(bool enabled);
};
```

### 7.4 回滚策略 (G-14 补全)

| 场景 | 回滚方案 |
|------|---------|
| C++ 内置组件有 bug | 运行时回退到 JS 组件: `Component::setFallbackToJS(true)` |
| 二进制反序列化失败 | 自动降级到 JSON + JS 反序列化 |
| C++ 资产加载崩溃 | `loadNativeAsset` 失败 → `callJSLoad` 兜底 |
| 引用计数不一致 | Debug 模式校验 + 重置功能 |
| 整体 C++ 化回退 | `NativeAssetManager::setMode(JS_ONLY)` 全局回退 |

**核心原则**: 每一个 C++ 化功能都必须有 JS 降级路径。

### 7.5 自动化基准测试 (G-15 补全)

```
test/benchmarks/
    ├── bench-scene-load/        ← 场景加载基准
    │   ├── 100-node.scene
    │   ├── 1000-node.scene
    │   └── bench.cpp
    ├── bench-prefab-instantiate/ ← Prefab 实例化基准
    ├── bench-asset-load/         ← 资产加载基准
    ├── bench-scheduler/          ← 调度器基准
    └── bench-results/            ← 结果归档
        ├── js-baseline.json      ← JS_ONLY 模式基准
        └── native-fast.json      ← NATIVE_FAST 模式基准
```

**CI 集成**: 每次提交运行基准，回归超过 10% 自动报警。

---

## 8. 统一实施时间线 (I-5 裁决)

```
Phase 0: 基础设施 (Week 1-3)
    ├── TypeRegistry 统一类型注册表
    ├── AssetRefManager 引用计数统一
    ├── 引用计数 JSB 桥接
    ├── 线程模型文档化
    └── 基准测试框架搭建

Phase 1: 组件框架 + 主循环 (Week 4-7)
    ├── Component 基类 C++ 实现
    ├── ScriptComponent 占位类
    ├── Node 组件管理代码补全
    ├── ComponentScheduler 双层三桶
    ├── NodeActivator C++
    ├── Director C++ (tick/loadScene/runSceneImmediate)
    ├── Scheduler 集成
    └── 主游戏循环打通

Phase 2: 场景 + 序列化 (Week 8-12)
    ├── Scene::load/activate 完善
    ├── 常驻节点管理
    ├── 二进制格式规范 v1
    ├── BinaryDeserializer 实现
    ├── 构建时转换脚本
    ├── Prefab C++ 类 + 二进制模板实例化
    ├── PrefabUtils (expandNested/applyOverrides)
    └── 版本迁移机制

Phase 3: 资产系统 (Week 13-16)
    ├── NativePipeline
    ├── NativeBundle
    ├── NativeDependUtil
    ├── NativeAssetManager
    ├── NativeReleaseManager
    ├── Texture2D/Mesh C++ 完整加载路径
    ├── 双模式切换逻辑
    └── JS 侧缓存同步

Phase 4: 内置组件 C++ 化 (Week 17-22)
    ├── Camera C++ 化 + 渲染集成
    ├── MeshRenderer C++ 化 + 渲染集成
    ├── Light C++ 化 + 渲染集成
    ├── SkeletonAnimation C++ 化
    ├── RigidBody C++ 化 + 物理集成
    ├── Sprite C++ 化 + 2D 渲染集成
    └── 各组件 deserializeBinary + getAssetProperties

Phase 5: 桥接优化 + 集成 (Week 23-26)
    ├── _tempFloatArray 消除
    ├── SharedArrayBuffer 变换共享 (可选)
    ├── native-binding 懒加载 / C++ 侧绑定
    ├── EngineProfiler 调试钩子
    ├── 回滚机制验证
    ├── 全面回归测试
    ├── 性能基准 A/B 对比
    └── 内存泄漏/压力测试

总计: ~6 个月 (26 周)
```

---

## 9. 风险矩阵 (统一修订)

| 风险 | 概率 | 影响 | 缓解 | 负责子系统 |
|------|------|------|------|-----------|
| 二进制格式与 JSON 不兼容 | 中 | 高 | 编辑器始终 JS_ONLY | Scene |
| TypeRegistry 与 TS CCClass 注册不一致 | 中 | 高 | classId 基于 @ccclass 名称哈希 | Component |
| 引用计数同步延迟 | 中 | 中 | 单线程模型，无需锁 | Asset |
| 内置组件遗漏属性导致 JS 侧断链 | 高 | 高 | 自动化属性覆盖测试 | Component |
| ScriptBridge 批量调用失败 | 低 | 高 | 分批 + 异常隔离 + 单个 try-catch | Script |
| 双层调度时序不一致 | 中 | 高 | 严格保持 start→update→lateUpdate | Script |
| 渲染管线集成断裂 | 中 | 高 | 复用已有 RenderScene C++ API | Component |
| 物理系统 C++ 化依赖第三方 | 中 | 中 | PhysX/Ammo 已是 C++ | Component |
| Web 平台不支持 NATIVE_FAST | 低 | 中 | 构建时跳过 .scene.bin | Build |
| C++ 内置组件生产环境 bug | 中 | 高 | 每个组件有 JS fallback | Component |
| 2D 渲染 C++ 化依赖 Render2D | 中 | 中 | Sprite C++ 化延后 | Component |
| 大规模重构导致回归 | 高 | 高 | 渐进式替换 + 每阶段回归测试 | All |

---

## 10. 文件清单与交付物

### 10.1 分析文档 (输入，已完成)

| 文件 | 说明 |
|------|------|
| `AI/analysis-component-framework.md` | 组件框架深度分析 |
| `AI/analysis-scene-system.md` | 场景系统深度分析 |
| `AI/analysis-asset-system.md` | 资产系统深度分析 |
| `AI/analysis-script-system.md` | 用户脚本系统深度分析 |

### 10.2 设计文档 (输出，已完成 + 本次统一修订)

| 文件 | 说明 | 状态 |
|------|------|------|
| `AI/design-cpp-component-framework.md` | C++ 组件框架 | ✅ v1.0 (需按 §2 修订) |
| `AI/design-cpp-scene-system.md` | C++ 场景系统 | ✅ v1.0 (需按 §3-4 修订) |
| `AI/design-cpp-asset-system.md` | C++ 资产系统 | ✅ v1.0 (需按 §6 修订) |
| `AI/design-cpp-script-system.md` | C++ 脚本系统 | ✅ v1.0 (需按 §2.3-2.4 修订) |
| **`AI/design-cpp-master-spec.md`** | **本文档 — 总体设计规范 v2.0** | ✅ 新建 |

### 10.3 修订项跟踪

| 修订项 | 影响文档 | 修订内容 |
|--------|---------|---------|
| I-1 | Component + Scene | TypeRegistry 合并 |
| I-2 | Component + Script | ComponentScheduler 双层三桶合并 |
| I-3 | Component + Scene | ScriptComponent 统一 |
| I-4 | Scene | 常驻节点归 Director |
| I-5 | Overview | 统一时间线 |
| G-1~G-15 | All | 各遗漏项补全 |

---

## 11. 下一步行动

1. **确认本规范** — Review §0 的不一致裁决和遗漏补全方案
2. **修订子文档** — 按本规范的裁决修改四份 v1.0 设计文档为 v2.0
3. **Phase 0 启动** — TypeRegistry + AssetRefManager (最底层，无依赖)
4. **Camera PoC** — 端到端验证: TypeRegistry → BinaryDeserializer → Camera C++ → 渲染
5. **基准建立** — 记录 JS_ONLY 模式各阶段耗时
