# Cocos Creator v3.8.8 - 用户脚本系统深度分析

> 生成日期: 2026-04-25
> 分析范围: 装饰器 / Property系统 / 脚本编译注册 / 生命周期执行 / JSB脚本桥接

---

## 1. 脚本执行环境

### 1.1 模块加载机制

Cocos Creator 使用自定义模块系统，用户脚本的加载流程：

```
[编辑器] TypeScript 源码
    ↓ tsc 编译
[构建输出] JavaScript 模块
    ↓ 运行时 require / import
[引擎] 模块执行
    ↓ @ccclass 装饰器触发
[注册] CCClass 注册到全局类型系统
```

### 1.2 RequiringFrame (`cocos/core/data/utils/requiring-frame.ts`)

RequiringFrame 是脚本加载的帧追踪机制，维护一个栈：

```typescript
let requiringFrames = [];

export function push(module, uuid, script, importMeta) {
    requiringFrames.push({
        uuid,           // 脚本资源 UUID
        script,         // 脚本标识
        module,         // 模块对象
        exports: module.exports,  // 原始 exports
        beh: null,      // 行为（CCClass）
        importMeta,
    });
}

export function pop() {
    const frameInfo = requiringFrames.pop();
    // 如果模块未导出任何内容，自动导出 CCClass
    if (exports === frameInfo.exports) {
        module.exports = frameInfo.cls;  // 自动导出类
    }
}

export function peek() {
    return requiringFrames[requiringFrames.length - 1];
}
```

**用途**:
- 追踪当前正在加载的脚本模块
- 为装饰器提供当前脚本上下文
- 自动导出唯一的 CCClass（无需手动 `export default`）
- 编辑器模式下支持 `reset()` 清理所有帧

---

## 2. 脅件器系统

### 2.1 装饰器总览 (`cocos/core/data/class-decorator.ts`)

| 装饰器 | 类型 | 目标 | 说明 |
|--------|------|------|------|
| `ccclass` | 类 | 构造函数 | 注册为 CCClass |
| `property` | 属性 | 属性/访问器 | 声明序列化属性 |
| `requireComponent` | 类 | 构造函数 | 依赖组件 |
| `executionOrder` | 类 | 构造函数 | 执行优先级 |
| `disallowMultiple` | 类 | 构造函数 | 禁止同节点多实例 |
| `executeInEditMode` | 类 | 构造函数 | 编辑器模式执行 |
| `menu` | 类 | 构造函数 | 组件菜单路径 |
| `playOnFocus` | 类 | 构造函数 | 聚焦时执行 |
| `inspector` | 类 | 构造函数 | 自定义检查器 |
| `icon` | 类 | 构造函数 | 自定义图标 |
| `help` | 类 | 构造函数 | 帮助文档 URL |
| `type` | 属性 | 属性 | 指定类型 |
| `serializable` | 属性 | 属性 | 可序列化标记 |
| `editable` | 属性 | 属性 | 可编辑标记 |
| `visible` | 属性 | 属性 | 可见性 |
| `displayName` | 属性 | 属性 | 显示名称 |
| `tooltip` | 属性 | 属性 | 提示信息 |
| `range` | 属性 | 属性 | 值范围 |
| `override` | 属性 | 属性 | 重写父类属性 |
| `formerlySerializedAs` | 属性 | 属性 | 旧序列化名 |

### 2.2 @ccclass 核心流程

```typescript
// makeSmartClassDecorator: 支持有参和无参两种用法
// @ccclass 或 @ccclass('MyClass')

export const ccclass = makeSmartClassDecorator<string>((constructor, name) => {
    let base = getSuper(constructor);
    if (base === Object) base = null;

    const proto = { name, extends: base, ctor: constructor };
    
    // 合并 @property 等装饰器缓存的元数据
    const cache = constructor[CACHE_KEY];
    if (cache?.proto) { mixin(proto, cache.proto); }
    
    // 核心：调用 CCClass 注册
    const res = CCClass(proto);
    
    // DEV 模式验证方法签名
    if (DEV) {
        const propNames = Object.getOwnPropertyNames(constructor.prototype);
        for (propName of propNames) {
            doValidateMethodWithProps_DEV(func, prop, getClassName(constructor), constructor, base);
        }
    }
    return res;
});
```

**CCClass 注册时做了什么**:
1. `js.setClassName(name, constructor)` — 注册到全局名称表
2. 自动设置 `__props__` / `__values__` 属性列表
3. 在原型上定义属性的 getter/setter（含编辑器属性验证）
4. 处理继承链（合并父类属性）
5. 缓存析构函数 `__destruct__`

### 2.3 makeSmartClassDecorator 设计模式

```typescript
function makeSmartClassDecorator<TArg>(decorate) {
    return function proxyFn(target) {
        if (typeof target === 'function') {
            // 无参数: @ccclass → decorate(class)
            return decorate(target);
        } else {
            // 有参数: @ccclass('name') → decorate(class, 'name')
            return function(constructor) {
                return decorate(constructor, target);
            };
        }
    };
}
```

