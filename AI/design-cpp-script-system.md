# C++ 用户脚本系统设计

> 版本: v2.0 (统一修订版)
> 日期: 2026-04-25
> 基于: AI/analysis-script-system.md
> 目标: 设计 C++ 侧的脚本引擎接口和内置组件/用户脚本混合调度方案
> 总体规范: AI/design-cpp-master-spec.md v2.0
> 修订项: I-1 (TypeRegistry统一), I-2 (双层三桶调度合并), I-3 (ScriptComponent统一), I-5 (统一时间线)
> 补全项: G-5 (线程模型), G-6 (错误传播), G-7 (CMake), G-8 (GC协议), G-12 (Profiler), G-14 (回滚), G-15 (基准)

---

## 1. 核心约束

**用户脚本无法 C++ 化** — 这是架构级约束，不可逾越：

| 方面 | 原因 | 影响 |
|------|------|------|
| 用户代码执行 | TS/JS 必须由 JS 引擎解释/编译 | 必须保留 V8/JSCore |
| @ccclass/@property | 运行时注册到 CCClass 系统 | C++ 无法替代 |
| 脚本反序列化 | 依赖 `getClassByName()` JS 反射 | C++ 无法创建用户组件 |
| 热重载 | 编辑器特性，纯 JS 机制 | C++ 不参与 |

**因此**: C++ 化的目标是**最小化 JS 开销**，而非消除 JS 引擎。

---

## 2. 设计策略：双层调度

### 2.1 核心思想

> **I-2 裁决**: 内置组件使用 `ThreeBucketArray<Component>` (保持 executionOrder 语义)，
> 用户脚本使用 `vector<uint32_t>` (批量 JS 调用)，
> 两层合并为统一 `ComponentScheduler`。
> 完整定义见 `design-cpp-master-spec.md` §2.3 和
> `design-cpp-component-framework.md` §4。

```
┌───────────────────────────────────────────────────────────┐
│               C++ ComponentScheduler (I-2 统一)           │
│                                                           │
│  ┌────────────────────────┐  ┌──────────────────────────┐ │
│  │ BuiltinSchedule        │  │ ScriptSchedule           │ │
│  │ (ThreeBucketArray)     │  │ (vector<uint32_t>)       │ │
│  │                        │  │                          │ │
│  │ ThreeBucketArray start │  │ vector<uint32_t> start   │ │
│  │ ThreeBucketArray update│  │ vector<uint32_t> update  │ │
│  │ ThreeBucketArray late  │  │ vector<uint32_t> late    │ │
│  │                        │  │                          │ │
│  │ → C++ 虚函数直接调用    │  │ → ScriptBridge 批量调用   │ │
│  │ → executionOrder 排序   │  │ → JS 侧自行排序          │ │
│  │ → 零 JS 开销           │  │ → 减少 JS↔C++ 桥接次数   │ │
│  └────────────────────────┘  └──────────────────────────┘ │
└───────────────────────────────────────────────────────────┘
```

**关键优化**:
1. **内置组件** — ThreeBucketArray 按 executionOrder 排序，C++ 虚函数直接调用，零 JS 开销
2. **用户脚本** — 统一 `ScriptComponent` 代理 (I-3)，通过 ScriptBridge 批量调用 compId
3. **统一注册** — 通过 `TypeRegistry` (I-1) 统一内置组件和脚本组件的类型信息
4. **混合节点** — 同一 Node 上的内置组件和用户脚本分别调度

### 2.2 性能对比

```
当前 (全 JS 调度):
  每帧 JS→C++ 桥接: ~200 次 (100个内置组件 × 2次/update)
  用户脚本调用: ~50 次 (25个用户组件 × 2次/update)
  总桥接次数: ~250 次/帧

优化后 (I-2 双层调度):
  内置组件: ThreeBucketArray.forEach() → C++ 虚函数, 0 次 JS 桥接
  用户脚本: ScriptBridge.invokeXxxBatch(compIds) → ~5 次批量调用
  总桥接次数: ~5 次/帧
  → 减少 98%
```

---

## 3. C++ ScriptBridge 接口

> ScriptBridge 是用户脚本系统的 C++ 侧核心类，负责：
> 1. 批量 JS 生命周期回调执行
> 2. JS 脚本组件注册到统一 TypeRegistry
> 3. 与统一 ScriptComponent (I-3) 配合完成生命周期代理

### 3.1 类设计

