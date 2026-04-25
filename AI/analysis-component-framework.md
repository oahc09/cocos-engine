# Cocos Creator v3.8.8 - 组件框架深度分析

> 生成日期: 2026-04-25
> 分析范围: CCObject / Component / Node组件系统 / 类注册反射 / 调度器

---

## 1. CCObject 基类 (`cocos/core/data/object.ts`, 683行)

### 1.1 类定义

```typescript
class CCObject implements EditorExtendableObject {
    _objFlags: number = 0;        // 状态位掩码
    protected _name: string;      // 对象名称
    [editorExtrasTag]: unknown;   // 编辑器扩展数据（仅 EDITOR）
}
```

### 1.2 CCObjectFlags 标志位系统

| 标志 | 值 | 用途 |
|------|-----|------|
| Destroyed | 1<<0 | 已销毁 |
| RealDestroyed | 1<<1 | 真正销毁（编辑器用） |
| ToDestroy | 1<<2 | 等待销毁 |
| DontSave | 1<<3 | 不序列化 |
| EditorOnly | 1<<4 | 仅编辑器 |
| Dirty | 1<<5 | 脏标记 |
| DontDestroy | 1<<6 | 场景切换不销毁 |
| Destroying | 1<<7 | 正在销毁中 |
| Deactivating | 1<<8 | 正在反激活 |
| LockedInEditor | 1<<9 | 编辑器锁定 |
| HideInHierarchy | 1<<10 | 层级隐藏 |
| **IsOnEnableCalled** | **1<<11** | onEnable 已调用 |
| **IsEditorOnEnableCalled** | **1<<12** | 编辑器 onEnable 已调用 |
| **IsPreloadStarted** | **1<<13** | __preload 已启动 |
| **IsOnLoadCalled** | **1<<14** | onLoad 已调用 |
| **IsOnLoadStarted** | **1<<15** | onLoad 已启动 |
| **IsStartCalled** | **1<<16** | start 已调用 |
| IsRotationLocked | 1<<17 | 旋转锁定 |
| IsScaleLocked | 1<<18 | 缩放锁定 |
| IsAnchorLocked | 1<<19 | 锚点锁定 |
| IsSizeLocked | 1<<20 | 尺寸锁定 |
| IsPositionLocked | 1<<21 | 位置锁定 |
| IsSkipTransformUpdate | 1<<24 | 跳过变换更新 |

- **PersistentMask**: `~(ToDestroy | Dirty | Destroying | ...)` — 决定哪些标志在 clone/序列化时保留

### 1.3 销毁机制（延迟销毁模式）

```
destroy() 调用
    ↓
设置 ToDestroy 标志
    ↓
推入 objectsToDestroy[] 全局数组
    ↓
[编辑器模式] 设置 deferredDestroyTimer
    ↓
[JSB模式] 立即调用 this._destroy()
    ↓
--- 帧末渲染前 ---
    ↓
CCObject._deferredDestroy()
    ↓
遍历 objectsToDestroy[]
    ↓
对每个对象调用 _destroyImmediate()
    ↓
    ├── _onPreDestroy()    // 子类覆写(Node/Component)
    ├── [JSB] this.destruct()
    ├── _destruct()        // 编译生成的属性重置函数
    └── 设置 Destroyed 标志
```

**关键设计**:
- 销毁是**延迟**的 — 当前帧内对象仍 `isValid`，下一帧才真正失效
- `_destruct()` 使用 JIT 编译（`SUPPORT_JIT`）: 将属性重置编译为 `Function('o', 'o._x=null;o._y=null;...')`，避免每次反射
- Node 和 Component 的 `_id` 在 destruct 时被**保留**（`idToSkip = '_id'`）

### 1.4 JSB 桥接

