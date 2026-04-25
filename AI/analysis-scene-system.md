# Cocos Creator v3.8.8 - 场景系统深度分析

> 生成日期: 2026-04-25
> 分析范围: Scene类 / 序列化反序列化 / Prefab / 场景加载管线 / Director场景管理

---

## 1. Scene 类 (`cocos/scene-graph/scene.ts`, 210行)

### 1.1 类层次

```
CCObject ← Node ← Scene
```

通过 `@ccclass('cc.Scene')` 注册。Scene **继承自 Node**，是场景图的根节点。

### 1.2 关键属性

| 属性 | 类型 | 说明 |
|------|------|------|
| `_renderScene` | `RenderScene \| null` | 渲染层场景引用 |
| `_globals` | `SceneGlobals` | 场景级渲染配置（环境光/阴影/雾效等） |
| `autoReleaseAssets` | `boolean` | 场景卸载时是否自动释放依赖资源 |
| `dependAssets` | `any` | 缓存所有依赖资源（用于自动释放） |
| `_inited` | `boolean` | 是否已初始化（_load 后为 true） |

### 1.3 构造函数

```typescript
constructor(name) {
    super(name);
    this._activeInHierarchy = false;  // Scene 初始不激活
    if (director && director.root) {
        this._renderScene = director.root.createScene({});  // 创建渲染场景
    }
    this._inited = game ? !game._isCloning : true;
}
```

**关键**: Scene 在构造时立即创建 `RenderScene`，但 `_activeInHierarchy` 初始为 false。

### 1.4 特殊限制

- **禁止 addComponent**: `addComponent()` 直接抛出错误 "Should not add component to scene"
- **禁止 _instantiate**: `_instantiate()` 返回 null，场景不能被克隆
- **_onHierarchyChanged / _onPostActivated**: 空实现，场景不参与层级变换

### 1.5 _load() — 场景初始化

```typescript
public _load(): void {
    if (!this._inited) {
        expandNestedPrefabInstanceNode(this);    // 展开嵌套 Prefab 实例
        applyTargetOverrides(this);              // 应用 Prefab 目标覆盖
        this._onBatchCreated(prefabSyncedInLiveReload);  // 批量初始化子节点
        this._inited = true;
    }
    this.walk(Node._setScene);  // 遍历所有子节点，设置 scene 引用
}
```

### 1.6 _activate() — 场景激活

```typescript
public _activate(active = true): void {
    // [编辑器] 注册所有节点
    director._nodeActivator.activateNode(this, active);
    // 激活全局渲染配置
    this._globals.activate(this);
}
```

### 1.7 destroy() — 场景销毁

```typescript
public destroy(): boolean {
    const success = CCObject.prototype.destroy.call(this);
    if (success) {
        // 反激活所有子节点
        for (const child of this._children) {
            child.active = false;
        }
    }
    // 销毁渲染场景
    if (this._renderScene) director.root.destroyScene(this._renderScene);
    this._active = false;
    this._activeInHierarchy = false;
    return success;
}
```

---

## 2. 序列化与反序列化

### 2.1 序列化格式 (`cocos/serialization/deserialize.ts`)

引擎使用自定义二进制压缩 JSON 格式，核心类型 ID：

| DataTypeID | 值 | 说明 |
|------------|-----|------|
| SimpleType | 0 | 原始值（null/number/string/boolean/plain object） |
| InstanceRef | 1 | 引用已解析实例 |
| Array_InstanceRef | 2 | 实例引用数组 |
| Array_AssetRefByInnerObj | 3 | 资源引用数组（内嵌对象） |
| Class | 4 | 嵌入对象 |
| ValueTypeCreated | 5 | ValueType（通过构造函数创建） |
| AssetRefByInnerObj | 6 | 资源引用（内嵌对象） |
| TRS | 7 | Node 专用 TypedArray（变换数据） |
| ValueType | 8 | 无默认值的 ValueType |
| Array_Class | 9 | 类对象数组 |
| CustomizedClass | 10 | 自定义类嵌入 |
| Dict | 11 | 通用字典 |
| Array | 12 | 通用数组 |