```cpp
// native/cocos/core/scripting/ScriptBridge.h

class ScriptBridge {
public:
    static ScriptBridge& getInstance();

    // === 初始化 ===
    void init(se::Object* globalObj);
    void shutdown();

    // === 生命周期回调 (批量调用) ===

    // 批量调用用户脚本的 start (由 ComponentScheduler::startPhase 调用)
    void invokeStartBatch(const ccstd::vector<uint32_t>& compIds);

    // 批量调用用户脚本的 update (由 ComponentScheduler::updatePhase 调用)
    void invokeUpdateBatch(const ccstd::vector<uint32_t>& compIds, float dt);

    // 批量调用用户脚本的 lateUpdate (由 ComponentScheduler::lateUpdatePhase 调用)
    void invokeLateUpdateBatch(const ccstd::vector<uint32_t>& compIds, float dt);

    // 单个调用 (用于 onEnable/onDisable/onLoad/onDestroy 等非帧循环回调)
    void invokeOnDestroy(uint32_t compId);
    void invokeOnEnable(uint32_t compId);
    void invokeOnDisable(uint32_t compId);
    void invokeOnLoad(uint32_t compId);

    // === 脚本类型注册 (运行时, 由 JS @ccclass 装饰器触发) ===

    // JS 侧注册脚本类型到统一 TypeRegistry
    // 内部调用 TypeRegistry::registerScriptType()
    uint32_t registerScriptClass(const ccstd::string& className,
                                  bool hasStart, bool hasUpdate,
                                  bool hasLateUpdate, bool hasOnLoad,
                                  bool hasOnDestroy, bool hasOnEnable,
                                  bool hasOnDisable,
                                  int32_t executionOrder,
                                  uint32_t requireComponent,
                                  bool disallowMultiple);

    // === 脚本组件实例注册 ===

    // ScriptComponent.bindScriptObject() 后调用
    // 将 JS 对象与 compId 关联
    uint32_t registerScriptInstance(se::Object* jsComp, ScriptComponent* scriptComp,
                                     const ccstd::string& className);

    // JS 侧注销脚本组件实例
    void unregisterScriptInstance(uint32_t compId);

    // === 属性同步 ===

    // 从 JS 同步属性到 C++（用于内置组件的 JS 属性设置器）
    void syncPropertyFromJS(uint32_t compId, const ccstd::string& propName,
                            const se::Value& value);

    // 从 C++ 同步属性到 JS（用于 C++ 内置组件状态变化通知）
    void syncPropertyToJS(uint32_t compId, const ccstd::string& propName,
                          const se::Value& value);

    // === Asset 引用收集 (供 ReleaseManager 使用) ===
    ccstd::vector<Asset*> collectAssetRefs(uint32_t compId);

    // === instanceof 检查 ===
    bool isInstanceOf(uint32_t compId, const ccstd::string& className);

private:
    ScriptBridge() = default;

    // 脚本组件实例注册表
    struct ScriptInstanceInfo {
        se::Object* jsObject{nullptr};       // JS 组件对象 (弱引用)
        ScriptComponent* scriptComp{nullptr}; // C++ ScriptComponent 代理
        uint32_t typeId{0};                  // TypeRegistry classId
        ccstd::string className;
    };

    ccstd::unordered_map<uint32_t, ScriptInstanceInfo> _instances;
    uint32_t _nextCompId{1};

    // 批量调用的 JS 函数缓存
    se::Object* _batchStartFn{nullptr};
    se::Object* _batchUpdateFn{nullptr};
    se::Object* _batchLateUpdateFn{nullptr};
};
```

### 3.2 JS 侧注册入口

```typescript
// cocos/scene-graph/component.jsb.ts (新增)

// 脚本类型注册: @ccclass 装饰器触发
// 调用统一 TypeRegistry::registerScriptType()
const origRegisterClass = jsb.registerClass;
jsb.registerClass = function(info) {
    // 注册到 C++ 统一 TypeRegistry
    const typeId = native.ScriptBridge.getInstance().registerScriptClass(
        info.className,       // 类名
        !!info.prototype.start,
        !!info.prototype.update,
        !!info.prototype.lateUpdate,
        !!info.prototype.onLoad,
        !!info.prototype.onDestroy,
        !!info.prototype.onEnable,
        !!info.prototype.onDisable,
        info.executionOrder || 0,
        info.requireComponent || 0,
        !!info.disallowMultiple
    );
    return typeId;
};

// 组件实例构造时自动注册到 C++ ScriptBridge
const _orig_ctor = jsb.Component.prototype._ctor;
jsb.Component.prototype._ctor = function(...args) {
    _orig_ctor.call(this, ...args);

    // 检测是否为用户脚本组件 (非内置组件)
    if (!this._isBuiltin) {
        // 统一 ScriptComponent (I-3): 在 C++ 侧已创建 ScriptComponent 代理
        // 此处绑定 JS 对象到 C++ ScriptComponent
        const compId = native.ScriptBridge.getInstance().registerScriptInstance(
            this._seObj,              // se::Object
            this._nativeScriptComp,   // C++ ScriptComponent* (已由反序列化创建)
            js.getClassName(this.constructor)
        );
        this.__compId = compId;
        this._nativeScriptComp.bindScriptObject(this._seObj);
    }
};
```

