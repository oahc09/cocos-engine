# 阶段4-TS绑定入口（S4-T1）

> 范围：`cocos/native-binding/` + `@types/jsb.d.ts` + `cc.config.json` 模块覆盖机制
> 目标：理解 TS 侧如何接收、封装、暴露 C++ 原生绑定的 JavaScript 接口。

---

## 1. 目录结构与文件职责

```
cocos/native-binding/
  ├── index.ts           # 公共出口：re-export internal:native + 声明 native 命名空间类型 + 导出 native 对象
  ├── impl.ts            # 运行时实现：初始化 jsb 全局对象（reflection/bridge/batch-call）
  ├── decorators.ts      # 自动生成：56 个 patch_* 函数，为 JSB 类重新挂载编辑器装饰器
  └── category.json      # 模块元信息（中文："原生接口"）
```

辅助文件：

| 文件 | 职责 |
|---|---|
| `@types/globals.d.ts` | 声明 `declare module 'internal:native' {}`（虚拟模块） |
| `@types/jsb.d.ts` | 手写 JSB 类型声明（`declare namespace jsb`，约 887 行） |
| `@types/box2d-jsb.d.ts` | Box2D 物理 JSB 类型声明 |
| `cc.config.json` | 配置 `internal:native` → `impl.ts` 的模块覆盖 + NATIVE 时 `.ts` → `.jsb.ts` 替换 |
| `cocos/core/scripting/batch-executor.ts` | 脚本桥批量调用（start/update/lateUpdate 的 C++ → JS 批量回调） |

---

## 2. 模块加载链路

### 2.1 虚拟模块 `internal:native`

`internal:native` 是一个**不存在于磁盘的虚拟模块**，通过三层机制工作：

**TypeScript 编译期**（`@types/globals.d.ts:107`）：
```ts
declare module 'internal:native' {}
```

**构建期覆盖**（`cc.config.json` moduleOverrides）：
```json
{
    "test": "true",
    "isVirtualModule": true,
    "overrides": {
        "internal:native": "cocos/native-binding/impl.ts"
    }
}
```

**测试期 Mock**（`tests/init.ts:20`）：
```ts
jest.mock('internal:native', () => ({ __esModule: true, default: {} }), { virtual: true });
```

### 2.2 加载流程

```
index.ts
  ├── export * from 'internal:native'   → impl.ts（运行时值）
  ├── export declare namespace native    → 类型声明（编译期）
  └── export const native = {...}        → 运行时对象（从 globalThis.jsb 提取）
```

---

## 3. impl.ts 运行时实现详解

文件：`cocos/native-binding/impl.ts`（166 行）

仅在 `NATIVE` 宏为 true 时执行（即原生平台）。

### 3.1 初始化步骤

| 步骤 | 代码位置 | 作用 |
|---|---|---|
| 获取 jsb 全局 | `const globalJsb = globalThis.jsb ?? {}` | C++ 引擎在 JS 启动前注入 |
| 安装批量调用 | `installScriptBridgeBatchCall(globalJsb)` | 注册 `__scriptBridgeBatchCall`，允许 C++ 批量调用 JS 组件生命周期 |
| jsb.reflection | 惰性 getter | 按平台创建 `JavascriptJavaBridge` / `JavaScriptObjCBridge` / `JavaScriptArkTsBridge` |
| jsb.bridge | 惰性 getter | 创建 `ScriptNativeBridge` 实例（低层双向消息） |
| jsb.jsbBridgeWrapper | 惰性 getter | 高层事件系统，封装 bridge.onNative 回调 |
| jsb.saveImageData | 覆写 | 将同步函数包装为 Promise 版本 |

### 3.2 native 对象导出

```ts
export const native = {
    DownloaderHints, Downloader, zipUtils, fileUtils,
    DebugRenderer, copyTextToClipboard, garbageCollect,
    reflection, bridge, jsbBridgeWrapper,
    AssetsManager, EventAssetsManager, Manifest,
    saveImageData, process, adpf,
};
```

这是 `globalThis.jsb` 的**精选子集**，只暴露开发者需要的接口。

---

## 4. 三大跨平台桥接器