### 2.4 @property 详细机制

**三种用法**:

```typescript
// 1. 无参数（从类字段自动推断默认值）
@property
public speed: number = 10;

// 2. 指定类型
@property(SpriteFrame)
public frame: SpriteFrame | null = null;

// 3. 完整选项
@property({ type: CCFloat, range: [0, 100], tooltip: "Speed" })
public speed: number = 10;
```

**内部流程**:

```
@property(options)
    ↓
创建/获取 ClassStash（类缓存）
    ↓
创建/获取 PropertyStash（属性缓存）
    ↓
mergePropertyOptions() — 合并属性描述
    ↓
setDefaultValue() — 设置默认值
    ├── Babel: descriptor.initializer → 函数返回值
    ├── TypeScript: new constructor() → 实例属性值
    └── 显式: options.default → 直接值
    ↓
[ccclass 装饰器触发时]
    ↓
ClassStash.proto.properties → CCClass(proto)
    ↓
CCClass 解析 properties → 定义原型 getter/setter
```

**PropertyStash 内部标志** (`PropertyStashInternalFlag`):
- `STANDALONE`: 独立属性（非 @property 声明的 @serializable 等）
- `NOT_SERIALIZABLE`: 不可序列化
- `OVERRIDE`: 重写父类属性

---

## 3. 脚本资源类型 (`cocos/asset/assets/scripts.ts`)

```typescript
@ccclass('cc.Script')
class Script extends Asset {}

@ccclass('cc.JavaScript')
class JavaScript extends Script {}

@ccclass('cc.TypeScript')
class TypeScript extends Script {}
```

脚本资源是轻量级 Asset，主要作为组件的 `__scriptAsset` 引用存在。脚本的实际逻辑由模块系统加载，Script Asset 本身不持有可执行代码。

---

## 4. 生命周期执行（完整链路）

### 4.1 从 Node 激活到用户脚本的完整调用链

```
Director.tick(dt)
    ↓
CompScheduler.startPhase()
    ↓
startInvoker.invoke()
    ↓
[JIT 路径]
var a = it.array;
for(it.i = 0; it.i < a.length; ++it.i) {
    var c = a[it.i];       // c = 用户脚本组件
    c.start();             // 调用用户定义的 start()
    c._objFlags |= IsStartCalled;
}
    ↓
[异常恢复]
try { fastPath(iterator); }
catch (e) {
    legacyCC._throw(e);
    c._objFlags |= IsStartCalled;  // 仍然标记已调用
    ++iterator.i;
    // 慢路径逐个 try-catch
}
```

### 4.2 update/lateUpdate 的调度注册

```
Component.enabled = true
    ↓
CompScheduler.enableComp(comp)
    ↓
_onEnabled(comp)
    ↓
_scheduleImmediate(comp)
    ├── [comp.internalStart 存在且未调用] → startInvoker.add(comp)
    ├── [comp.internalUpdate 存在] → updateInvoker.add(comp)
    └── [comp.internalLateUpdate 存在] → lateUpdateInvoker.add(comp)
```

**关键**: 仅当组件定义了 `update()` 或 `lateUpdate()` 方法时，才会被加入调度器。没有这些方法的组件不会产生每帧开销。

### 4.3 internalXxx 访问器的作用

```typescript
// Component 中定义
protected update?(dt: number): void;
public get internalUpdate(): ((dt: number) => void) | undefined { return this.update; }
```

`internalUpdate` 作为公共访问器，允许调度器检测组件是否定义了 `update` 方法，而不需要直接访问 `protected` 属性。这是一个 **引擎内部 API 边界**设计。

---

## 5. JSB 脚本桥接

### 5.1 脚本在 Native 平台的执行

```
[Native 启动]
    ↓
ScriptEngine 初始化 (C++)
    ↓
注册 native 绑定对象 (jsb.*)
    ↓
[加载游戏脚本]
    ↓
通过 JSCore/V8 执行编译后的 JS
    ↓
JS 中的 @ccclass 注册 CCClass
    ↓
[jsb.CCObject] 替换 [TS CCObject]
    ↓
组件实例化走 JS 路径
```

**核心事实**: 在 JSB 环境下，用户脚本仍然由 JS 引擎执行。Native 层不直接执行用户脚本。

### 5.2 native-binding/decorators.ts (127KB 自动生成)

这是最大的单文件，包含所有 C++ 侧类的 JS 补丁：

```typescript
// 典型模式：属性桥接
Object.defineProperty(jsb.Sprite.prototype, 'spriteFrame', {
    get() { return this._spriteFrame; },
    set(v) { this._spriteFrame = v; this._onSpriteFrameUpdate(); }
});

// 构造钩子
const _orig_ctor = jsb.Sprite.prototype._ctor;
jsb.Sprite.prototype._ctor = function(...args) {
    _orig_ctor.call(this, ...args);
    // 初始化 JS 侧属性
};
```