### 3.3 批量调用优化

```cpp
void ScriptBridge::invokeUpdateBatch(const ccstd::vector<uint32_t>& compIds,
                                      float dt) {
    if (compIds.empty()) return;

    EngineProfiler::beginSection("script_update_batch");

    // 构建参数数组: [[compId1, dt], [compId2, dt], ...]
    se::AutoHandleScope scope;
    auto* args = se::Object::createArrayObject(compIds.size());
    for (size_t i = 0; i < compIds.size(); ++i) {
        auto* pair = se::Object::createArrayObject(2);
        pair->setArrayElement(0, se::Value(static_cast<uint32_t>(compIds[i])));
        pair->setArrayElement(1, se::Value(dt));
        args->setArrayElement(i, se::Value(pair));
        pair->decRef();
    }

    // 一次桥接调用处理所有组件
    se::Value result;
    _batchUpdateFn->call(se::Value::Undefined, &se::Value(args), 1, &result);
    args->decRef();

    EngineProfiler::endSection();
}
```

**JS 侧的批量执行函数** (带统一错误处理, 参考 master spec §1.5 G-6):

```typescript
// cocos/core/scripting/batch-executor.ts (新增)

export function batchUpdate(items: [number, number][]): void {
    for (const [compId, dt] of items) {
        try {
            const comp = getScriptComponent(compId);
            if (comp && !comp._destroyed && comp._hasUpdate) {
                comp.update(dt);
            }
        } catch (e) {
            // G-6: JS 用户脚本异常 → try-catch 包裹 → 记录日志 → 继续调度
            cclegacy._throw(e);
        }
    }
}
```

---

## 4. 混合调度器设计 (I-2 完全重写)

> **I-2 裁决**: 合并 v1.0 的 `vector<Component*>` + `vector<uint32_t>` 分离模式
> 与 Component 框架的 `ThreeBucketArray` + `Invoker` 模式为**统一的双层三桶调度**。
> 完整定义见 `design-cpp-master-spec.md` §2.3。
> 本节从脚本系统视角描述调度器与 ScriptBridge 的协作方式。

### 4.1 ComponentScheduler 架构 (引用)

```cpp
// 完整定义: core/component/ComponentScheduler.h
// 参见 design-cpp-component-framework.md §4.3

class ComponentScheduler {
public:
    void startPhase();
    void updatePhase(float dt);
    void lateUpdatePhase(float dt);

    void scheduleBuiltin(Component* comp);
    void unscheduleBuiltin(Component* comp);

    void scheduleScript(uint32_t compId, bool hasStart, bool hasUpdate, bool hasLateUpdate);
    void unscheduleScript(uint32_t compId);

    void enableComp(Component* comp);
    void disableComp(Component* comp);

private:
    // 内置组件: 三桶排序 (保持 executionOrder 语义)
    struct BuiltinSchedule {
        ThreeBucketArray<Component> start;
        ThreeBucketArray<Component> update;
        ThreeBucketArray<Component> lateUpdate;
    } _builtin;

    // 用户脚本: 批量调用 (不区分 executionOrder, JS 侧自行排序)
    struct ScriptSchedule {
        ccstd::vector<uint32_t> start;
        ccstd::vector<uint32_t> update;
        ccstd::vector<uint32_t> lateUpdate;
    } _script;

    ccstd::vector<Component*> _deferredComps;
    bool _updating{false};
    void processDeferred();
};
```

### 4.2 帧调度执行 (脚本系统视角)

```cpp
// ====== startPhase ======

inline void ComponentScheduler::startPhase() {
    _updating = true;

    // 1. 内置组件 start (ThreeBucketArray 三桶排序遍历)
    //    按 executionOrder: neg → zero → pos
    EngineProfiler::beginSection("builtin_start");
    _builtin.start.forEach([](Component* comp) {
        CC_ASSERT(comp->isValid());
        comp->start();
    });
    _builtin.start.clear();
    EngineProfiler::endSection();

    // 2. 用户脚本 start (批量 JS 调用)
    EngineProfiler::beginSection("script_start");
    if (!_script.start.empty()) {
        ScriptBridge::getInstance().invokeStartBatch(_script.start);
        _script.start.clear();
    }
    EngineProfiler::endSection();

    _updating = false;
    processDeferred();
}

// ====== updatePhase ======

inline void ComponentScheduler::updatePhase(float dt) {
    _updating = true;

    // 1. 内置组件 update (ThreeBucketArray 遍历)
    EngineProfiler::beginSection("builtin_update");
    _builtin.update.forEach([dt](Component* comp) {
        CC_ASSERT(comp->isValid());
        comp->update(dt);
    });
    EngineProfiler::endSection();

    // 2. 用户脚本 update (ScriptBridge 批量调用)
    EngineProfiler::beginSection("script_update");
    if (!_script.update.empty()) {
        ScriptBridge::getInstance().invokeUpdateBatch(_script.update, dt);
    }
    EngineProfiler::endSection();

    _updating = false;
    processDeferred();
}

// ====== lateUpdatePhase ======

inline void ComponentScheduler::lateUpdatePhase(float dt) {
    _updating = true;

    // 1. 内置组件 lateUpdate
    EngineProfiler::beginSection("builtin_lateUpdate");
    _builtin.lateUpdate.forEach([dt](Component* comp) {
        CC_ASSERT(comp->isValid());
        comp->lateUpdate(dt);
    });
    EngineProfiler::endSection();

    // 2. 用户脚本 lateUpdate
    EngineProfiler::beginSection("script_lateUpdate");
    if (!_script.lateUpdate.empty()) {
        ScriptBridge::getInstance().invokeLateUpdateBatch(_script.lateUpdate, dt);
        _script.lateUpdate.clear();
    }
    EngineProfiler::endSection();

    _updating = false;
    processDeferred();
}
```