```typescript
if (JSB) {
    copyAllProperties(CCObject, jsb.CCObject, ['prototype', 'length', 'name']);
    copyAllProperties(CCObject.prototype, jsb.CCObject.prototype, 
        ['constructor', 'name', 'hideFlags', 'isValid']);
    (CCObject as any) = jsb.CCObject;  // 原型替换！
}
```
JSB 环境下，CCObject 被替换为 `jsb.CCObject`（C++ 侧实现），TS 侧的属性/方法通过 `copyAllProperties` 复制到 native 原型上。

---

## 2. Component 类 (`cocos/scene-graph/component.ts`, 829行)

### 2.1 类层次

```
CCObject ← Component
```

通过 `@ccclass('cc.Component')` 注册为 CCClass。

### 2.2 关键属性

| 属性 | 类型 | 说明 |
|------|------|------|
| `node` | `Node` | 所属节点（`@serializable`，初始为 NullNode） |
| `_enabled` | `boolean` | 启用状态（`@serializable`，默认 true） |
| `__prefab` | `CompPrefabInfo\|null` | Prefab 关联信息（`@serializable`） |
| `_id` | `string` | 唯一ID，由 `IDGenerator('Comp')` 生成 |
| `_sceneGetter` | `(()=>RenderScene)\|null` | 渲染场景获取回调 |

### 2.3 生命周期方法

```typescript
// 生命周期方法（全部为可选 protected）
protected __preload?(): void;      // 内部初始化，onLoad 前
protected onLoad?(): void;         // 节点激活/组件附加时
protected start?(): void;          // 第一次 update 前
protected update?(dt: number): void;    // 每帧
protected lateUpdate?(dt: number): void; // 每帧（update 后）
protected onEnable?(): void;       // 组件启用 + 节点激活
protected onDisable?(): void;      // 组件禁用 / 节点停用
protected onDestroy?(): void;      // 销毁时
```

**internal 访问器**: 每个生命周期方法都有对应的 `internalXxx` public getter，供引擎内部调度使用：
```typescript
public get internalUpdate(): ((dt: number) => void) | undefined { return this.update; }
public get internalStart(): (() => void) | undefined { return this.start; }
// ...同理
```

### 2.4 enabled 属性的调度联动

```typescript
set enabled(value) {
    if (this._enabled !== value) {
        this._enabled = value;
        if (this.node.activeInHierarchy) {
            const compScheduler = legacyCC.director._compScheduler;
            if (value) {
                compScheduler.enableComp(this);   // → onEnable → 调度 start/update
            } else {
                compScheduler.disableComp(this);  // → onDisable → 取消调度
            }
        }
    }
}
```

**核心逻辑**: enabled 变更仅在 `node.activeInHierarchy === true` 时才触发调度，避免重复激活。

### 2.5 destroy 流程

```typescript
destroy(): boolean {
    // [编辑器] 检查 _requireComponent 依赖
    if (super.destroy()) {  // CCObject.destroy()
        if (this._enabled && this.node.activeInHierarchy) {
            compScheduler.disableComp(this);  // 取消调度
        }
        return true;
    }
}

_onPreDestroy(): void {
    this.unscheduleAllCallbacks();           // 取消所有定时器
    director._nodeActivator.destroyComp(this); // → onDisable + onDestroy
    this.node._removeComponent(this);        // 从节点移除
}
```

### 2.6 定时器系统

Component 持有对 Director Scheduler 的代理：
- `schedule(callback, interval, repeat, delay)` — 注册定时回调
- `scheduleOnce(callback, delay)` — 单次延迟回调
- `unschedule(callback_fn)` — 取消回调
- `unscheduleAllCallbacks()` — 取消所有

所有调度以 Component 自身为 target，支持暂停/恢复。

### 2.7 静态配置属性

```typescript
static _executionOrder: number = 0;       // 执行顺序
static _requireComponent: Constructor | null = null;  // 依赖组件
// [编辑器专用]
static _executeInEditMode = false;
static _disallowMultiple = null;
static _playOnFocus = false;
static _inspector = '';
static _icon = '';
```

---

## 3. Node 的组件管理系统 (`cocos/scene-graph/node.ts`)

### 3.1 组件存储

