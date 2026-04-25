# C++ Scene 系统设计

> 版本: v2.0 (统一修订版)
> 日期: 2026-04-25
> 基于: AI/analysis-scene-system.md
> 目标: 将 Cocos Creator v3.8.8 的场景系统关键路径 C++ 化
> 总体规范: AI/design-cpp-master-spec.md v2.0
> 修订项: I-1 (TypeRegistry合并), I-3 (ScriptComponent统一), I-4 (常驻节点归Director)

---

## 1. 现有 C++ 实现现状

### 1.1 已实现

| 组件 | 文件 | 状态 |
|------|------|------|
| Scene | `native/cocos/core/scene-graph/Scene.h/.cpp` | ✅ 基本完成，load/activate/destroy |
| SceneGlobals | `native/cocos/core/scene-graph/SceneGlobals.h/.cpp` | ✅ 完成，渲染配置全 C++ 化 |
| SceneAsset | `native/cocos/core/assets/SceneAsset.h/.cpp` | ✅ 完成，持有 Scene 引用 |
| RenderScene | `native/cocos/scene/RenderScene.h/.cpp` | ✅ 完成 |
| Node::instantiate | `native/cocos/core/scene-graph/Node.h` | ✅ 已有静态方法 |
| CCObject Flags | `native/cocos/core/data/Object.h` | ✅ 完整标志位系统 |

### 1.2 TODO/注释掉（C++ 侧未完成）

| 功能 | 位置 | 注释 |
|------|------|------|
| expandNestedPrefabInstanceNode | `Scene.cpp:56` | `// TODO(xwx): not implement yet` |
| applyTargetOverrides | `Scene.cpp:57` | `// TODO(xwx): not implement yet` |
| _onBatchCreated | `Scene.cpp:58` | `// Moved from Node, only emits event to JS now` |
| NodeActivator | `Scene.cpp:72` | `// Director::getInstance()->getNodeActivator()` 注释掉 |
| Director | `Scene.cpp:27` | `// #include "core/Director.h"` 注释掉 |
| Prefab | 整个 native 目录 | 无 Prefab.h/.cpp |
| 序列化/反序列化 | 整个 native 目录 | 无 deserializer 实现 |

### 1.3 关键结论

**Scene 核心结构已 C++ 化**，但以下关键路径仍依赖 JS：
1. **反序列化**: 完全由 JS 执行，C++ 侧只有结果
2. **Prefab 系统**: 无 C++ 实现，JIT 编译是纯 JS 机制
3. **场景激活链**: NodeActivator 是 JS 实现，C++ 只调 globals.activate
4. **常驻节点管理**: Director 在 JS 侧，C++ 无对应实现

---

## 2. C++ 化设计：分层策略

### 2.1 设计原则

```
┌──────────────────────────────────────────────────┐
│                 JS 编辑器模式                      │
│   JSON反序列化 + JS反射 + JIT编译 + 热重载         │
├──────────────────────────────────────────────────┤
│                 运行时模式                         │
│   二进制反序列化 + C++反射 + 预编译实例化 + 零JS   │
└──────────────────────────────────────────────────┘
```

**核心策略**: 双轨制——编辑器保持 JS 路径，运行时走 C++ 快速路径。

### 2.2 各子系统 C++ 化方案

---

## 3. Scene 核心 — 完善 C++ 实现

### 3.1 当前缺失 → 补充

> **I-4 裁决**: Scene.h **不增加** `_persistRootNodes`。常驻节点**仅由 Director 管理**。
> 理由：常驻节点跨场景存活，逻辑上属于 Director 而非任何 Scene。

```cpp
// native/cocos/core/scene-graph/Scene.h — 增补 (无常驻节点)
class Scene final : public Node {
public:
    // ... 已有接口 ...

    // === 新增 (仅 Scene 级功能) ===
    inline bool isAutoReleaseAssets() const { return _autoReleaseAssets; }
    inline void setAutoReleaseAssets(bool val) { _autoReleaseAssets = val; }

    // 注意: 常驻节点管理移至 Director.h (I-4 裁决)
    // Scene 不持有 _persistRootNodes

protected:
    bool _autoReleaseAssets{false};
};
```

### 3.2 Scene::load() 完善

```cpp
void Scene::load() {
    events::SceneLoad::broadcast();
    if (!_inited) {
        expandNestedPrefabInstanceNode(this);   // ← 需实现
        applyTargetOverrides(this);             // ← 需实现
        onBatchCreated(false);
        _inited = true;
    }
    _scene = this;
    walk(Node::setScene);
}
```