### 4.3 ScriptComponent 与调度器的交互

```
场景加载 → 反序列化
    ↓
创建 ScriptComponent (I-3 统一类)
    ├── TypeRegistry::create(classId) → 返回 ScriptComponent*
    ├── ScriptComponent.setScriptClassPath(path)
    ├── ScriptComponent.setSerializedProps(data, size)
    └── ScriptComponent 加入 Node._components
    ↓
节点激活 → NodeActivator::activateComp()
    ├── ScriptComponent.onLoad() → safeCallJS → ScriptBridge.invokeOnLoad()
    ├── ScriptComponent.onEnable() → safeCallJS → ScriptBridge.invokeOnEnable()
    └── 调度器注册:
        ComponentScheduler::scheduleScript(compId,
            _hasStart, _hasUpdate, _hasLateUpdate)
        ↓
        将 compId 加入 ScriptSchedule 对应的 start/update/lateUpdate 数组
    ↓
帧循环
    ├── ComponentScheduler::startPhase()
    │   └── ScriptBridge::invokeStartBatch(_script.start)
    ├── ComponentScheduler::updatePhase(dt)
    │   └── ScriptBridge::invokeUpdateBatch(_script.update, dt)
    └── ComponentScheduler::lateUpdatePhase(dt)
        └── ScriptBridge::invokeLateUpdateBatch(_script.lateUpdate, dt)
```

### 4.4 ScriptComponent 生命周期代理实现

```cpp
// ScriptComponent 通过 safeCallJS 代理所有生命周期到 JS 侧
// 参见 design-cpp-master-spec.md §2.4 和 §3.6

void ScriptComponent::update(float dt) {
    safeCallJS([dt](se::Object* jsObj) {
        // 调用 JS update
        se::Value fn;
        if (jsObj->getProperty("update", &fn) && fn.isFunction()) {
            se::Value args[1] = { se::Value(dt) };
            se::Value result;
            fn.toObject()->call(args, 1, &result);
        }
    });
}

void ScriptComponent::start() {
    safeCallJS([](se::Object* jsObj) {
        se::Value fn;
        if (jsObj->getProperty("start", &fn) && fn.isFunction()) {
            se::Value result;
            fn.toObject()->call(nullptr, 0, &result);
        }
    });
}

// onLoad/onEnable/onDisable/onDestroy 类似实现
// 使用统一的 safeCallJS 模板确保 GC 安全 (G-8)
```

---

## 5. 内置组件 C++ 化路线

> 内置组件的完整设计参见 `design-cpp-master-spec.md` §5。
> 本节从脚本系统视角补充 C++ 化模板和优先级。

### 5.1 优先级排序

| 组件 | 理由 | 复杂度 | 优先级 | Master Phase |
|------|------|--------|--------|-------------|
| Camera | 渲染核心，每帧调用 | 中 | P0 | Phase 4 (Week 17-19) |
| MeshRenderer | 渲染核心，每帧调用 | 中 | P0 | Phase 4 (Week 17-19) |
| Light (Directional/Point/Spot) | 渲染核心 | 低 | P0 | Phase 4 (Week 17-19) |
| SkeletonAnimation | 性能热点 | 高 | P1 | Phase 4 (Week 20-22) |
| RigidBody (物理) | 物理模拟热点 | 高 | P1 | Phase 4 (Week 20-22) |
| Sprite | 2D 渲染核心 | 中 | P1 | Phase 4 (Week 20-22) |
| Label | 文本渲染 | 中 | P2 | 延后 |
| AudioSource | 音频播放 | 低 | P2 | 延后 |
| ParticleSystem | 粒子模拟 | 高 | P3 | 延后 |
| UI 组件 (Widget/Layout) | UI 系统 | 中 | P3 | 延后 |