```typescript
@serializable
protected _components: Component[] = [];

get components(): ReadonlyArray<Component> { return this._components; }
public getWritableComponents(): Component[] { return this._components; }
```

### 3.2 addComponent 完整流程

```
addComponent(typeOrClassName)
    ↓
[字符串参数] js.getClassByName(name) → 获取构造函数
    ↓
类型检查: 必须是 Component 子类
    ↓
[编辑器] _disallowMultiple 检查
    ↓
_requireComponent 递归添加依赖组件
    ↓
const component = new constructor()     ← 直接 new！
component.node = this                   ← 双向关联
this._components.push(component)        ← 加入数组
    ↓
[编辑器] EditorExtends.Component.add()
    ↓
emit(NodeEventType.COMPONENT_ADDED)
    ↓
[节点已激活] director._nodeActivator.activateComp(component)
    ↓                                        ↓
                                    __preload → onLoad → onEnable → 调度 start/update
```

**关键发现**:
1. 组件通过 `new constructor()` 直接实例化 — 无工厂模式
2. `component.node = this` — 立即建立双向引用
3. `_requireComponent` 是递归的 — 可能触发多个组件创建
4. 激活时自动走完整生命周期

### 3.3 getComponent 查找机制

```typescript
protected static _findComponent<T>(node, constructor): T | null {
    if (constructor._sealed) {
        // 精确匹配: constructor === comp.constructor
        // 用于内置组件，避免 instanceof 的跨原型链匹配
    } else {
        // instanceof 匹配: comp instanceof constructor
        // 用于用户脚本，支持多态查找
    }
}
```

**双查找策略**:
- `_sealed` 类（内置组件）: 使用严格 `constructor ===` 比较，O(n) 但更快
- 非 `_sealed` 类（用户脚本）: 使用 `instanceof`，支持父类查找

### 3.4 _removeComponent

在 `Component._onPreDestroy()` 中调用：
```typescript
this.node._removeComponent(this);
```
从 `_components` 数组中移除引用。

---

## 4. 类注册与反射系统

### 4.1 @ccclass 装饰器 (`cocos/core/data/decorators/ccclass.ts`)

```typescript
export const ccclass = makeSmartClassDecorator<string>((constructor, name) => {
    let base = getSuper(constructor);
    if (base === Object) base = null;

    const proto = { name, extends: base, ctor: constructor };
    // 合并 @property 等装饰器缓存
    const cache = constructor[CACHE_KEY];
    if (cache) { mixin(proto, cache.proto); }

    const res = CCClass(proto);  // 调用核心注册
    return res;
});
```

### 4.2 CCClass 核心注册 (`cocos/core/data/class.ts`)

CCClass 是引擎的类型系统核心，负责：
1. **类名注册** → `js.setClassName(name, constructor)`
2. **属性定义** → 从 `proto.properties` 提取属性描述
3. **原型修改** → 在构造函数原型上定义 getter/setter
4. **继承链维护** → `__props__`, `__values__` 属性列表
5. **fastDefine** → 快速定义序列化属性默认值

### 4.3 类名注册表 (`cocos/core/utils/js-typed.ts`)

```typescript
// 全局注册表
const _nameToClass = {};  // className → Constructor
const _idToClass = {};    // classId → Constructor

export function setClassName(className, constructor) {
    doSetClassName(className, constructor);
    // 自动设置 classId
    if (!constructor.prototype.hasOwnProperty(classIdTag)) {
        _setClassId(id, constructor);
    }
}

export function getClassByName(classname) {
    return _nameToClass[classname];  // 简单哈希表查找
}
```

**反射查找链**:
```
类名字符串 → _nameToClass[name] → 构造函数 → new constructor() → 组件实例
```

### 4.4 @property 装饰器 (`cocos/core/data/decorators/property.ts`)

```typescript
export function property(target, propertyKey, descriptorOrInitializer) {
    const classStash = getClassCache(target.constructor);  // 获取类缓存
    const propertyStash = properties[propertyKey] ??= {};  // 创建属性缓存

    // 合并选项
    mergePropertyOptions(classStash, propertyStash, ctor, propertyKey, options, descriptor);
}
```