### 2.2 反序列化流程

```
JSON 二进制数据
    ↓
deserializeDynamic(data, options)
    ↓
[解析阶段]
    ├── 读取共享字符串表
    ├── 读取实例类型信息
    ├── 创建所有实例（new constructor()）
    └── 建立 uuid → 实例映射
    ↓
[赋值阶段]
    ├── 遍历每个实例的属性
    ├── 根据 DataTypeID 解码值
    │   ├── SimpleType → 直接赋值
    │   ├── InstanceRef → 查找已解析实例
    │   ├── Class → 递归创建嵌入对象
    │   ├── AssetRefByInnerObj → 延迟解析资源引用
    │   └── TRS → 解码 Node 变换数据
    └── 设置属性值
    ↓
[引用解析阶段]
    ├── 解析跨实例引用
    └── 解析资源 UUID 引用
```

### 2.3 _deserialize() 方法

CCObject 的 `_deserialize` 是动态注入的（`prototype._deserialize = null`），由子类覆写：

- **Node._deserialize**: 解析 ltc/layer/变换数据/子节点/组件
- **Component._deserialize**: 解析属性值、回调引用
- **Asset._deserialize**: 解析 native 属性、依赖资源

---

## 3. Prefab 系统 (`cocos/scene-graph/prefab/`, 4个文件)

### 3.1 Prefab 类 (`prefab.ts`, 231行)

```
CCObject ← Asset ← Prefab
```

| 属性 | 类型 | 说明 |
|------|------|------|
| `data` | `any` | Prefab 的根节点数据（Node 实例） |
| `optimizationPolicy` | `OptimizationPolicy` | 实例化优化策略 |
| `persistent` | `boolean` | 是否持久化 |
| `_createFunction` | `Function \| null` | JIT 编译的创建函数 |
| `_instantiatedTimes` | `number` | 已实例化次数 |

### 3.2 优化策略

```typescript
const OptimizationPolicy = Enum({
    AUTO: 0,            // 自动选择（默认）
    SINGLE_INSTANCE: 1, // 单次实例优化，跳过 JIT
    MULTI_INSTANCE: 2,  // 多次实例优化，启用 JIT
});
```

**自动策略逻辑**:
- 首次实例化 → 使用普通路径
- 实例化次数 ≥ `OptimizationPolicyThreshold`（默认3）→ 启用 JIT 编译

### 3.3 JIT 实例化编译

```typescript
public compileCreateFunction(): void {
    if (SUPPORT_JIT) {
        this._createFunction = compile(this.data);
    }
}
```

`compile()` 来自 `serialization/instantiate-jit.ts`，它将 Prefab 的节点树数据编译为 JavaScript 函数：

```javascript
// 编译结果示例（简化）
function createPrefab(rootToRedirect) {
    var n1 = new cc.Node();
    n1._lpos.x = 100; n1._lpos.y = 200;
    var c1 = new cc.Sprite();
    c1.spriteFrame = ...; // 资源引用
    n1._components.push(c1);
    c1.node = n1;
    // ... 递归子节点
    return n1;
}
```

**性能优势**: 避免每次实例化时的反射/序列化开销，直接硬编码属性赋值。

### 3.4 _instantiate() 流程

```
Prefab._instantiate()
    ↓
[选择策略]
    ├── SINGLE_INSTANCE → this.data._instantiate()        ← 普通路径
    ├── MULTI_INSTANCE → this._doInstantiate()            ← JIT 路径
    └── AUTO → 根据 _instantiatedTimes 自动选择
    ↓
[JIT 路径]
    _doInstantiate()
        ├── [首次] compileCreateFunction()
        └── _createFunction(rootToRedirect)
            ↓ 返回节点（未初始化）
        └── this.data._instantiate(node)                  ← 初始化节点
    ↓
++_instantiatedTimes
```

