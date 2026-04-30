# 阶段3-minigame-adapter结构（S3-T3）

> 范围：`platforms/minigame/` 公共层与各平台分支（wechat/bytedance/alipay/taobao/taobao-mini-game/xiaomi），以及与 `pal/minigame`、`platforms/runtime` 的关系  
> 目标：厘清小游戏 adapter 的共性骨架、平台差异和边界归属。

---

## 1. 结论先看（S3-T3 收敛）

- `platforms/minigame/` 是 **小游戏运行时适配层**：负责 Web-like 环境注入、平台 API 归一桥接、文件系统注入、引擎侧补丁接入。  
- 目录呈“**公共层 + 平台分支**”结构：公共层复用资产管理/EditBox 等逻辑，平台分支只放差异化实现。  
- `oppo/vivo/huawei` 这类快速游戏平台主要落在 `platforms/runtime` 与 `bin/adapter/runtime`，不在 `platforms/minigame/platforms` 主目录。

---

## 2. 顶层结构

- `platforms/minigame/common/engine/`：公共引擎补丁（`AssetManager.js`、`cache-manager.js`、`Editbox.js`）  
- `platforms/minigame/common/xmldom/`：DOM parser 相关 polyfill  
- `platforms/minigame/openDataContext/`：开放数据域入口  
- `platforms/minigame/platforms/*/wrapper/`：平台分支（`builtin`、`engine`、`unify.js`、`fs-utils.js`）

平台分支现状（源码层）：
- `wechat`、`bytedance`、`alipay`、`taobao`、`taobao-mini-game`、`xiaomi`

---

## 3. 平台分支通用骨架

| 分层 | 作用 | 典型文件 |
| --- | --- | --- |
| `wrapper/builtin/*` | 注入 `window/document/canvas` 等 Web API 垫片 | `.../wrapper/builtin/index.js` |
| `wrapper/unify.js` | 注入 `window.__globalAdapter`，`cloneMethod` 归一平台 API | `.../wrapper/unify.js` |
| `wrapper/fs-utils.js` | 注入 `window.fsUtils` 统一文件能力 | `.../wrapper/fs-utils.js` |
| `wrapper/engine/index.js` | 组装平台引擎补丁（AssetManager/Editbox/Video 等） | `.../wrapper/engine/index.js` |

关键共性证据：
- 各平台 `unify.js` 都以 `window.__globalAdapter = window.__globalAdapter || {}` 起手。  
- 各平台 `fs-utils.js` 以 `window.fsUtils = module.exports = fsUtils` 暴露文件能力。  
- `common/engine/AssetManager.js`、`common/engine/Editbox.js` 会直接消费 `__globalAdapter` 与 `window.fsUtils`。

---

## 4. 典型平台差异点

## 4.1 WeChat / ByteDance（结构相近）

- `wrapper/builtin/index.js` 注入 `GameGlobal.__isAdapterInjected`，并映射 `CCWebAssembly`（`WXWebAssembly`/`TTWebAssembly`）。  
- `wrapper/unify.js` 覆盖面广：`createVideo`、`showKeyboard`、`onMessage`、`loadSubpackage`、`onShow/onHide`、加速度等。  
- wechat 额外有 `wrapper/engine/VideoPlayer.js` 依赖 `__globalAdapter.createVideo`。

## 4.2 Alipay

- `wrapper/unify.js` 在触摸和音频上有更多定制（如 `onCanPlay` 兼容、触摸绑定到 `canvas`）。  
- `wrapper/engine/index.js` 增加 `Label/Console/AudioPlayer` 定制。

## 4.3 Taobao / Taobao-mini-game

- 都有 `__globalAdapter`，但能力支持不完全一致。  
- `taobao` 对键盘接口多处降级为 not supported；`taobao-mini-game` 支持面更完整并含 `loadSubpackage`。  
- 两者在 `builtin/index.js` 里都有 `window/canvas` 注入与定时器桥接。

## 4.4 Xiaomi

- `wrapper/builtin.js` 为单文件打包形式（非 `builtin/` 目录拆分）。  
- `wrapper/unify.js` 对 `showKeyboard`、加速度尺度与方向做平台特化。  
- `wrapper/engine/index.js` 引入 `download-ttf` 与 Editbox 补丁。

---