**属性描述符结构 (PropertyStash)**:
- `type`: 属性类型（CCString/CCInteger/CCFloat/构造函数等）
- `default`: 默认值（函数或直接值）
- `get/set`: 计算属性
- `serializable`: 是否可序列化
- `editorOnly`: 仅编辑器
- `visible`, `displayName`, `tooltip`, `range` 等编辑器元数据

**默认值提取策略**:
1. **Babel**: 从 `descriptor.initializer` 获取
2. **TypeScript**: 实例化一个临时对象 `new constructor()`，读取属性值
3. **显式指定**: `@property({ default: value })`

---

## 5. 组件调度器 (`cocos/scene-graph/component-scheduler.ts`, 593行)

### 5.1 架构概览

```
ComponentScheduler
    ├── startInvoker: OneOffInvoker    ← start() 只调用一次
    ├── updateInvoker: ReusableInvoker ← update(dt) 每帧调用
    ├── lateUpdateInvoker: ReusableInvoker ← lateUpdate(dt) 每帧调用
    ├── _deferredComps: Component[]    ← 帧中新增的组件缓冲
    └── _updating: boolean             ← 是否在帧循环中
```

### 5.2 三桶排序 (Three-Bucket Sorting)

每个 Invoker 维护三个优先级桶：
- `_neg`: `_executionOrder < 0` 的组件
- `_zero`: `_executionOrder === 0` 的组件（默认）
- `_pos`: `_executionOrder > 0` 的组件

**执行顺序**: `_neg` → `_zero` → `_pos`

### 5.3 OneOffInvoker vs ReusableInvoker

| 特性 | OneOffInvoker | ReusableInvoker |
|------|---------------|-----------------|
| 用途 | start() | update() / lateUpdate() |
| 排序时机 | invoke() 时排序 | add() 时排序 |
| 调用后 | 清空数组 | 保留数组（可复用） |
| 插入方式 | push（后排序） | sortedIndex 二分插入 |

### 5.4 JIT 编译优化

```typescript
// JIT 模式：将循环编译为字符串函数
const invokeUpdate = SUPPORT_JIT 
    ? createInvokeImplJit('c.update(dt)', true)
    : createInvokeImpl(
        (c, dt) => { c.update(dt); },
        (iterator, dt) => { for (...) array[i].update(dt); }
    );
```

编译结果类似：
```javascript
function(it, dt) {
    var a = it.array;
    for(it.i = 0; it.i < a.length; ++it.i) {
        var c = a[it.i];
        c.update(dt);
    }
}
```

**优势**: 避免每帧函数调用开销，V8 可内联优化。

### 5.5 异常恢复机制

```typescript
// createInvokeImpl 中的 try-catch 策略
return (iterator, dt) => {
    try {
        fastPath(iterator, dt);  // JIT 编译的快速路径
    } catch (e) {
        legacyCC._throw(e);
        if (ensureFlag) array[iterator.i]._objFlags |= ensureFlag;
        ++iterator.i;  // 跳过失败的组件
        for (; iterator.i < array.length; ++iterator.i) {
            try { singleInvoke(array[iterator.i], dt); }
            catch (e) { /* 标记 + 继续 */ }
        }
    }
};
```

**设计思想**: 快速路径无 try-catch（性能优先），异常时回退到逐组件 try-catch 的慢路径，确保一个组件崩溃不影响其他组件。

### 5.6 enableComp / disableComp 流程