### 5.2 内置组件 C++ 化模板 (统一宏)

```cpp
// native/cocos/core/components/Camera.h
// 使用统一 CC_COMPONENT_DECLARE 和 CC_REGISTER_BUILTIN (I-1)

#include "core/component/Component.h"
#include "core/component/ComponentMacros.h"
#include "core/component/BuiltinTypeIds.h"

namespace cc {

class Camera : public Component {
    CC_COMPONENT_DECLARE(Camera, BUILTIN_CAMERA)
public:
    Camera() = default;
    ~Camera() override = default;

    // === 生命周期 ===
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;

    bool hasUpdateMethod() const override { return true; }
    void update(float dt) override;

    // === Camera 特有接口 ===
    void setPriority(int32_t priority);
    int32_t getPriority() const { return _priority; }

    void setFOV(float fov);
    float getFOV() const { return _fov; }

    // ... 其他 Camera 属性

    // === 序列化 (参见 master spec §4) ===
    void deserializeBinary(const uint8_t* data, uint32_t size) override;
    ccstd::vector<Asset*> getAssetProperties() const override;

    // === 渲染管线集成 (G-9) ===
    // onEnable → RenderScene::createCamera() + activate(true)
    // onDisable → activate(false)
    // 参见 master spec §5.2

private:
    int32_t _priority{0};
    float _fov{45.0f};
    float _near{1.0f};
    float _far{1000.0f};
    // ... 其他属性

    IntrusivePtr<scene::Camera> _renderCamera;
};

} // namespace cc

// 使用统一 TypeRegistry 注册 (I-1)
CC_REGISTER_BUILTIN(cc::Camera, "cc.Camera", cc::BUILTIN_CAMERA)
```

### 5.3 Camera 二进制反序列化

```cpp
void Camera::deserializeBinary(const uint8_t* data, uint32_t size) {
    // 固定布局 (参见 master spec §4.1 二进制格式):
    // offset 0:  priority (int32, 4B)
    // offset 4:  fov (float, 4B)
    // offset 8:  near (float, 4B)
    // offset 12: far (float, 4B)
    // offset 16: color (RGBA, 4B)
    // offset 20: clearFlags (uint32, 4B)
    // offset 24: projection (uint8, 1B)
    // offset 25: viewport (4×float, 16B)
    // total: 41 bytes

    CC_ASSERT(size >= 41);
    const float* fdata = reinterpret_cast<const float*>(data);
    _priority = *reinterpret_cast<const int32_t*>(data);
    _fov = fdata[1];
    _near = fdata[2];
    _far = fdata[3];
    _color = *reinterpret_cast<const Color*>(data + 16);
    _clearFlags = *reinterpret_cast<const uint32_t*>(data + 20);
    _projection = static_cast<Projection>(data[24]);
    memcpy(&_viewport, data + 25, 16);
}
```

---

## 6. _tempFloatArray 优化

### 6.1 当前问题

```typescript
// 每次变换更新需要多次属性设置 + 一次桥接
node._lpos.x = 100;
node._lpos.y = 200;
_tempFloatArray[0] = node._lpos.x;
_tempFloatArray[1] = node._lpos.y;
nativeFunc.updateTransform(_tempFloatArray);  // JS→C++ 桥接
```

**问题**: 每个 Node 变换更新 = 2次 JS 属性写入 + 1次桥接调用

### 6.2 优化方案：直接 C++ 属性访问

```cpp
// C++ 侧的 Node 已有 _lpos/_lrot/_lscl 属性
// 优化：JS 直接写 C++ 属性，无需 _tempFloatArray 中转

// JSB 绑定自动生成:
// node._lpos.x = 100 → 直接写 C++ Node::_lpos.x
// node._lpos.y = 200 → 直接写 C++ Node::_lpos.y
// 无需额外的桥接调用
```

### 6.3 SharedArrayBuffer 方案（进阶）

```cpp
// 将 Node 的变换数据放在共享内存区域
// JS 和 C++ 都可以直接读写

class NodeTransformShared {
public:
    // 在共享内存中分配变换数据
    static constexpr size_t TRANSFORM_SIZE = 64;  // lpos(12) + lrot(16) + lscl(12) + padding(24)

    struct alignas(16) TransformData {
        Vec3 lpos;
        Quaternion lrot;
        Vec3 lscl;
        uint32_t dirtyFlags;
    };

    // JS 侧直接作为 Float32Array 访问
    // C++ 侧直接作为 TransformData* 访问
    TransformData* data() { return reinterpret_cast<TransformData*>(_sharedMemory); }

private:
    float* _sharedMemory;  // SharedArrayBuffer
};
```