### 3.5 Prefab 加载处理

```typescript
public onLoaded(): void {
    const rootNode = this.data as Node;
    utils.expandNestedPrefabInstanceNode(rootNode);  // 展开嵌套 Prefab
    utils.applyTargetOverrides(rootNode);             // 应用目标覆盖
    if (JSB) {
        updateChildrenForDeserialize(rootNode);       // JSB 子节点修复
    }
}
```

### 3.6 PrefabInfo (`prefab-info.ts`, 248行)

每个 Prefab 实例节点都有 `_prefab: PrefabInfo` 属性：

```typescript
class PrefabInfo {
    asset: Prefab | null;        // 关联的 Prefab 资源
    fileId: string;              // 文件内唯一 ID
    infoId: number;              // 同步信息 ID
    root: Node | null;           // Prefab 实例根节点
    isDeleted: boolean;          // 是否已删除
    // ... 编辑器同步相关
}
```

### 3.7 Prefab 工具函数 (`utils.ts`, 509行)

- **expandNestedPrefabInstanceNode**: 展开嵌套 Prefab 实例节点（递归处理嵌套 Prefab 引用）
- **applyTargetOverrides**: 应用 Prefab 实例上的属性覆盖（修改 Prefab 默认值）

---

## 4. 场景加载管线

### 4.1 Director.loadScene() — 入口

```typescript
public loadScene(sceneName, onLaunched, onUnloaded): boolean {
    // 检查是否已在加载
    if (this._loadingScene) return false;
    
    // 查找包含该场景的 Bundle
    const bundle = assetManager.bundles.find(b => !!b.getSceneInfo(sceneName));
    
    this._loadingScene = sceneName;
    
    // 通过 Bundle 异步加载场景
    bundle.loadScene(sceneName, (err, scene) => {
        this._loadingScene = '';
        if (!err) {
            this.runSceneImmediate(scene, onUnloaded, onLaunched);
        }
    });
}
```

### 4.2 Director.runScene() — 延迟运行

```typescript
public runScene(scene, onBeforeLoadScene, onLaunched): void {
    // 延迟到帧末执行
    this.once(DirectorEvent.END_FRAME, () => {
        this.runSceneImmediate(scene, onBeforeLoadScene, onLaunched);
    });
}
```

### 4.3 Director.runSceneImmediate() — 核心流程

```
runSceneImmediate(scene, onBeforeLoadScene, onLaunched)
    ↓
[SceneAsset → Scene] 转换
    ↓
scene._load()
    ├── expandNestedPrefabInstanceNode()
    ├── applyTargetOverrides()
    ├── _onBatchCreated()
    └── walk(Node._setScene)
    ↓
[处理常驻节点]
    ├── 遍历 _persistRootNodes
    ├── 场景中存在同 UUID 节点 → 替换旧节点
    └── 不存在 → 设为场景子节点
    ↓
[销毁旧场景]
    ├── 保留常驻节点的子树
    ├── 旧场景的 destroy() 调用
    └── 清理全局状态
    ↓
[设置新场景]
    ├── this._scene = scene
    └── scene._activate(true)
        ├── NodeActivator.activateNode(scene, true)
        │   └── 递归激活所有子节点和组件
        └── _globals.activate(scene)
    ↓
emit(DirectorEvent.AFTER_SCENE_LAUNCH)
```

### 4.4 完整场景加载时序

```
用户调用: director.loadScene("MainScene")
    ↓
1. 查找 Bundle → 找到 SceneAsset 配置
    ↓
2. Bundle.loadScene() → 异步加载
    ├── 下载 .scene JSON 文件
    ├── 反序列化为 SceneAsset 对象
    │   ├── 创建 Scene 实例
    │   ├── 递归创建 Node 树
    │   ├── 创建所有 Component
    │   └── 解析资源引用（延迟加载）
    └── 缓存 SceneAsset
    ↓
3. runSceneImmediate(scene)
    ├── scene._load()
    │   ├── 展开嵌套 Prefab
    │   ├── 应用属性覆盖
    │   └── walk 设置 scene 引用
    ├── 处理常驻节点
    ├── 销毁旧场景
    └── scene._activate(true)
        ├── NodeActivator 递归激活
        │   ├── __preload()
        │   ├── onLoad()
        │   └── onEnable()
        └── 全局渲染配置激活
```

