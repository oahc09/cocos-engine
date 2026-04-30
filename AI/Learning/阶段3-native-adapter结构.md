# 阶段3-native-adapter结构（S3-T2）

> 范围：`platforms/native/` 适配层结构、入口、运行时桥接点，以及与 `pal/` 的边界  
> 目标：明确 native adapter 在引擎启动与宿主能力接入中的职责，避免与 PAL 主实现混淆。

---

## 1. 结论先看（S3-T2 收敛）

- `platforms/native/` 的核心职责是 **JSB 运行时桥接与 Web API 垫片**，不是 PAL 业务能力主实现层。  
- 入口分两段：`builtin/index.js` 先做环境注入，`engine/index.js` 再挂接引擎扩展能力。  
- PAL native 分支会直接消费这些注入能力（如 `jsb.window.__canvas`、`requestAnimationFrame`、`window.resize`）。

---

## 2. 目录与入口结构

## 2.1 顶层结构

- `platforms/native/builtin/`：全局对象、DOM/事件、计时器、输入等运行时垫片  
- `platforms/native/engine/`：资源加载、缓存、gfx、video/webview、physics 等引擎侧桥接  
- `platforms/native/jsbWindow.js`：`globalThis.jsb.window` 命名空间建立  
- `platforms/native/modules.json`：适配模块与入口清单

## 2.2 启动主入口

1. `platforms/native/builtin/index.js`  
   - 加载 `wasm`、`jsbWindow`、`jsb-adapter`、`jsb_input` 等；  
   - 注入 `requestAnimationFrame/cancelAnimationFrame`、`setTimeout/setInterval`；  
   - 暴露 `jsb.fileUtils`、`XMLHttpRequest`、`WebSocket` 等。

2. `platforms/native/engine/index.js`  
   - 加载 `jsb-game/jsb-gfx/jsb-loader/jsb-fs-utils/jsb-cache-manager/...`；  
   - 按条件注入 openharmony/video/physics 等扩展。

3. 模板侧接入证据  
   - `templates/native/index.ejs` 里在 `System.import('cc')` 后 `require('jsb-adapter/engine-adapter.js')`。

---

## 3. 关键桥接能力与职责

| 模块 | 职责 | 关键文件 | 对 PAL/引擎的影响 |
| --- | --- | --- | --- |
| 启动与全局注入 | 注入 `window`/`document`/计时器/RAF | `platforms/native/builtin/index.js` | PAL `pacer-native` 依赖 RAF，PAL `env-native` 依赖 `jsb.window` |
| JSB Window 命名空间 | 建立 `globalThis.jsb.window` | `platforms/native/jsbWindow.js` | PAL `env-native` 读取 `jsb.window.__canvas` |
| DOM/事件垫片 | 事件模型、`window.resize`、`canvas` 事件转发 | `platforms/native/builtin/jsb-adapter/window.js` | PAL `screen-adapter/native` 触发 `window.resize(...)` |
| 输入桥接 | 文本输入、输入事件派发 | `platforms/native/builtin/jsb_input.js` | 引擎 UI 输入链与宿主输入回调对接 |
| 生命周期桥接 | 错误与低内存事件接入 | `platforms/native/engine/jsb-game.js` | `cc.Game.EVENT_LOW_MEMORY` 从 JSB 侧上抛 |
| 资源加载桥接 | downloader/parser 注册与 bundle 下载 | `platforms/native/engine/jsb-loader.js` | 与 assetManager 运行时加载链路对接 |
| 文件系统桥接 | `jsb.fileUtils`/`jsb.Downloader` 封装 | `platforms/native/engine/jsb-fs-utils.js` | 下载、读写、缓存依赖宿主文件系统 |
| 缓存管理 | cache 目录、LRU、持久化列表 | `platforms/native/engine/jsb-cache-manager.js` | 远端资源缓存生命周期管理 |

---

## 4. 与 PAL 的边界判定（native 视角）

1. `platforms/native` 内检索不到 `pal/` 导入，说明它不直接实现 PAL 模块。  
2. `pal/env/native/env.ts` 使用 `jsb.window` 与 `__canvas`，依赖 `platforms/native` 注入。  
3. `pal/pacer/pacer-native.ts` 使用 RAF/`setPreferredFramesPerSecond`，依赖 `builtin/index.js` 的计时器与宿主能力。  
4. `pal/screen-adapter/native/screen-adapter.ts` 调用 `window.resize(...)`，其实现来自 `jsb-adapter/window.js`。  
5. 结论：**platforms/native 提供运行时底座；PAL/native 提供能力抽象实现；两者分工互补。**

---

## 5. 常见排障路径（S3-T2）

1. **native 端帧不驱动/时序异常**：先看 `platforms/native/builtin/index.js` 是否正确注入 RAF/timer。  
2. **屏幕尺寸与方向异常**：先看 `jsb.onResize -> window.resize` 链是否执行，再看 `pal/screen-adapter/native`。  
3. **远端资源下载/缓存异常**：先查 `jsb-loader.js` 的 download/parse 注册，再查 `jsb-fs-utils.js` 与 `jsb-cache-manager.js`。  
4. **输入框/文本输入异常**：查 `jsb_input.js` 事件派发，再看 UI 层消费链。

---

## 6. 证据文件（S3-T2）

- `platforms/native/README.md`  
- `platforms/native/modules.json`  
- `platforms/native/jsbWindow.js`  
- `platforms/native/builtin/index.js`  
- `platforms/native/builtin/jsb_prepare.js`  
- `platforms/native/builtin/jsb_input.js`  
- `platforms/native/builtin/jsb-adapter/window.js`  
- `platforms/native/engine/index.js`  
- `platforms/native/engine/jsb-game.js`  
- `platforms/native/engine/jsb-loader.js`  
- `platforms/native/engine/jsb-fs-utils.js`  
- `platforms/native/engine/jsb-cache-manager.js`  
- `templates/native/index.ejs`  
- `pal/env/native/env.ts`  
- `pal/screen-adapter/native/screen-adapter.ts`  
- `pal/pacer/pacer-native.ts`

---

状态：已完成（S3-T2）
