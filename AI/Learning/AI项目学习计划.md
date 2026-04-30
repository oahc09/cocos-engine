# 当前工程目录学习计划（cocos-engine）

> 目标：读懂当前仓库目录与模块关系，形成“可追踪、可答题、可量化”的学习闭环。

## 1. 学习目标（面向本仓库）

- 能清晰说明 TS 引擎层（`cocos/`）与 C++ 原生层（`native/`）的职责边界。
- 能从功能需求快速定位目录：渲染、资源、场景、平台适配、JSB 绑定。
- 能完成至少 2 条 TS -> JSB -> C++ 调用链追踪。
- 能输出标准化 MD 文档并通过阶段试卷考核。

---

## 2. 目录认知地图（学习基线）

### A. 核心源码
- `cocos/`：TypeScript 引擎核心逻辑
- `native/cocos/`：C++ 原生实现（渲染/平台/绑定）
- `pal/`：平台抽象层（web/native/minigame）
- `@types/`：类型声明（含 `pal`、`jsb`、`webGPU`）

### B. 对外与适配
- `exports/`：对外 API 出口
- `platforms/`：平台 adapter
- `templates/`：平台工程模板

### C. 工具与配置
- `scripts/`：构建脚本
- `cc.config.json`：模块/特性配置
- `tsconfig.json`：TS 编译与类型入口
- `package.json`、`native/package.json`：命令入口
- `native/CMakeLists.txt`：原生构建入口

---

## 3. 任务状态规范（统一追踪）

- `未进行`：任务尚未开始
- `进行中`：任务已开始，正在执行
- `已完成`：任务和对应 MD 输出均完成

> 要求：每次学习结束必须更新状态；一个阶段内同时处于 `进行中` 的任务不超过 2 个。

---

## 4. 按阶段任务学习路径（细化版）

## 阶段 1：工程入口与模块边界

- **阶段任务追踪表**

| 任务ID | 任务内容 | 输出MD | 状态 |
| --- | --- | --- | --- |
| S1-T1 | 阅读 `README.zh-CN.md` 与 `docs/contribution/modules.md`，梳理模块术语 | `AI/Learning/阶段1-术语与模块边界.md` | 已完成 |
| S1-T2 | 阅读 `package.json`、`native/package.json`，梳理命令入口 | `AI/Learning/阶段1-命令入口清单.md` | 已完成 |
| S1-T3 | 阅读 `tsconfig.json`、`cc.config.json`，梳理配置作用 | `AI/Learning/阶段1-配置项对照表.md` | 已完成 |
| S1-T4 | 形成顶层目录职责图与构建入口图 | `AI/Learning/阶段1-工程入口与模块边界.md` | 已完成 |

- **答题验收标准（100分）**
  - 单选题 20 分 + 简答题 40 分 + 定位题 40 分
  - **通过线：70 分**

## 阶段 2：TS 主干运行时（`cocos/`）

- **阶段任务追踪表**

| 任务ID | 任务内容 | 输出MD | 状态 |
| --- | --- | --- | --- |
| S2-T1 | 学习 `cocos/core/` 与 `cocos/game/` 生命周期主线 | `AI/Learning/阶段2-核心生命周期.md` | 已完成 |
| S2-T2 | 学习 `cocos/scene-graph/` 与 `cocos/asset/` 主链路 | `AI/Learning/阶段2-场景与资源主链路.md` | 已完成 |
| S2-T3 | 学习 `cocos/gfx/`、`cocos/render-scene/`、`cocos/rendering/` | `AI/Learning/阶段2-渲染主线拆解.md` | 已完成 |
| S2-T4 | 补充 `animation/ui/physics/audio` 定位与边界 | `AI/Learning/阶段2-功能模块定位表.md` | 已完成 |
| S2-T5 | 输出 2 条关键调用链与问题定位样例 | `AI/Learning/阶段2-TS主干运行时.md` | 已完成 |

- **答题验收标准（100分）**
  - 单选题 20 分 + 调用链题 50 分 + 场景分析题 30 分
  - **通过线：75 分**

## 阶段 3：平台抽象与适配（`pal/` + `platforms/`）

- **阶段任务追踪表**

| 任务ID | 任务内容 | 输出MD | 状态 |
| --- | --- | --- | --- |
| S3-T1 | 学习 `pal/` 抽象接口与 web/native/minigame 分支 | `AI/Learning/阶段3-PAL接口分层.md` | 已完成 |
| S3-T2 | 学习 `platforms/native/` 适配结构 | `AI/Learning/阶段3-native-adapter结构.md` | 已完成 |
| S3-T3 | 学习 `platforms/minigame/` 适配结构 | `AI/Learning/阶段3-minigame-adapter结构.md` | 已完成 |
| S3-T4 | 总结 PAL 与 adapter 边界判定规则（>=10条） | `AI/Learning/阶段3-平台抽象与适配.md` | 已完成 |