**优势**: 完全消除 _tempFloatArray 的拷贝开销，JS/C++ 零拷贝共享变换数据。

---

## 7. native-binding/decorators.ts 优化

### 7.1 当前问题

`native-binding/decorators.ts` (127KB) 在启动时执行大量猴子补丁：

```typescript
// 问题 1: Object.defineProperty 在原型上定义属性 → 阻止 V8 优化
Object.defineProperty(jsb.Sprite.prototype, 'spriteFrame', { ... });

// 问题 2: _ctor 钩子链 → 每次构造额外开销
const _orig_ctor = jsb.Sprite.prototype._ctor;
jsb.Sprite.prototype._ctor = function(...args) { ... };

// 问题 3: 127KB 代码在启动时全部执行 → 增加启动时间
```

### 7.2 优化策略

**策略 1: 将补丁代码移到 C++ 侧的 JSB 绑定中**

```cpp
// 在 JSB 绑定注册时直接定义属性
// native/cocos/bindings/auto/jsb_auto_register.cpp

bool js_register_Sprite(se::Object* obj) {
    auto* cls = se::Class::create("Sprite", obj, jsb_Component_ctor, nullptr);

    // 直接注册属性（而非 JS 侧的 Object.defineProperty）
    cls->defineProperty("spriteFrame",
        [](se::Object* thisObj, se::Value& result) -> bool {
            auto* sprite = static_cast<Sprite*>(thisObj->getNativeObject());
            result = se::Value(sprite->getSpriteFrame());
            return true;
        },
        [](se::Object* thisObj, const se::Value& value) -> bool {
            auto* sprite = static_cast<Sprite*>(thisObj->getNativeObject());
            auto* frame = static_cast<SpriteFrame*>(value.toObject()->getNativeObject());
            sprite->setSpriteFrame(frame);
            return true;
        }
    );

    cls->install();
    return true;
}
```

**策略 2: 懒加载补丁**

```typescript
// 只在首次访问时应用补丁，而非启动时全部应用
const _patches: Record<string, Function> = {
    'Sprite': () => { /* apply Sprite patches */ },
    'Camera': () => { /* apply Camera patches */ },
    // ...
};

// 首次访问时触发
function ensurePatched(className: string) {
    if (_patches[className]) {
        _patches[className]();
        delete _patches[className];
    }
}
```

**预期收益**: 启动时间减少 50-100ms（127KB → 0KB 立即执行）。

---

## 8. RequiringFrame C++ 化

### 8.1 分析

RequiringFrame 是 JS 侧的模块加载追踪机制，核心操作：
- `push(module, uuid, script, importMeta)` — 进入脚本加载
- `pop()` — 退出脚本加载
- `peek()` — 获取当前加载帧

**结论**: 此机制完全绑定 JS 模块系统，**不需要也不应该 C++ 化**。它是编辑器模式和开发模式的 JS 基础设施。

### 8.2 运行时优化

运行时模式下，可以跳过 RequiringFrame 机制：

```typescript
// 构建时将所有 @ccclass 注册为静态注册表
// 运行时启动时一次性注册，无需逐模块 push/pop

// build-output/ccclass-registry.ts (构建时生成)
import { registerClass } from 'cc';
import { PlayerController } from './scripts/PlayerController';
import { EnemyController } from './scripts/EnemyController';
// ...

export function registerAllClasses() {
    registerClass('PlayerController', PlayerController);
    registerClass('EnemyController', EnemyController);
    // ...
}
```

---

## 9. 完整架构图 (v2.0 统一版)