### 4.1 reflection（反射调用原生静态方法）

按平台选择桥接类：

| 平台 | 桥接类 | 用途 |
|---|---|---|
| Android / OHOS | `JavascriptJavaBridge` | 调用 Java/Kotlin 静态方法 |
| iOS / macOS | `JavaScriptObjCBridge` | 调用 ObjC 静态方法 |
| OpenHarmony | `JavaScriptArkTsBridge` | 调用 ArkTS 静态方法 |

API：
```ts
native.reflection.callStaticMethod(className, methodName, methodSignature, ...params)
```

### 4.2 bridge（底层双向消息通道）

- `ScriptNativeBridge` 实例
- `bridge.sendToNative(eventName, arg)` — TS → 原生
- `bridge.onNative = (methodName, arg1) => {...}` — 原生 → TS 回调

### 4.3 jsbBridgeWrapper（高层事件系统）

```ts
jsbBridgeWrapper.addNativeEventListener(eventName, listener)  // 注册监听
jsbBridgeWrapper.dispatchEventToNative(eventName, arg)         // 派发到原生
jsbBridgeWrapper.triggerEvent(eventName, arg)                  // 原生回调触发
```

底层由 `bridge.onNative` 驱动，内部维护 `Map<string, Function[]>` 事件映射。

---

## 5. decorators.ts — 自动生成的装饰器补丁

文件：`cocos/native-binding/decorators.ts`（1610 行，自动生成）

### 5.1 patch_* 函数模式

每个 JSB 类对应一个 `patch_<ClassName>` 函数：

```ts
export function patch_cc_Asset(ctx: cc_Asset_Context_Args, apply = defaultExec) {
    const { Asset } = { ...ctx };
    apply(() => { $.serializable(Asset.prototype, '_native', () => ''); }, 'serializable', '_native');
    apply(() => { $.ccclass('cc.Asset')(Asset); }, 'ccclass', null);
}
```

**执行机制**：
- `defaultExec(cb)` 直接执行回调 — 运行时默认行为。
- `apply` 参数允许拦截/过滤/延迟装饰器应用（用于测试或选择性挂载）。

每个 `apply()` 调用传入三个参数：
1. 回调函数（包装实际装饰器调用）
2. 装饰器名称字符串（如 `'serializable'`、`'ccclass'`）
3. 属性名或 `null`（类级装饰器）

### 5.2 覆盖的装饰器类型

| 装饰器 | 作用 |
|---|---|
| `ccclass` | 类注册到编辑器类系统 |
| `serializable` | 标记字段可序列化 |
| `type` | 属性类型标注 |
| `property` | 属性组元数据 |
| `tooltip` | 编辑器悬浮提示 |
| `editable` | 标记属性可编辑 |
| `visible` | 条件可见性 |
| `displayOrder` | 检查器面板排序 |
| `range` / `rangeStep` / `rangeMin` | 数值范围约束 |
| `formerlySerializedAs` | 字段重命名迁移 |
| `override` | 覆盖继承属性 |
| `executeInEditMode` | 编辑器模式下执行 |
| `menu` / `help` | 组件菜单 / 帮助链接 |
| `editorOnly` | 仅编辑器属性 |
| `readOnly` | 只读属性 |

### 5.3 规模

- **56 个 patch_* 函数**，覆盖 56 个 JSB 类
- **约 736 个 apply() 调用**
- 覆盖类包括：Asset、Node、Scene、Material、Mesh、各 Light 类型、各 Pipeline/Flow/Stage 类型、各 Asset 子类等

### 5.4 为什么需要装饰器补丁

C++ 绑定生成的 JSB 类只有**方法和属性**，没有 TS 装饰器元数据（ccclass 名称、serializable 标记、tooltip、range 等）。`patch_*` 函数将这些元数据重新挂载到原生类的 prototype 上，使编辑器、序列化、Inspector 正常工作。

---

## 6. .jsb.ts 文件覆盖机制

### 6.1 cc.config.json 模块覆盖

当 `NATIVE` 为 true 时，构建系统将 `.ts` 文件替换为 `.jsb.ts`：