### 3.3 Scene::activate() 完善

```cpp
void Scene::activate(bool active) {
    // 方案: 将 NodeActivator 核心逻辑 C++ 化
    // 参见 design-cpp-component-framework.md 的 NodeActivator 设计
    if (active) {
        activateNodeRecursive(this, true);
    } else {
        deactivateNodeRecursive(this, false);
    }
    _globals->activate(this);
    if (_renderScene) {
        _renderScene->activate();
    }
}
```

---

## 4. 二进制序列化系统

### 4.1 设计思路

当前 TS 侧的 `deserialize.ts` 使用 DataTypeID + 共享字符串表格式，解析依赖 JS 反射 (`getClassByName`)。C++ 化需要：

1. **构建时预处理**: 将 JSON 场景文件转换为 C++ 可直接读取的二进制格式
2. **类型注册表**: C++ 侧维护 `classId → constructor` 映射
3. **零反射反序列化**: 通过类型 ID 直接创建 C++ 对象

### 4.2 二进制场景格式 (.scene.bin)

```
┌─────────────────────────────────────┐
│  Header (32 bytes)                  │
│  ├── magic: "CCSC" (4B)            │
│  ├── version: uint16 (2B)          │
│  ├── flags: uint16 (2B)            │
│  ├── stringTableOffset: uint32 (4B) │
│  ├── instanceTableOffset: uint32(4B)│
│  ├── rootNodeOffset: uint32 (4B)    │
│  └── reserved (12B)                │
├─────────────────────────────────────┤
│  String Table                       │
│  ├── count: uint32                  │
│  ├── offsets[count]: uint32[]       │
│  └── utf8 strings (null-terminated)│
├─────────────────────────────────────┤
│  Instance Table                     │
│  ├── count: uint32                  │
│  └── InstanceEntry[count]:          │
│      ├── classId: uint32            │  ← C++ 类型注册 ID
│      ├── uuidOffset: uint32         │  ← 字符串表索引
│      ├── dataOffset: uint32         │  ← 属性数据偏移
│      └── dataSize: uint32           │
├─────────────────────────────────────┤
│  Node Tree                          │
│  └── NodeEntry (递归):              │
│      ├── instanceIdx: uint32        │  ← 实例表索引
│      ├── childCount: uint32         │
│      ├── lrot: Quaternion (16B)     │
│      ├── lpos: Vec3 (12B)          │
│      ├── lscl: Vec3 (12B)          │
│      ├── layer: uint32              │
│      ├── active: uint8              │
│      ├── nameOffset: uint32         │
│      ├── componentCount: uint32     │
│      ├── componentIdx[componentCount]: uint32[]
│      └── children[childCount]: NodeEntry[]
├─────────────────────────────────────┤
│  Component Data                     │
│  └── ComponentEntry[]:              │
│      ├── classId: uint32            │
│      ├── instanceIdx: uint32        │
│      ├── propertyDataSize: uint32   │
│      └── propertyData[]:            │  ← 类型特化的属性二进制
│          ├── BuiltinComponent:      │  ← C++ 直接读取
│          │   固定布局，零反射
│          └── UserScriptComponent:   │  ← 延迟到 JS 侧
│              仅存 classPath + JSON
└─────────────────────────────────────┤
│  Asset Reference Table              │
│  ├── count: uint32                  │
│  └── AssetRef[count]:               │
│      ├── uuidOffset: uint32         │
│      ├── assetType: uint16          │
│      └── ownerInstanceIdx: uint32   │
└─────────────────────────────────────┘
```

### 4.3 C++ 类型注册表 (I-1 修订)

> **I-1 裁决**: 本文 v1.0 的 `TypeRegistry` 已与 Component 框架的 `ComponentTypeRegistry` 合并为**统一 TypeRegistry**。
> 完整定义见 `design-cpp-master-spec.md` §2.1。

```cpp
// 完整定义: native/cocos/core/serialization/TypeRegistry.h
// 参见 design-cpp-master-spec.md §2.1

// 使用方式:
// 1. 内置组件: CC_REGISTER_BUILTIN() 宏在静态初始化时注册
// 2. 用户脚本: TypeRegistry::registerScriptType() 在运行时注册
// 3. 序列化: TypeRegistry::create(classId) + deserialize()
// 4. 查询: TypeRegistry::getTypeInfo(classId) → TypeInfo*
```

### 4.4 内置组件二进制反序列化

