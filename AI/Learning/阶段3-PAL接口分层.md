# 阶段3-PAL接口分层（S3-T1）

> 范围：`pal/` 抽象接口与 `web/native/minigame` 分支，及其与 `platforms/` 的边界关系  
> 目标：明确 PAL 的“契约层-分发层-实现层”分层，并给出可执行的边界判定规则。

---

## 1. 结论先看（S3-T1 收敛）

- PAL 不是“纯转发层”，而是**有契约、有实现、有平台分支**的能力层。  
- 平台选择通过 `cc.config.json` 的 `moduleOverrides` 在构建期完成，不在业务代码里大规模 `if/else` 分发。  
- `platforms/` 主要承担运行时桥接/垫片（例如 `__globalAdapter`、`fsUtils`、`CCWebAssembly` 注入），**不等价于 PAL 主实现**。

---

## 2. PAL 三层结构：契约层 -> 分发层 -> 实现层

## 2.1 契约层（类型接口）

- 位置：`@types/pal/*.d.ts`
- 作用：定义能力契约，供 `cocos/` 上层按统一接口调用。
- 典型文件：
  - `@types/pal/system-info.d.ts`
  - `@types/pal/screen-adapter.d.ts`
  - `@types/pal/input.d.ts`
  - `@types/pal/audio.d.ts`
  - `@types/pal/env.d.ts`
  - `@types/pal/pacer.d.ts`
  - `@types/pal/wasm.d.ts`

## 2.2 分发层（构建期映射）

- 位置：`cc.config.json` 的 `moduleOverrides`
- 作用：把同一抽象模块（如 `pal/system-info`）映射到 `web/native/minigame/runtime` 对应实现。
- 关键映射样例：
  - web：`pal/system-info -> pal/system-info/web/system-info.ts`
  - native：`pal/system-info -> pal/system-info/native/system-info.ts`
  - minigame：`pal/system-info -> pal/system-info/minigame/system-info.ts`

## 2.3 实现层（平台能力落地）

- 位置：`pal/*/(web|native|minigame)` 及 `pal/minigame/*.ts`
- 作用：直接对接 DOM / `jsb` / 小游戏平台 API，完成能力实现。
- 编译链证据：`tsconfig.json` 的 `include` 包含 `pal/**/*.ts` 与 `@types/pal/*`。

---

## 3. PAL 能力模块分层表（模块-职责-关键文件-边界判定）

| 模块 | 职责 | 关键文件 | 边界判定 |
| --- | --- | --- | --- |
| `system-info` | 系统信息、平台能力、生命周期事件（show/hide/close） | `pal/system-info/web/system-info.ts`、`pal/system-info/native/system-info.ts`、`pal/system-info/minigame/system-info.ts` | PAL 直接实现能力；native 明确依赖 `jsb`，minigame 依赖 `pal/minigame` |
| `screen-adapter` | 分辨率、窗口尺寸、方向、安全区与窗口事件 | `pal/screen-adapter/web/screen-adapter.ts`、`pal/screen-adapter/native/screen-adapter.ts`、`pal/screen-adapter/minigame/screen-adapter.ts` | web 侧实现最完整；native/minigame 受宿主限制但仍在 PAL 内实现 |
| `input` | 键鼠触摸/加速度/手柄输入抽象 | `pal/input/web/index.ts`、`pal/input/native/index.ts`、`pal/input/minigame/index.ts` | PAL 并非只转发；各平台输入细节在对应分支实现 |
| `audio` | 播放器加载、播放控制、中断处理、PCM 访问 | `pal/audio/web/player.ts`、`pal/audio/native/player.ts`、`pal/audio/minigame/player.ts` | web/native/minigame 各自实现明显不同，属于 PAL 主实现域 |
| `env` | Canvas 发现、脚本加载、运行时环境接入 | `pal/env/web/env.ts`、`pal/env/native/env.ts`、`pal/env/minigame/env.ts`、`pal/env/runtime/env.ts` | 与宿主环境强耦合，天然属于 PAL 层 |
| `pacer` | 帧驱动、目标帧率控制 | `pal/pacer/pacer-web.ts`、`pal/pacer/pacer-native.ts`、`pal/pacer/pacer-minigame.ts` | native/minigame 直接调用宿主帧率接口 |
| `wasm` | wasm/二进制加载与实例化 | `pal/wasm/wasm-web.ts`、`pal/wasm/wasm-native.ts`、`pal/wasm/wasm-minigame.ts` | minigame 依赖平台注入全局对象（`CCWebAssembly` / `fsUtils`） |
| `minigame` 子平台 | 归一化 wx/tt/qg/ral 等 API | `pal/minigame/wechat.ts`、`pal/minigame/bytedance.ts`、`pal/minigame/runtime.ts`、`pal/minigame/xiaomi.ts` | PAL 内部完成多小游戏平台 API 归一；不是 `platforms/` 直接替代 |

---

## 4. `pal/` 与 `platforms/` 的边界

## 4.1 编译与模块边界

- `tsconfig.json` 的 `include` 包含 `pal/**/*.ts`，无 `platforms/**/*.ts`。  
- `platforms/` 目录以 JS 适配脚本为主（wrapper/builtin/engine），不是 PAL 的 TS 主体实现目录。

## 4.2 运行时桥接边界

`platforms/` 会注入全局桥接对象，供 PAL 某些能力使用：