```json
{
    "test": "context.buildTimeConstants.NATIVE",
    "overrides": {
        "cocos/gfx/index.ts": "cocos/gfx/index.jsb.ts",
        "cocos/scene-graph/node.ts": "cocos/scene-graph/node.jsb.ts",
        "cocos/asset/assets/asset.ts": "cocos/asset/assets/asset.jsb.ts",
        "cocos/render-scene/core/pass.ts": "cocos/render-scene/core/pass.jsb.ts"
    }
}
```

### 6.2 四种 JSB 绑定模式

#### 模式 A：纯重导出（GFX 类）

```ts
declare const gfx: any;  // C++ 注入的全局命名空间
export const Device: typeof JsbDevice = gfx.Device;
export type Device = JsbDevice;
```

特点：无 prototype 扩展、无 patch_* 调用。原生类已完整实现所有方法。

#### 模式 B：构造器 + Prototype + 装饰器补丁（Asset）

```ts
declare const jsb: any;
applyMixins(jsb.Asset, [CallbacksInvoker]);        // 混入 JS 接口
jsb.Asset.prototype._ctor = function() { ... };     // 定义初始化函数
Object.defineProperty(proto, '_nativeAsset', {...}); // JS 侧属性访问器
proto.addRef = function() { ... };                   // JS 侧方法
patch_cc_Asset({Asset});                             // 装饰器补丁
export const Asset = jsb.Asset;
```

#### 模式 C：重度 Prototype 覆写 + 共享内存 + 装饰器补丁（Node）

```ts
// 1. 共享内存：JS 与 C++ 通过 SharedArrayBuffer 交换数据
const sharedBuffer = this._initAndReturnSharedBuffer();
this._sharedUint32Arr = new Uint32Array(sharedBuffer, 0, 3);
this._sharedFloat32Arr = new Float32Array(sharedBuffer, 20, 4);

// 2. 保存原生方法后覆写
const nativeSetPosition = proto.setPosition;
proto.setPosition = function(val, y?, z?) { ... };

// 3. _tempFloatArray 协议：C++ 写入 → JS 读取
proto.getWorldPosition = function(out) {
    this._getWorldPosition();  // C++ 写入 _tempFloatArray
    return out.set(_tempFloatArray[0], _tempFloatArray[1], _tempFloatArray[2]);
};

// 4. 定义 C++ → JS 回调桩
proto._onTransformChanged = function(type) { this.emit(...); };
proto._onChildAdded = function(child) { this._children.push(child); ... };

// 5. 装饰器补丁
patch_cc_Node({Node, Vec3, Quat, MobilityMode, Layers});
```

#### 模式 D：最小覆写（Pass）

```ts
proto.getUniform = function(handle, out) {
    const val = this._getUniform(handle);  // 调用原生
    // JS 侧处理数学类型转换
    return out;
};
```

### 6.3 .jsb.ts 通用模板

| 步骤 | 说明 | 适用模式 |
|---|---|---|
| `declare const jsb: any` | 声明全局 jsb 对象 | A/B/C/D |
| `export const X = jsb.X` | 导出原生构造器 | A/B/C/D |
| `applyMixins` | 混入 JS 接口 | B/C |
| `_ctor` 定义 | JS 侧初始化状态 | B/C |
| 保存原生方法 | 覆写前保存引用 | C |
| prototype 方法覆写 | 添加/替换方法 | B/C/D |
| `Object.defineProperty` | 定义属性访问器 | B/C |
| 共享内存 TypedArray | JS ↔ C++ 数据交换 | C |
| C++ → JS 回调桩 | 原生回调到 JS | C |
| `patch_cc_X(...)` | 装饰器元数据补丁 | B/C |

---

## 7. 脚本桥批量调用

文件：`cocos/core/scripting/batch-executor.ts`（196 行）

### 7.1 注册与注销

```ts
registerScriptInstance(jsComp, compId)    // 注册组件到 Map<number, unknown>
unregisterScriptInstance(compId)           // 移除组件
```

### 7.2 批量调用

```ts
scriptBridgeBatchCall(compIds, method, dt?)
```

C++ 端传入一组组件 ID + 方法名（如 `'start'`、`'update'`、`'lateUpdate'`），JS 端遍历并逐个调用。