- **答题验收标准（100分）**
  - 判断题 20 分 + 对照题 40 分 + 设计题 40 分
  - **通过线：75 分**

## 阶段 4：TS-Native 绑定链路（`native-binding` + `bindings`）

- **阶段任务追踪表**

| 任务ID | 任务内容 | 输出MD | 状态 |
| --- | --- | --- | --- |
| S4-T1 | 学习 `cocos/native-binding/` 的 TS 绑定入口 | `AI/Learning/阶段4-TS绑定入口.md` | 未进行 |
| S4-T2 | 学习 `native/cocos/bindings/` 的 C++ 绑定结构 | `AI/Learning/阶段4-C++绑定结构.md` | 未进行 |
| S4-T3 | 追踪 TS -> JSB -> C++ 链路（链路1） | `AI/Learning/阶段4-链路1追踪.md` | 未进行 |
| S4-T4 | 追踪 C++ -> JSB -> TS 链路（链路2） | `AI/Learning/阶段4-链路2追踪.md` | 未进行 |
| S4-T5 | 形成关键符号索引与调试步骤 | `AI/Learning/阶段4-绑定链路追踪.md` | 未进行 |

- **答题验收标准（100分）**
  - 选择题 20 分 + 链路还原题 50 分 + 故障定位题 30 分
  - **通过线：80 分**

## 阶段 5：Native 渲染与构建工程化

- **阶段任务追踪表**

| 任务ID | 任务内容 | 输出MD | 状态 |
| --- | --- | --- | --- |
| S5-T1 | 学习 `native/cocos/core/` 与 `native/cocos/scene/` | `AI/Learning/阶段5-core与scene梳理.md` | 未进行 |
| S5-T2 | 学习 `native/cocos/renderer/` 架构与 `gfx-base` | `AI/Learning/阶段5-renderer架构.md` | 未进行 |
| S5-T3 | 对照 `gles2/gles3/vulkan/metal/d3d12/wgpu` 后端差异 | `AI/Learning/阶段5-多后端对照表.md` | 未进行 |
| S5-T4 | 学习 `native/CMakeLists.txt` 与 `native/cmake/predefine.cmake` | `AI/Learning/阶段5-构建流程与开关.md` | 未进行 |
| S5-T5 | 输出问题定位 checklist 与最终阶段报告 | `AI/Learning/阶段5-Native渲染与构建.md` | 未进行 |

- **答题验收标准（100分）**
  - 选择题 20 分 + 架构题 40 分 + 实战定位题 40 分
  - **通过线：80 分**

---

## 5. 学习过程追踪看板（每次更新）

建议维护文件：`AI/Learning/学习任务进度看板.md`

模板如下：

| 日期 | 阶段 | 任务ID | 当前状态 | 完成度(0-100%) | 产出MD链接 | 阻塞项 | 下一步 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| YYYY-MM-DD | 阶段X | SX-TX | 未进行/进行中/已完成 | 0 | - | - | - |

> 规则：当任务状态改为 `已完成` 时，完成度必须是 100%，且 `产出MD链接` 不可为空。

---

## 6. 学习过程考核机制（接入试卷体系）

- 试卷体系文档：`AI/Learning/学习过程试卷体系.md`
- 每完成一个阶段，必须完成对应试卷并记录得分。
- **阶段成绩低于通过线不得进入下一阶段**。
- 当前执行状态（2026-04-30）：
  - 试卷A：`已完成`（94分，通过）
  - 试题：`AI/Learning/试卷A-工程入口与模块边界.md`
  - 答卷：`AI/Learning/试卷A-答卷-20260430.md`
  - 复盘：`AI/Learning/试卷A-错题复盘-20260430.md`
  - 台账：`AI/Learning/阶段成绩台账.md`
- 最终成绩计算：
  - 阶段 1~5 占比：10% / 20% / 20% / 25% / 25%
  - 总评 \(S=0.1S_1+0.2S_2+0.2S_3+0.25S_4+0.25S_5\)
- 结项门槛：
  - 总评 \(S \ge 78\)
  - 且阶段 4、5 单科不得低于 75

---

## 7. 结项产出（必须为 MD）

- 阶段任务产出文件（按每个任务的 `输出MD` 完整提交）
- `AI/Learning/学习任务进度看板.md`
- `AI/Learning/阶段成绩台账.md`
- `AI/Learning/学习过程总复盘.md`

> 建议：每份阶段总文档末尾固定追加“试卷得分、错题复盘、下一阶段风险项”。