---

## 5. Director 场景管理 (`cocos/game/director.ts`, 35.5KB)

### 5.1 核心职责

- 场景生命周期管理（加载/运行/卸载）
- 游戏主循环驱动
- 常驻节点管理
- 组件调度器管理
- 系统调度

### 5.2 常驻节点 (Persist Root Node)

```typescript
// 设置常驻节点
public addPersistRootNode(node: Node): void {
    const uuid = node.uuid;
    if (this._persistRootNodes[uuid]) return;
    
    node._originalSceneId = node.scene.uuid;  // 记录原始场景 ID
    node._objFlags |= CCObjectFlags.DontDestroy;
    this._persistRootNodes[uuid] = node;
}

// 取消常驻节点
public removePersistRootNode(node: Node): void {
    node._objFlags &= ~CCObjectFlags.DontDestroy;
    delete this._persistRootNodes[node.uuid];
}
```

**场景切换时的处理**:
1. 遍历所有常驻节点
2. 如果新场景中有同 UUID 节点 → 用常驻节点替换
3. 否则 → 将常驻节点设为新场景的子节点
4. 设置 `DontSave` 标志（避免序列化）

### 5.3 场景事件

| 事件 | 触发时机 |
|------|---------|
| BEFORE_SCENE_LOADING | loadScene 开始时 |
| BEFORE_SCENE_LAUNCH | runSceneImmediate 场景激活前 |
| AFTER_SCENE_LAUNCH | 场景激活完成后 |

---

## 6. C++ 化设计关键考量

### 6.1 可 Native 化的部分

| 子系统 | 可行性 | 说明 |
|--------|--------|------|
| Scene 核心结构 | ✅ 高 | 简单数据容器 |
| RenderScene 管理 | ✅ 高 | 已有 C++ 实现 |
| SceneGlobals | ✅ 高 | 渲染配置，已部分 C++ 化 |
| Director 场景切换逻辑 | ✅ 高 | 纯逻辑流程 |
| 常驻节点管理 | ✅ 高 | UUID 索引 + 标志位 |

### 6.2 需要 JS 桥接的部分

| 子系统 | 原因 |
|--------|------|
| 场景反序列化 | 依赖 CCClass 反射、getClassByName |
| Prefab JIT 编译 | 生成 JS 代码 |
| Node 组件创建 | new constructor() 依赖 JS 类型系统 |
| expandNestedPrefabInstanceNode | 依赖 JS 对象引用 |

### 6.3 反序列化 C++ 化策略

**建议方案**: 双格式序列化

```
[编辑器导出] .scene JSON → [构建时转换] → 二进制格式 (.scene.bin)
                                              ↓
                              C++ 直接读取二进制格式
                              （无需 JS 反射）
```

- **编辑器模式**: 继续使用 JSON + JS 反序列化
- **运行时模式**: 使用预编译的二进制格式，C++ 侧直接解析
- **二进制格式设计**: 类型 ID + 属性偏移表 + 数据块，无需类名查找

### 6.4 Prefab C++ 化策略

**当前 JIT 编译方案无法在 C++ 复用**，建议：

1. **Prefab 数据预编译**: 构建时将 Prefab 节点树转换为 C++ 可读的二进制描述
2. **C++ 实例化器**: 根据二进制描述创建 Node/Component 实例
3. **用户脚本组件**: C++ 创建 Node + 占位 Component，JS 侧绑定实际脚本
4. **内置组件**: 完全 C++ 化，无需 JS 参与