## 5. 与 runtime 平台线的关系

- `oppo-mini-game` / `vivo-mini-game` / `huawei-quick-game` 适配主要在：  
  - `platforms/runtime/platforms/*`（源码）  
  - `bin/adapter/runtime/*/engine-adapter.js`（产物）
- 因此阶段3的“minigame adapter”应分两类看：
  1. `platforms/minigame`（微信系/字节/支付宝/淘宝/小米）  
  2. `platforms/runtime`（oppo/vivo/huawei 等快速游戏平台）

---

## 6. 与 PAL 的边界判定（minigame 视角）

1. `platforms/minigame` 内检索 `pal/` 导入基本为 0，说明它不直接承担 PAL 模块实现。  
2. `pal/minigame/*.ts` 才是 IMiniGame 契约实现（如 `wechat.ts/bytedance.ts/alipay.ts/taobao.ts/xiaomi.ts`）。  
3. `platforms/minigame` 提供的是 PAL 运行所需的全局桥接输入（`__globalAdapter/fsUtils/CCWebAssembly`）。  
4. `common/engine/*` 更偏引擎运行时补丁，不等同于 PAL 抽象能力定义。  
5. 结论：**PAL 是能力实现层，minigame adapter 是宿主接入与环境底座层。**

---

## 7. 常见排障路径（S3-T3）

1. **小游戏键盘/触摸异常**：先查目标平台 `wrapper/unify.js` 是否桥接对应 API，再看 `common/engine/Editbox.js`。  
2. **资源下载/缓存异常**：先查平台 `wrapper/fs-utils.js`，再查 `common/engine/AssetManager.js` 与 `cache-manager.js`。  
3. **视频播放异常（微信）**：查 `wrapper/engine/VideoPlayer.js` 与 `__globalAdapter.createVideo` 是否存在。  
4. **WASM/二进制加载异常**：查 `wrapper/builtin/index.js` 是否注入 `CCWebAssembly`，再看 PAL wasm 分支。

---

## 8. 证据文件（S3-T3）

- `platforms/minigame/README.md`  
- `platforms/minigame/common/engine/index.js`  
- `platforms/minigame/common/engine/AssetManager.js`  
- `platforms/minigame/common/engine/cache-manager.js`  
- `platforms/minigame/common/engine/Editbox.js`  
- `platforms/minigame/openDataContext/index.js`  
- `platforms/minigame/platforms/wechat/wrapper/builtin/index.js`  
- `platforms/minigame/platforms/wechat/wrapper/unify.js`  
- `platforms/minigame/platforms/wechat/wrapper/fs-utils.js`  
- `platforms/minigame/platforms/wechat/wrapper/engine/index.js`  
- `platforms/minigame/platforms/wechat/wrapper/engine/VideoPlayer.js`  
- `platforms/minigame/platforms/bytedance/wrapper/builtin/index.js`  
- `platforms/minigame/platforms/bytedance/wrapper/unify.js`  
- `platforms/minigame/platforms/bytedance/wrapper/fs-utils.js`  
- `platforms/minigame/platforms/alipay/wrapper/builtin/index.js`  
- `platforms/minigame/platforms/alipay/wrapper/unify.js`  
- `platforms/minigame/platforms/alipay/wrapper/engine/index.js`  
- `platforms/minigame/platforms/taobao/wrapper/builtin/index.js`  
- `platforms/minigame/platforms/taobao/wrapper/unify.js`  
- `platforms/minigame/platforms/taobao-mini-game/wrapper/builtin/index.js`  
- `platforms/minigame/platforms/taobao-mini-game/wrapper/unify.js`  
- `platforms/minigame/platforms/xiaomi/wrapper/builtin.js`  
- `platforms/minigame/platforms/xiaomi/wrapper/unify.js`  
- `platforms/runtime/platforms/oppo-mini-game/engine/index.js`  
- `platforms/runtime/platforms/vivo-mini-game/engine/index.js`  
- `platforms/runtime/platforms/huawei-quick-game/engine/index.js`  
- `bin/adapter/minigame/wechat/web-adapter.js`  
- `bin/adapter/minigame/wechat/engine-adapter.js`  
- `bin/adapter/runtime/oppo-mini-game/engine-adapter.js`

---

状态：已完成（S3-T3）