### 5.3 _tempFloatArray 桥接

```typescript
// JSB 环境下的核心数据传递通道
const _tempFloatArray = new Float32Array(64);  // 共享内存

// Node 变换更新
node._lpos.x = 100;
node._lpos.y = 200;
_tempFloatArray[0] = node._lpos.x;
_tempFloatArray[1] = node._lpos.y;
nativeFunc.updateTransform(_tempFloatArray);  // 一次桥接调用传递多个值
```

### 5.4 双实现文件 (.ts vs .jsb.ts)

| 文件 | 平台 | 说明 |
|------|------|------|
| `node.ts` (109KB) | Web | 完整 Node 实现 |
| `node.jsb.ts` (48KB) | Native | 精简版，大量逻辑代理到 C++ |
| `component.ts` (33KB) | Web | 完整 Component |
| `scene.jsb.ts` (4.7KB) | Native | Scene 的 native 补丁 |

JSB 版本通常是 Web 版本的精简，因为核心逻辑在 C++ 侧执行，JS 仅提供接口兼容层。

---

## 6. 脚本热重载

### 6.1 编辑器脚本重载

```
[用户修改脚本]
    ↓
编辑器检测文件变更
    ↓
重新编译 TS → JS
    ↓
cclegacy._RF.reset()      ← 清空 requiringFrames
    ↓
重新加载脚本模块
    ↓
CCClass 重新注册（覆盖旧的类定义）
    ↓
[场景中的旧组件实例]
    ├── 构造函数引用更新
    └── 属性保持不变（已序列化的值）
```

### 6.2 脚本重置机制

```typescript
// 编辑器中可用的重置方法
if (EDITOR) {
    cclegacy._RF.reset = () => { requiringFrames = []; };
}
```

---

## 7. C++ 化设计关键考量

### 7.1 用户脚本无法 C++ 化

用户脚本的本质是 **用户定义的 JS/TS 代码**，必须由 JS 引擎执行。因此：

| 方面 | 结论 |
|------|------|
| 用户脚本执行 | ❌ 无法 C++ 化，必须保留 JS 引擎 |
| 脚本注册/反射 | ❌ 依赖 JS CCClass 系统 |
| @property 默认值 | ❌ 依赖 JS 实例化 |
| 生命周期调度 | ⚠️ 部分 C++ 化（见下） |
| 装饰器处理 | ❌ 编译时 JS 特性 |

### 7.2 可优化的方向

**方向 1: 内置组件 C++ 化**
```
Sprite (JS) → Sprite (C++) + 薄 JS 绑定层
    ↓
C++ 直接调用 update/render
    ↓
减少 JS↔C++ 桥接次数
```

**方向 2: 调度器 C++ 化**
```
C++ ComponentScheduler
    ├── 管理内置组件的调度（直接 C++ 调用）
    └── 通过回调调用 JS 用户脚本的 start/update
```

**方向 3: 属性系统预编译**
```
[构建时] 分析 @property 装饰器
    ↓
生成 C++ 属性描述（类型/偏移/默认值）
    ↓
[运行时] C++ 直接读写属性，无需 JS 反射
```

### 7.3 最小可行 C++ 脚本桥接

```
C++ ScriptEngine Interface
    ├── registerClass(name, constructor)     ← @ccclass
    ├── defineProperty(name, type, offset)   ← @property
    ├── scheduleStart(compId)                ← start
    ├── scheduleUpdate(compId)               ← update(dt)
    └── invokeLifecycle(compId, phase)       ← 通用生命周期回调

    ↕ 桥接

JS Script Engine (V8/JSCore)
    ├── 执行用户脚本
    ├── CCClass 类型系统
    └── 回调通知 C++ 侧
```

### 7.4 终极目标架构

```
┌────────────────────────────────────────┐
│           C++ Engine Core              │
│  ┌──────────┐  ┌──────────────────────┐│
│  │ Scheduler │  │ AssetManager (C++)  ││
│  │ (C++)    │  │ Scene System (C++)   ││
│  └────┬─────┘  └──────────┬───────────┘│
│       │                    │            │
│  ┌────┴────────────────────┴───────────┐│
│  │       C++/JS Bridge Layer           ││
│  │  _tempFloatArray / Callback Queue   ││
│  └────┬────────────────────┬───────────┘│
└───────┼────────────────────┼────────────┘
        │                    │
┌───────┴────────┐  ┌───────┴────────────┐
│  Built-in Comps│  │  User Scripts      │
│  (C++ Native)  │  │  (JS Engine)       │
│  Sprite/Camera │  │  @ccclass/@property│
│  RigidBody     │  │  Custom Components │
└────────────────┘  └────────────────────┘
```

**核心思想**: 内置组件走纯 C++ 路径（零 JS 开销），用户脚本走 JS 引擎路径（通过最小桥接接口与 C++ 通信）。