```
┌─────────────────────────────────────────────────────────────────┐
│                      C++ Engine Core                            │
│                                                                 │
│  ┌────────────────┐  ┌─────────────────┐  ┌────────────────┐  │
│  │ Director       │  │ TypeRegistry    │  │ Asset System   │  │
│  │ (tick/场景/    │  │ (I-1 统一注册)  │  │ (加载/释放/GC) │  │
│  │  常驻节点)     │  │                 │  │                │  │
│  └───────┬────────┘  └────────┬────────┘  └───────┬────────┘  │
│          │                    │                    │            │
│  ┌───────┴────────────────────┴────────────────────┴────────┐  │
│  │      ComponentScheduler (I-2 双层三桶调度)               │  │
│  │  ┌──────────────────────┐  ┌──────────────────────────┐ │  │
│  │  │  BuiltinSchedule     │  │  ScriptSchedule          │ │  │
│  │  │  (ThreeBucketArray)  │  │  (vector<uint32_t>)      │ │  │
│  │  │  Camera.start()      │  │  ScriptBridge            │ │  │
│  │  │  MeshRenderer.update()│  │  .invokeStartBatch()     │ │  │
│  │  │  Light.lateUpdate()  │  │  .invokeUpdateBatch()    │ │  │
│  │  │  (纯 C++ 虚函数调用)  │  │  .invokeLateUpdateBatch()│ │  │
│  │  └──────────────────────┘  └──────────────────────────┘ │  │
│  └──────────────────────────────────────────────────────────┘  │
│                              │                                  │
│  ┌───────────────────────────┴──────────────────────────────┐  │
│  │              ScriptComponent (I-3 统一占位)               │  │
│  │  C++ 代理 → safeCallJS → ScriptBridge → JS 执行         │  │
│  │  支持: 反序列化占位 + JS 绑定 + 延迟绑定                 │  │
│  └───────────────────────────┬──────────────────────────────┘  │
│                              │                                  │
│  ┌───────────────────────────┴──────────────────────────────┐  │
│  │                   JSB Bridge Layer                       │  │
│  │  se::Object / SharedArrayBuffer / GC Protocol (G-8)     │  │
│  └───────────────────────────┬──────────────────────────────┘  │
└──────────────────────────────┼──────────────────────────────────┘
                               │
┌──────────────────────────────┴──────────────────────────────────┐
│                    JS Engine (V8/JSCore)                        │
│                                                                 │
│  ┌──────────────────────┐  ┌──────────────────────────────┐   │
│  │  CCClass 类型系统    │  │  用户脚本执行                 │   │
│  │  @ccclass →          │  │  PlayerController.update()   │   │
│  │  TypeRegistry        │  │  EnemyController.start()     │   │
│  │  .registerScriptType │  │  ...                          │   │
│  └──────────────────────┘  └──────────────────────────────┘   │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  JS 侧注册                                               │  │
│  │  ScriptBridge.registerScriptClass() → TypeRegistry       │  │
│  │  ScriptBridge.registerScriptInstance() → 组件实例绑定    │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 10. 统一实施时间线 (I-5 修订)

> **I-5 裁决**: 取消本文档 v1.0 的独立 Phase 1-5 时间线，统一到 master spec §8 的 26 周时间线。

| 阶段 | 对应 Master Phase | 脚本系统工作 |
|------|------------------|------------|
| 基础设施 | Phase 0 (Week 1-3) | TypeRegistry 脚本类型注册接口 (`registerScriptType`) |
| 组件框架 | Phase 1 (Week 4-7) | ScriptBridge 核心实现 + ScriptComponent 统一占位 + ComponentScheduler 脚本调度集成 |
| 场景+序列化 | Phase 2 (Week 8-12) | ScriptComponent 反序列化占位 + JS 绑定 (延迟绑定场景) |
| 资产系统 | Phase 3 (Week 13-16) | ScriptBridge.collectAssetRefs() 集成 ReleaseManager |
| 内置组件 | Phase 4 (Week 17-22) | Camera/MeshRenderer/Light C++ 化 + ScriptBridge 桥接优化 |
| 桥接优化 | Phase 5 (Week 23-26) | _tempFloatArray 消除 + native-binding 优化 + SharedArrayBuffer + 回滚验证 + 性能基准 |

---

## 11. 风险与缓解

| 风险 | 影响 | 缓解措施 | 参考 |
|------|------|---------|------|
| ScriptBridge 批量调用失败导致整批组件不更新 | 高 | 分批执行 + 单个 try-catch + 异常隔离 | G-6 |
| 内置组件 C++ 化后 JS 侧属性访问丢失 | 高 | JSB 属性绑定全覆盖测试 | — |
| 双层调度时序与原 JS 调度不一致 | 高 | 严格保持 start→update→lateUpdate 顺序 + 执行顺序测试 | master §9 |
| 用户脚本注册/注销竞态 | 中 | 单线程模型 + 延迟到帧末处理 (`_deferredComps`) | G-5 |
| SharedArrayBuffer 浏览器兼容性 | 低 | 降级到 _tempFloatArray (仅 Web 平台) | — |
| 组件销毁后仍有未执行的脚本回调 | 高 | safeCallJS 检查 isDead() + isValid() 守卫 | G-8 |
| TypeRegistry 脚本类型 ID 与 CCClass 不一致 | 高 | registerScriptType 统一入口，classId 基于类名哈希 | I-1 |
| ScriptComponent 反序列化后 JS 绑定失败 | 高 | bindScriptObject 重试机制 + 降级到 JS 反序列化 | I-3 |

---

## 12. 统一工程约束 (G-5~G-15 补全)

> 以下内容补全 v1.0 遗漏项，完整定义见 master spec 对应章节。

### 12.1 线程模型 (G-5)

- **主线程独占**: ScriptBridge 和 ComponentScheduler 仅在主线程执行，内部数据无需锁
- **ScriptBridge._instances**: 无需 `std::mutex`，单线程访问
- **Worker 线程限制**: 不触碰 CCObject/Node/Component/ScriptComponent 树
- 详见 master spec §1.4

### 12.2 错误传播 (G-6)

| 错误源 | 传播方式 | 处理策略 |
|--------|---------|---------|
| C++ ComponentScheduler 崩溃 | `CC_ASSERT` (Debug) / 静默跳过 (Release) | 不使用 C++ 异常 |
| JS 用户脚本 update 异常 | `try-catch` 包裹 (batch-executor.ts) | 记录日志，标记组件，继续调度 |
| ScriptBridge 批量调用部分失败 | 分批 + 单个 try-catch | 单组件失败不影响整批 |
| JS 对象已 GC | `safeCallJS` 检查 `isDead()` | 静默跳过，设 `_bound = false` |
| 异步脚本加载失败 | `onComplete(error, null)` 回调 | 与现有 JS 错误回调模型一致 |

详见 master spec §1.5

### 12.3 GC 交互协议 (G-8)

```
ScriptComponent C++ 对象              JS 包装器 (se::Object)
──────────────────────              ──────────────────────