```cpp
// 每个 C++ 组件实现 deserializeBinary()
class Transform : public Component {
public:
    void deserializeBinary(const uint8_t* data, uint32_t size) override {
        // 固定布局：直接 memcpy
        // lpos(12B) + lrot(16B) + lscl(12B) + layer(4B) = 44 bytes
        CC_ASSERT(size >= 44);
        memcpy(&_lpos, data, 12);
        memcpy(&_lrot, data + 12, 16);
        memcpy(&_lscl, data + 28, 12);
        memcpy(&_layer, data + 40, 4);
    }
};
```

### 4.5 二进制场景反序列化器

```cpp
// native/cocos/core/serialization/BinaryDeserializer.h

class BinaryDeserializer {
public:
    struct Result {
        IntrusivePtr<Scene> scene;
        ccstd::vector<IntrusivePtr<Asset>> assets;
    };

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

### 4.6 构建时转换器

```
编辑器导出 .scene (JSON)
         ↓
  build-scene-binary.js (构建脚本)
         ↓
  .scene.bin (二进制格式)
         ↓
  打包到 Bundle 中
```

构建脚本负责：
1. 解析 JSON 场景文件
2. 为每个类分配 `classId`（基于 `@ccclass` 名称的哈希）
3. 收集所有字符串 → 字符串表
4. 将节点树递归编码为二进制
5. 内置组件属性直接二进制化
6. 用户脚本组件保留为 JSON 子段
7. 输出 `.scene.bin`

---

## 5. Prefab C++ 化

### 5.1 设计思路

TS 的 JIT 编译方案（`compile()` → JS Function）无法在 C++ 复用。替代方案：

**Prefab 二进制预编译** — 构建时将 Prefab 节点树转换为二进制模板描述。

### 5.2 C++ Prefab 类

```cpp
// native/cocos/core/scene-graph/Prefab.h

class Prefab : public Asset {
public:
    using Super = Asset;

    enum class OptimizationPolicy : uint8_t {
        AUTO = 0,
        SINGLE_INSTANCE = 1,
        MULTI_INSTANCE = 2,
    };

    ~Prefab() override;

    // 从二进制数据初始化
    bool initFromBinary(const uint8_t* data, uint32_t size);

    // 实例化
    Node* instantiate();

    // 优化策略
    void setOptimizationPolicy(OptimizationPolicy policy);
    OptimizationPolicy getOptimizationPolicy() const { return _optimizationPolicy; }

    // 数据根节点模板（编辑器模式用）
    Node* getData() const { return _data.get(); }
    void setData(Node* data);

private:
    IntrusivePtr<Node> _data;          // 编辑器模式的 JS 实例化入口
    OptimizationPolicy _optimizationPolicy{OptimizationPolicy::AUTO};
    uint32_t _instantiatedTimes{0};

    // 运行时二进制模板
    struct BinaryTemplate {
        ccstd::vector<uint8_t> data;
        // 预计算的对象池大小
        uint32_t nodeCount{0};
        uint32_t componentCount{0};
    };
    BinaryTemplate _binaryTemplate;

    // C++ 快速实例化
    Node* instantiateFromBinary();
};
```

### 5.3 Prefab 实例化流程

```
Prefab::instantiate()
    ↓
[选择策略]
    ├── SINGLE_INSTANCE → instantiateFromBinary()
    │   直接从二进制模板创建
    ├── MULTI_INSTANCE → instantiateFromBinary() + 对象池
    │   预分配对象，复用内存
    └── AUTO → 根据 _instantiatedTimes 自动切换
    ↓
instantiateFromBinary()
    ├── 预分配 nodeCount 个 Node
    ├── 预分配 componentCount 个 Component
    ├── 顺序遍历二进制模板
    │   ├── 读取 classId → TypeRegistry::create()
    │   ├── 读取属性数据 → deserializeBinary()
    │   └── 建立父子/组件关系
    ├── resolveReferences() — 解析资源 UUID 引用
    └── 返回根 Node
    ↓
++_instantiatedTimes
```

### 5.4 PrefabInfo C++ 化

```cpp
// native/cocos/core/scene-graph/PrefabInfo.h

struct PrefabInfo {
    IntrusivePtr<Prefab> asset;         // 关联的 Prefab 资源
    ccstd::string fileId;               // 文件内唯一 ID
    uint32_t infoId{0};                 // 同步信息 ID
    Node* root{nullptr};                // Prefab 实例根节点
    bool isDeleted{false};
    // 编辑器同步字段省略...
};
```

### 5.5 expandNestedPrefabInstanceNode C++ 实现

```cpp
// native/cocos/core/scene-graph/PrefabUtils.h