```
enableComp(comp, invoker?)
    ↓
[已调用 onEnable?] 检查 IsOnEnableCalled 标志
    ↓
[comp.internalOnEnable 存在?]
    ├── 有 invoker → invoker.add(comp)     ← 延迟到批量调用
    └── 无 invoker → 直接调用 comp.internalOnEnable()
    ↓
_onEnabled(comp)
    ├── scheduler.resumeTarget(comp)       ← 恢复定时器
    ├── 设置 IsOnEnableCalled 标志
    └── _scheduleImmediate(comp)           ← 注册 start/update/lateUpdate
        ├── [有 internalStart 且未调用] → startInvoker.add(comp)
        ├── [有 internalUpdate] → updateInvoker.add(comp)
        └── [有 internalLateUpdate] → lateUpdateInvoker.add(comp)

disableComp(comp)
    ↓
[已调用 onEnable?] 检查 IsOnEnableCalled 标志
    ↓
comp.internalOnDisable()
    ↓
_onDisabled(comp)
    ├── scheduler.pauseTarget(comp)        ← 暂停定时器
    ├── 清除 IsOnEnableCalled 标志
    ├── 从 _deferredComps 中移除
    └── 从 startInvoker/updateInvoker/lateUpdateInvoker 移除
```

### 5.7 帧循环调度时序

```
Director.tick(dt)
    ↓
CompScheduler.startPhase()
    ├── _updating = true
    ├── startInvoker.invoke()         ← 调用所有 start()
    │   └── 设置 IsStartCalled 标志
    └── _startForNewComps()           ← 处理帧中新增组件
    ↓
CompScheduler.updatePhase(dt)
    └── updateInvoker.invoke(dt)      ← 调用所有 update(dt)
    ↓
CompScheduler.lateUpdatePhase(dt)
    ├── lateUpdateInvoker.invoke(dt)  ← 调用所有 lateUpdate(dt)
    ├── _updating = false
    └── _startForNewComps()           ← 处理帧中新增组件（下一帧执行）
```

---

## 6. NodeActivator (`cocos/scene-graph/node-activator.ts`, 422行)

### 6.1 职责

NodeActivator 负责**节点激活/反激活**时触发的组件生命周期管理，与 ComponentScheduler 协同工作。

### 6.2 激活流程

```
NodeActivator.activateNode(node, true)
    ↓
从 activateTasksPool 获取 ActivateTask
    ↓
_activateNodeRecursively(node, preload, onLoad, onEnable)
    ↓
[检查 Deactivating 标志] ← 防止激活-反激活死循环
    ↓
node._setActiveInHierarchy(true)
    ↓
遍历 node._components → activateComp(comp, preload, onLoad, onEnable)
    ↓
遍历 node.children → 递归激活子节点
    ↓
node._onPostActivated(true)
    ↓
[返回后] 批量调用:
    task.preload.invoke()    ← __preload
    task.onLoad.invoke()     ← onLoad
    task.onEnable.invoke()   ← onEnable
    ↓
回收 ActivateTask 到 Pool
```

### 6.3 activateComp 详细流程

```
activateComp(comp, preloadInvoker, onLoadInvoker, onEnableInvoker)
    ↓
[检查 isValid(comp, true)] ← 已销毁则跳过
    ↓
[未启动 __preload?]
    ├── 设置 IsPreloadStarted 标志
    └── [有 internalPreload] → preloadInvoker.add(comp) 或直接调用
    ↓
[未启动 onLoad?]
    ├── 设置 IsOnLoadStarted 标志
    └── [有 internalOnLoad] → onLoadInvoker.add(comp) 或直接调用
    ↓
[comp._enabled?]
    └── [node.activeInHierarchy?] → compScheduler.enableComp(comp, onEnableInvoker)
```

### 6.4 反激活流程

```
NodeActivator.activateNode(node, false)
    ↓
_deactivateNodeRecursively(node)
    ↓
设置 Deactivating 标志
    ↓
node._setActiveInHierarchy(false)
    ↓
遍历 _components → compScheduler.disableComp(component)
    ↓
[中途被重新激活?] → 清除 Deactivating，立即返回
    ↓
遍历 children → 递归反激活子节点
    ↓
node._onPostActivated(false)
清除 Deactivating 标志
```

### 6.5 对象池优化

```typescript
const activateTasksPool = new Pool<ActivateTask>(4);
```