ScriptComponent 构造                (尚未创建 JS 对象)
    │
    │ ← bindScriptObject(jsObj) ──── JS 侧构造
    │                                   │
    │ _jsObject = weakRef(jsObj)        │ V8 GC 管理
    │ _bound = true                     │
    │                                   │
    │ safeCallJS(fn):                   │
    │   if !_jsObject → return          │
    │   if _jsObject->isDead() →        │
    │     _bound = false → return       │ ← GC 已回收
    │   fn(_jsObject)                   │
    │                                   │
ScriptComponent::onDestroy()        se::Object 弱引用失效
    │                                   │
    ├── safeCallJS → invokeOnDestroy    │
    ├── _jsObject = nullptr             │
    ├── _bound = false                  │
    └── destruct() 清理 C++ 资源      V8 GC 回收 se::Object
```

**关键协议**:
1. `ScriptComponent::_jsObject` 是弱引用，不阻止 GC
2. `safeCallJS()` 在每次 JS 调用前检查 `isDead()`
3. GC 回收后 `_bound` 设为 false，后续调度自动跳过
4. 详见 master spec §3.6

### 12.4 CMake 构建 (G-7)

```cmake
# native/cocos/core/CMakeLists.txt — 脚本系统新增
target_sources(cc_core PRIVATE
    scripting/ScriptBridge.cpp
    component/ScriptComponent.cpp
    component/ComponentScheduler.cpp
)
```

详见 master spec §7.1

### 12.5 Profiler 钩子 (G-12)

ScriptBridge 批量调用自动记录 Profiler 数据：

```cpp
// 已在 §4.2 帧调度执行中集成
// 每个 Phase 都包裹 EngineProfiler::beginSection/endSection
// 可追踪:
//   - builtin_start / builtin_update / builtin_lateUpdate
//   - script_start / script_update / script_lateUpdate
//   - script_update_batch (ScriptBridge 内部)

// FrameStats 中脚本相关字段:
struct FrameStats {
    // ...
    float scriptUpdateTime;       // 用户脚本 update 总时间
    uint32_t scriptCompCount;     // 脚本组件数
    uint32_t jsbCallCount;        // JSB 桥接调用次数
};
```

详见 master spec §7.3

### 12.6 回滚策略 (G-14)

| 场景 | 回滚方案 |
|------|---------|
| ScriptBridge 批量调用崩溃 | 降级到逐个调用 (`invokeUpdate` 替代 `invokeUpdateBatch`) |
| ScriptComponent 绑定失败 | 降级到全 JS 反序列化 (JSON 路径) |
| 双层调度时序异常 | 切换到全 JS 调度 (`NativeAssetManager::setMode(JS_ONLY)`) |
| TypeRegistry 脚本注册不一致 | Debug 模式校验 + 重置功能 |

**核心原则**: 每一个 C++ 化功能都必须有 JS 降级路径。详见 master spec §7.4

### 12.7 基准测试 (G-15)

```
test/benchmarks/
    ├── bench-scheduler/              ← 调度器基准
    │   ├── 100-comp-builtin.scene
    │   ├── 100-comp-script.scene
    │   ├── 1000-comp-mixed.scene     ← 混合内置+脚本
    │   └── bench.cpp
    ├── bench-script-bridge/          ← ScriptBridge 基准
    │   ├── bench-batch-invoke.cpp
    │   └── bench-single-invoke.cpp
    └── bench-results/
        ├── js-baseline.json          ← JS_ONLY 模式基准
        └── native-fast.json          ← NATIVE_FAST 模式基准
```

**关键指标**:
- 帧 JSB 桥接调用次数 (目标: < 10 次/帧)
- 脚本 update 批量调用延迟
- ScriptComponent 绑定/解绑开销

详见 master spec §7.5