### 7.3 资源引用收集

```ts
scriptBridgeCollectAssetRefs(compId)
```

递归收集组件序列化属性中的资源引用（通过 `__values__` 数组和 `serializable` 属性），用于 C++ 端的资源管理。

---

## 8. 类型声明体系

### 8.1 手写类型（`@types/jsb.d.ts`）

```
declare namespace jsb {
    // 数学 POD 类型（继承 NativePOD）
    Color, Vec2, Vec3, Vec4, Quat, Mat3, Mat4

    // 高级原生类
    AssetsManager, Manifest, EventAssetsManager
    AssetRefManager, ScriptBridge, BinaryDeserializer

    // 子命名空间
    device: { getDevicePixelRatio, getNetworkType, getBatteryLevel, ... }
    AudioEngine: { play, pause, setVolume, ... }
}
declare namespace ns { Line, Plane, Ray, Sphere, AABB, ... }
```

### 8.2 自动生成类型（`native/tools/tojs/gen_d_ts/`）

- 读取 `native/cocos/bindings/auto/` 中的 JSON 元数据
- 映射 C++ 类型到 TS 类型（如 `cc::Vec2` → `jsb.Vec2`）
- 输出 `jsb.auto.d.ts`，包含 `gfx`、`cc`、`jsb`、`nr` 命名空间

### 8.3 index.ts 中的 native 命名空间类型

`cocos/native-binding/index.ts` 中 `export declare namespace native` 声明了公共 API 类型：
- 顶层函数：`copyTextToClipboard`、`garbageCollect`
- 类：`Downloader`、`AssetsManager`、`Manifest`、`EventAssetsManager`、`DebugRenderer`
- 命名空间：`zipUtils`、`fileUtils`、`reflection`、`bridge`、`jsbBridgeWrapper`

---

## 9. TS 绑定入口总结图

```
┌─────────────────────────────────────────────────────────┐
│                    C++ 原生运行时                         │
│  在 JS 启动前注入 globalThis.jsb / globalThis.gfx 等     │
└──────────────┬──────────────────────┬───────────────────┘
               │                      │
               ▼                      ▼
┌──────────────────────┐   ┌──────────────────────────┐
│ cocos/native-binding │   │ cocos/*/index.jsb.ts     │
│                      │   │ (各模块的 JSB 覆盖文件)    │
│ impl.ts              │   │                          │
│  ├─ jsb.reflection   │   │ 从 jsb/gfx 全局取原生类   │
│  ├─ jsb.bridge       │   │ ├─ prototype 扩展         │
│  ├─ jsbBridgeWrapper │   │ ├─ _ctor 初始化           │
│  └─ installBatchCall │   │ ├─ 共享内存 TypedArray    │
│                      │   │ ├─ C++→JS 回调桩          │
│ index.ts             │   │ └─ patch_cc_X() 装饰器    │
│  ├─ export native    │   │                          │
│  └─ declare 类型     │   └──────────────────────────┘
│                      │
│ decorators.ts        │
│  └─ 56个 patch_*     │
│     (自动生成)        │
└──────────────────────┘
         │
         ▼
┌──────────────────────┐
│ cc.config.json       │
│  ├─ internal:native  │
│  │   → impl.ts       │
│  └─ NATIVE 覆盖      │
│     .ts → .jsb.ts    │
└──────────────────────┘
```

---

## 10. 常见问题定位

1. **TS 编译报错找不到 `internal:native`**：检查 `@types/globals.d.ts` 是否有 `declare module 'internal:native' {}`。
2. **运行时 jsb 对象为空**：确认在原生平台运行（`NATIVE` 宏为 true），C++ 引擎是否正确注入了 `globalThis.jsb`。
3. **装饰器/序列化失效**：检查对应的 `.jsb.ts` 文件是否调用了 `patch_cc_X()`。
4. **模块覆盖未生效**：检查 `cc.config.json` 中 NATIVE 条件覆盖是否正确配置。
5. **JS↔C++ 数据不同步**：对 Node 类，检查共享内存 TypedArray 偏移量是否正确。

---

状态：已完成（S4-T1）