namespace PrefabUtils {
    // 展开嵌套 Prefab 实例节点
    void expandNestedPrefabInstanceNode(Node* root);

    // 应用目标覆盖
    void applyTargetOverrides(Node* root);

    // 更新子节点的反序列化后修复（JSB 专用）
    void updateChildrenForDeserialize(Node* root);
}
```

---

## 6. Director C++ 化

### 6.1 C++ Director 类 (G-1 补全)

> 完整定义见 master spec §3.1。此处补充主循环 tick 和子系统访问。

```cpp
// native/cocos/core/Director.h

class Director : public RefCounted {
public:
    static Director* getInstance();

    // === 主循环 (G-1 补全) ===
    void tick(float dt);

    // === 场景管理 ===
    bool loadScene(const ccstd::string& sceneName,
                   const std::function<void(Scene*)>& onLaunched = nullptr,
                   const std::function<void(Scene*)>& onUnloaded = nullptr);
    void runScene(Scene* scene,
                  const std::function<void()>& onBeforeLoadScene = nullptr,
                  const std::function<void()>& onLaunched = nullptr);
    void runSceneImmediate(Scene* scene,
                           const std::function<void()>& onBeforeLoadScene = nullptr,
                           const std::function<void()>& onLaunched = nullptr);

    // === 常驻节点 (I-4: 仅 Director 管理) ===
    void addPersistRootNode(Node* node);
    void removePersistRootNode(Node* node);
    bool isPersistRootNode(Node* node) const;

    // === 当前场景 ===
    Scene* getScene() const { return _scene.get(); }
    Scene* getLoadingScene() const;

    // === 子系统访问 ===
    NodeActivator* getNodeActivator() const { return _nodeActivator.get(); }
    ComponentScheduler* getCompScheduler() const { return _compScheduler.get(); }
    Scheduler* getScheduler() const { return _scheduler.get(); }

private:
    IntrusivePtr<Scene> _scene;
    ccstd::string _loadingScene;
    ccstd::unordered_map<ccstd::string, IntrusivePtr<Node>> _persistRootNodes;
    IntrusivePtr<NodeActivator> _nodeActivator;
    IntrusivePtr<ComponentScheduler> _compScheduler;
    IntrusivePtr<Scheduler> _scheduler;

    void handlePersistRootNodes(Scene* newScene);
    void destroyOldScene();
};
```

### 6.2 runSceneImmediate 核心流程

```cpp
void Director::runSceneImmediate(Scene* scene,
                                  const std::function<void()>& onBeforeLoadScene,
                                  const std::function<void()>& onLaunched) {
    // 1. 前置回调
    if (onBeforeLoadScene) onBeforeLoadScene();

    // 2. SceneAsset → Scene 转换（如果传入的是 SceneAsset）
    // ... SceneAsset::getScene() ...

    // 3. 场景加载
    scene->load();

    // 4. 处理常驻节点
    handlePersistRootNodes(scene);

    // 5. 销毁旧场景
    destroyOldScene();

    // 6. 设置新场景
    _scene = scene;

    // 7. 激活新场景
    scene->activate(true);

    // 8. 触发事件
    events::AfterSceneLaunch::broadcast();

    // 9. 完成回调
    if (onLaunched) onLaunched();
}
```

### 6.3 常驻节点处理

```cpp
void Director::handlePersistRootNodes(Scene* newScene) {
    for (auto& [uuid, persistNode] : _persistRootNodes) {
        // 查找新场景中是否有同 UUID 节点
        Node* existingNode = findNodeByUUID(newScene, uuid);
        if (existingNode) {
            // 替换：将常驻节点插入到 existingNode 的位置
            replaceNodeInParent(existingNode, persistNode);
            existingNode->destroy();
        } else {
            // 将常驻节点设为新场景的子节点
            newScene->addChild(persistNode);
        }
        persistNode->_objFlags |= CCObject::Flags::DONT_SAVE;
    }
}
```

---

## 7. JS ↔ C++ 桥接策略

### 7.1 双模式运行

```
                    ┌──────────────────┐
                    │   场景加载请求     │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │ 是否有 .scene.bin?│
                    └────┬───────┬─────┘
                    Yes  │       │ No
                    ┌────▼──┐  ┌─▼──────────┐
                    │C++ 路径│  │JS 路径      │
                    │       │  │(现有流程)   │
                    │Binary │  │JSON反序列化 │
                    │Deser. │  │+JS反射     │
                    └───┬───┘  └─────┬──────┘
                        │            │
                        └──────┬─────┘
                               │
                    ┌──────────▼──────────┐
                    │ Scene C++ 对象已就绪  │
                    │ (JS 侧有 se::Object  │
                    │  包装器)             │
                    └─────────────────────┘