ActivateTask 使用对象池复用，避免频繁 GC。每个 task 包含三个 invoker：
- `preload: UnsortedInvoker`（无排序，内部用）
- `onLoad: OneOffInvoker`（按 executionOrder 排序）
- `onEnable: OneOffInvoker`（按 executionOrder 排序）

### 6.6 去抖动 (Debounce) 机制

反激活时，会清理激活栈中已不活跃的组件：
```typescript
// 从之前的激活任务中移除不活跃的组件
for (const lastTask of stack) {
    lastTask.preload.cancelInactive(IsPreloadStarted);
    lastTask.onLoad.cancelInactive(IsOnLoadStarted);
    lastTask.onEnable.cancelInactive(IsOnEnableCalled);
}
```

---

## 7. 完整生命周期时序图

```
节点首次激活 (active = true)
    │
    ├─ NodeActivator._activateNodeRecursively()
    │   ├─ node._setActiveInHierarchy(true)
    │   └─ 对每个 Component:
    │       ├─ __preload()          ← UnsortedInvoker
    │       ├─ onLoad()             ← OneOffInvoker (排序后)
    │       ├─ onEnable()           ← OneOffInvoker (排序后)
    │       └─ [调度注册] start/update/lateUpdate
    │
    ├─ [帧循环]
    │   ├─ CompScheduler.startPhase()
    │   │   └─ start()              ← OneOffInvoker (仅一次)
    │   ├─ CompScheduler.updatePhase(dt)
    │   │   └─ update(dt)           ← ReusableInvoker (每帧)
    │   └─ CompScheduler.lateUpdatePhase(dt)
    │       └─ lateUpdate(dt)       ← ReusableInvoker (每帧)
    │
    ├─ [节点反激活] (active = false)
    │   └─ NodeActivator._deactivateNodeRecursively()
    │       └─ 对每个 Component:
    │           ├─ onDisable()
    │           └─ [取消调度] start/update/lateUpdate
    │
    └─ [组件销毁] (component.destroy())
        ├─ CompScheduler.disableComp() → onDisable()
        ├─ unscheduleAllCallbacks()
        ├─ NodeActivator.destroyComp() → onDestroy()
        └─ node._removeComponent(this)
```

---

## 8. C++ 化设计关键考量

### 8.1 可 Native 化的部分

| 子系统 | C++ 化可行性 | 说明 |
|--------|-------------|------|
| CCObjectFlags 状态机 | ✅ 高 | 纯位运算，无 JS 依赖 |
| Component._enabled 调度 | ✅ 高 | 标志位 + 数组操作 |
| ComponentScheduler 三桶排序 | ✅ 高 | 固定结构数组 + 二分插入 |
| OneOffInvoker / ReusableInvoker | ✅ 高 | 无 JS 特殊语义 |
| JIT 编译循环 | ⚠️ 中 | C++ 不需要，直接循环即可 |
| NodeActivator 递归激活 | ✅ 高 | 递归遍历 + 批量调用 |

### 8.2 必须 JS 保留的部分

| 子系统 | 原因 |
|--------|------|
| CCClass 类型系统 | JS 反射/动态属性/装饰器 |
| @property 默认值提取 | 依赖 `new constructor()` 实例化 |
| getClassByName 查找 | 字符串→构造函数的动态映射 |
| 用户脚本生命周期 | 用户定义的 start/update 等 |
| _destruct() JIT 编译 | 动态生成属性重置函数 |

### 8.3 桥接设计建议

```
C++ ComponentScheduler
    ↕ _tempFloatArray / 消息队列
JS ComponentScheduler (用户脚本部分)
```

- **内置组件** (Sprite/RigidBody 等): 完全 C++ 化，生命周期在 C++ 侧直接调用
- **用户脚本组件**: 保留 JS 调度，通过桥接回调 C++ 侧
- **混合模式**: C++ 调度器管理执行顺序，但用户脚本的 start/update 通过 JS 引擎执行