1. 小游戏统一适配对象：
   - `platforms/minigame/platforms/wechat/wrapper/unify.js` 中设置 `window.__globalAdapter` 并 `cloneMethod(...)`。
2. wasm 全局能力注入：
   - wechat：`wrapper/builtin/index.js` 注入 `global.CCWebAssembly = global.WebAssembly = global.WXWebAssembly`。
   - bytedance：`wrapper/builtin/index.js` 注入 `global.WebAssembly = global.CCWebAssembly = global.TTWebAssembly`。
3. 文件系统能力注入：
   - `platforms/minigame/platforms/wechat/wrapper/fs-utils.js` 将 `window.fsUtils = module.exports = fsUtils`。

## 4.3 PAL 对这些注入的实际依赖

- `pal/wasm/wasm-minigame.ts` 明确注释 `CCWebAssembly` 来源于 `platforms/.../wrapper/builtin/index.js`。  
- 同文件通过 `globalThis.fsUtils.readArrayBuffer(...)` 读取二进制。  
- 结论：`platforms/` 是运行时桥接与宿主垫片；PAL 才是引擎能力抽象与平台实现主体。

---

## 5. 边界判定规则（可直接复用）

1. 若模块以 `pal/*` 虚拟路径导入并受 `moduleOverrides` 选择，默认归入 PAL 体系。  
2. 若文件位于 `@types/pal/*.d.ts`，它定义的是契约，不是平台具体实现。  
3. 若同一能力在 `web/native/minigame` 均有实现文件，说明 PAL 承担主实现职责。  
4. 若仅在 `platforms/**/*.js` 出现，优先判断为运行时垫片/桥接。  
5. 若 PAL 文件直接调用 `jsb.*`、DOM API、`minigame.*`，属于 PAL 实现层。  
6. 若 `platforms` 通过 `window/global` 注入对象（`__globalAdapter`、`fsUtils`、`CCWebAssembly`），属于 PAL 的外部依赖输入，而非 PAL 主体。  
7. 出现“同名能力”时（如音频/输入），优先看 `cc.config.json` 最终映射目标文件再定责。  
8. 排障时先定位到 PAL 具体分支文件（web/native/minigame），再回溯 `platforms` 注入是否缺失。  
9. `checkPalIntegrity` 的存在说明该模块走了接口完整性校验链（如 `system-info/screen-adapter/input/env/pacer/wasm`）。  
10. 某能力即便使用 `platforms` 注入，也不意味着实现转移到 `platforms`，应以主要逻辑所在地判定归属。

---

## 6. S3-T1 首轮排障路径（建议）

1. **能力不生效（跨平台差异）**：先查 `cc.config.json` 的 `moduleOverrides` 映射到哪一支。  
2. **小游戏 wasm/资源读取异常**：查 `pal/wasm/wasm-minigame.ts` 的 `CCWebAssembly` 与 `globalThis.fsUtils`，再查 `platforms/.../builtin/index.js`、`fs-utils.js` 是否注入。  
3. **输入/屏幕行为异常**：优先进 `pal/input/*`、`pal/screen-adapter/*` 的目标分支，再回查 `platforms/.../unify.js` 是否完整桥接。

---

## 7. 证据文件清单（S3-T1）

- `cc.config.json`  
- `tsconfig.json`  
- `pal/integrity-check.ts`  
- `@types/pal/system-info.d.ts`  
- `@types/pal/screen-adapter.d.ts`  
- `@types/pal/input.d.ts`  
- `@types/pal/audio.d.ts`  
- `@types/pal/env.d.ts`  
- `@types/pal/pacer.d.ts`  
- `@types/pal/wasm.d.ts`  
- `pal/system-info/web/system-info.ts`  
- `pal/system-info/native/system-info.ts`  
- `pal/system-info/minigame/system-info.ts`  
- `pal/screen-adapter/web/screen-adapter.ts`  
- `pal/screen-adapter/native/screen-adapter.ts`  
- `pal/screen-adapter/minigame/screen-adapter.ts`  
- `pal/input/web/index.ts`  
- `pal/input/native/index.ts`  
- `pal/input/minigame/index.ts`  
- `pal/audio/web/player.ts`  
- `pal/audio/native/player.ts`  
- `pal/audio/minigame/player.ts`  
- `pal/env/web/env.ts`  
- `pal/env/native/env.ts`  
- `pal/env/minigame/env.ts`  
- `pal/env/runtime/env.ts`  
- `pal/pacer/pacer-web.ts`  
- `pal/pacer/pacer-native.ts`  
- `pal/pacer/pacer-minigame.ts`  
- `pal/wasm/wasm-web.ts`  
- `pal/wasm/wasm-native.ts`  
- `pal/wasm/wasm-minigame.ts`  
- `pal/minigame/wechat.ts`  
- `pal/minigame/bytedance.ts`  
- `pal/minigame/runtime.ts`  
- `pal/minigame/xiaomi.ts`  
- `platforms/minigame/platforms/wechat/wrapper/unify.js`  
- `platforms/minigame/platforms/bytedance/wrapper/unify.js`  
- `platforms/minigame/platforms/wechat/wrapper/builtin/index.js`  
- `platforms/minigame/platforms/bytedance/wrapper/builtin/index.js`  
- `platforms/minigame/platforms/wechat/wrapper/fs-utils.js`

---

状态：已完成（S3-T1）