```

### 7.2 用户脚本组件处理 (I-3 修订)

> **I-3 裁决**: 原 `ScriptComponentPlaceholder` 已合并为统一的 `ScriptComponent`。
> 完整定义见 `design-cpp-master-spec.md` §2.4 和 `design-cpp-component-framework.md` §2.3。

C++ 反序列化遇到用户脚本时：

```cpp
// BinaryDeserializer 内部
if (TypeRegistry::getInstance().hasType(classId)) {
    // 内置组件 → C++ 直接创建
    component = TypeRegistry::getInstance().create(classId);
    TypeRegistry::getInstance().deserialize(classId, component, propData, propSize);
} else {
    // 用户脚本 → 创建统一 ScriptComponent 占位，延迟到 JS 侧绑定
    auto* scriptComp = ccnew ScriptComponent();
    scriptComp->setScriptClassPath(getString(classPathOffset));
    scriptComp->setSerializedProps(propData, propSize);  // 保留原始 JSON
    component = scriptComp;
}
```

### 7.3 ScriptComponent (统一版)

> 不再单独定义 ScriptComponentPlaceholder，统一使用 `ScriptComponent`。
> 该类同时承担：(1) JS 生命周期代理 (2) 反序列化占位 (3) 延迟绑定。
> 参见 `design-cpp-component-framework.md` §2.3 的完整定义。

---

## 8. 性能对比预估

### 8.1 场景加载时间（典型 1000 节点场景）

| 阶段 | JS 路径 | C++ 二进制路径 | 加速比 |
|------|---------|---------------|--------|
| JSON 解析 | 8ms | 0ms (跳过) | ∞ |
| 实例创建 | 15ms | 3ms (预分配+memcpy) | 5× |
| 属性赋值 | 20ms | 4ms (二进制直读) | 5× |
| 引用解析 | 5ms | 2ms (偏移表查找) | 2.5× |
| 激活遍历 | 10ms | 6ms (C++ 递归) | 1.7× |
| **总计** | **58ms** | **15ms** | **~4×** |

### 8.2 Prefab 实例化（100 节点 Prefab × 10 次）

| 指标 | JS JIT 路径 | C++ 二进制路径 |
|------|------------|---------------|
| 首次实例化 | 12ms (含编译) | 4ms |
| 后续实例化 | 6ms (编译缓存) | 3ms (对象池) |
| 内存分配 | 48KB/次 | 32KB/次 (预分配) |

---

## 9. 实施路线图 (I-5 修订)

> **I-5 裁决**: 取消本文档 v1.0 的独立 Phase 1-4 时间线，统一到 master spec §8 的 26 周时间线。

| 阶段 | 对应 Master Phase | 场景系统工作 |
|------|------------------|------------|
| 场景核心 | Phase 1 (Week 4-7) | Scene::load/activate 完善 + Director C++ + 常驻节点 |
| 序列化 | Phase 2 (Week 8-12) | 二进制格式 v1 + BinaryDeserializer + 构建脚本 + 版本迁移 (G-4) |
| Prefab | Phase 2 (Week 8-12) | Prefab C++ 类 + 二进制模板 + expandNested/applyOverrides |
| 集成 | Phase 5 (Week 23-26) | 全面回归测试 + 性能基准 + 内存泄漏检测 |

---

## 10. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 二进制格式与 JSON 格式差异导致编辑器不兼容 | 高 | 编辑器始终走 JS 路径 (INV-2) |
| 用户脚本属性序列化格式不稳定 | 中 | ScriptComponent 保留原始 JSON (I-3) |
| C++ 反射系统与 TS 装饰器不一致 | 中 | TypeRegistry 的 classId 基于 @ccclass 名称哈希 (I-1) |
| 嵌套 Prefab 在 C++ 侧展开逻辑复杂 | 高 | 先用 JS 展开 + C++ 创建混合模式 |
| 常驻节点跨场景替换时的引用断裂 | 高 | 保持 UUID 一致 + 引用延迟解析 (I-4) |
| 二进制格式版本迁移失败 | 高 | 构建脚本检测 version → 应用迁移函数 (G-4, master spec §4.4) |
