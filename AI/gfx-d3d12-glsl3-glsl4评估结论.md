# gfx-d3d12 后端 glsl3 / glsl4 Shader 版本切换评估

- 评估日期：2026-08-11
- 评估对象：`native/cocos/renderer/gfx-d3d12` 渲染后端的 shader 源码版本选择
- 结论：**不能直接切换到 glsl3，也不建议切换**。两者不只是版本号差异，而是面向不同图形 API 的两种绑定模型；D3D12 后端的整条编译管线按 glsl4 语义硬编码。

---

## 1. 版本选择逻辑所在位置

| 位置 | 说明 |
| --- | --- |
| `native/cocos/renderer/core/ProgramLib.cpp` `getDeviceShaderVersion()` (L108-L119) | GLES2/WebGL → `glsl1`；GLES3/WebGL2 → `glsl3`；其余（含 D3D12）→ `glsl4` |
| `native/cocos/renderer/core/ProgramLib.cpp` `getProgram()` (L477-L485) | 按设备版本从 EffectAsset 中取对应源码 |
| `native/cocos/renderer/pipeline/deferred/ReflectionComp.cpp` `getAppropriateShaderSource()` (L394-L407) | 内建管线 compute shader 同样按 API 选择，D3D12 落入 glsl4 分支 |

每个 EffectAsset 同时携带 `glsl3` / `glsl4` 两套源码（由编辑器侧 effect compiler 从同一份 `.effect` 生成），运行时按设备挑选。

## 2. glsl4 与 glsl3 的具体差异

以 `ReflectionComp.cpp` (L143-L250) 中同时存在的两套 compute shader 源码为例：

| 维度 | glsl4（D3D12 当前使用） | glsl3（GLES3 用） |
| --- | --- | --- |
| 语言基准 | GLSL 4.50 core（Vulkan 风格） | GLSL 330 / ESSL 3.x 子集 |
| 资源绑定 | 显式 `layout(set = X, binding = Y)` 标注所有 UBO / sampler / image / SSBO | 裸 `uniform sampler2D`、`layout(std140) uniform Block {}`，无 set/binding |
| 绑定点来源 | 源码声明即绑定号，与引擎 DescriptorSetLayout 元数据一一对应 | 由 GLES3 后端运行时通过 GL 反射解析 |
| 高级特性 | 支持 `input_attachment_index`（subpass input）、完整 compute 等 | 不含 subpass input 等 Vulkan 专属语法 |
| 语义微调 | 针对 Vulkan/D3D 裁剪空间约定 | 针对 OpenGL 约定（如深度解包：glsl4 用 `coord.z`，glsl3 用 `2.0 * coord.z - 1.0`） |

## 3. D3D12 后端依赖 glsl4 的四个硬性原因

### 3.1 编译管线硬编码 Vulkan / GLSL 450

`native/cocos/renderer/gfx-d3d12/D3D12Shader.cpp` 的编译流程：GLSL → SPIR-V（glslang）→ HLSL（SPIRV-Cross）→ DXBC（D3DCompile）。

- `buildD3D12ProcessedSource()` (L823-L830) 强制前插 `#version 450`；
- glslang 配置 (L1664-L1672) 为 `EShClientVulkan` + `EShMsgVulkanRules` + Vulkan 1.1 / SPIR-V 1.3。

**Vulkan 规则要求所有描述符资源必须显式声明 binding**，glsl3 源码（无 binding）会直接解析失败。

### 3.2 绑定一致性依赖显式 set/binding

SPIRV-Cross 从 SPIR-V 读取 `DecorationDescriptorSet` / `DecorationBinding` 并映射到 HLSL `register(bN, spaceN)`（D3D12Shader.cpp L1730-L1777），必须与引擎根签名（每个 descriptor set 对应一个 RegisterSpace）精确匹配。glsl3 源码没有这些数字，即使用 glslang auto-map 强行分配，也几乎必然与 DescriptorSetLayout 元数据错位，导致资源绑到错误寄存器。

### 3.3 custom pipeline 只为 glsl4 做绑定改写

`native/cocos/renderer/pipeline/custom/NativeProgramLibrary.cpp` `overwriteShaderProgramBinding()` (L961-L983) 对非 glsl4 直接 early return。

### 3.4 glsl3 按 OpenGL 约定调优

个别计算（深度处理等）与 glsl4 不同，直接喂给 D3D12 可能产生渲染结果差异。

## 4. 若要强行切换所需工作量

1. glslang 切换到 OpenGL/ESSL 解析模式并关闭 Vulkan rules；
2. 实现一套完整的"无 binding 源码 → 反射 → 重映射到引擎描述符布局"的绑定解析路径；
3. 改写 `overwriteShaderProgramBinding` 以支持非 glsl4；
4. 逐 shader 验证 glsl3/glsl4 的语义差异（深度、裁剪空间等）。

工作量相当于重建后端的资源绑定层，而收益基本为零（最终产物同样是 DXBC，性能无差别）。

## 5. 最终结论

维持 glsl4 是正确选择。glsl3 变体仅适用于 GLES3/WebGL2 的 GL 反射绑定模型，与 D3D12 的显式绑定（root signature / register space）模型根本不兼容。

---

## 6. 独立补充评估（2026-08-11 二次评审）

以上结论经源码独立复核后确认成立，但原文档遗漏了 5 个关键技术约束，工作量估算偏乐观，风险评估不充分。以下为补充意见。

### 6.1 文档遗漏的硬约束

#### 6.1.1 CC_USE_D3D12 宏无条件注入

`D3D12Shader.cpp` `buildD3D12ProcessedSource()` (L822-L829) 对所有源码无条件追加 `#define CC_USE_D3D12 1`。effect 源码中存在 `#if !defined(CC_USE_D3D12)` 的条件分支（例如 `editor/assets/effects/legacy/standard.effect` L451 控制阴影深度范围 [0,1] vs [-1,1]）。切换 glsl3 后这些分支会强制走 D3D12 路径，与 GLES3 后端的语义不一致，可能直接导致阴影错位。原文档完全未评估这一层影响。

#### 6.1.2 Legacy Shadow 源码重写

`D3D12Shader.cpp` `rewriteD3D12LegacyShadowClipDepth()` (L565-L595) 在编译前做 5 处字符串替换，移除 OpenGL 风格的 `* 0.5 + 0.5` 深度转换。这些替换依赖 glsl4 源码的精确文本格式，glsl3 源码的写法可能不同，替换会静默失效，导致阴影渲染错误。原文档没有评估这个交互。

#### 6.1.3 DXBC 缓存键包含完整 processedSource

`D3D12Shader.cpp` `makeDXBCCacheKey()` (L831-L848, L1499-L1512) 将整个 `processedSource` 哈希进缓存键。切换 glsl3 后所有现存 DXBC 缓存失效，首次启动需要全量重新编译——在生产环境会造成明显的启动卡顿。原文档的工作量估算未包含缓存迁移策略。

#### 6.1.4 SPIRV-Cross stage 字段硬约束

`D3D12Shader.cpp` L1753 注释明确："MUST match remap lookup key"。`hlslBinding.stage = toSPIRVExecutionModel(stage)` 必须与 `remap_hlsl_resource_binding` 的查找键匹配，否则绑定静默不生效。重写绑定解析路径时必须保留这个语义，原文档未提及。

#### 6.1.5 Fragment Linkage 重编译依赖 glsl4 源码

`D3D12Shader.cpp` L2234-L2238 在 DXBC 安装后保留 fragment 源码，用于 `getFragmentBytecodeForVertexLinkage` 的 VS/PS linkage 修复重编译。这条路径假设源码能被 glslang + VulkanRules 再次解析。切换 glsl3 后 linkage 修复路径也需要重新实现。

### 6.2 工作量估算修正

原文档列出 4 项核心改造，实际还需补充：
- CC_USE_D3D12 宏注入路径的语义验证
- Legacy Shadow 源码重写规则对 glsl3 的适配
- DXBC 磁盘缓存迁移策略
- Fragment linkage 重编译路径重写
- SPIRV-Cross stage 字段语义保留验证

**实际工作量应为原文档估算的 1.5~2 倍。**

### 6.3 未评估的"不切换"收益

D3D12 后端已经围绕 glsl4 建立完整的优化管线：
- shader source 裁剪（`optimizeD3D12ShaderSource`，L597-）
- DXBC 缓存（Shader Cache Session）
- 异步编译调度（D3D12ShaderCompileScheduler）
- DXBC 安装后源码清理（L2237-L2238 `clear + shrink_to_fit`，仅保留 fragment）

切换 glsl3 会失去这些已落地的优化，还要重新实现等价机制。

### 6.4 与当前内存优化任务的关系

当前 gfx-d3d12 内存优化任务中，shader 源码内存已被 D3D12Shader.cpp 的源码清理机制控制（DXBC 安装后 `clear + shrink_to_fit`，仅保留 fragment 源码用于 linkage 修复）。所以"通过 glsl3 减少源码内存"这个潜在动机其实空间有限——切换的收益不抵成本。

### 6.5 修正后结论

| 维度 | 评价 |
| --- | --- |
| 核心结论 | 正确 |
| 证据充分性 | 中等：主链路证据完整，但遗漏 5 个关键技术点 |
| 工作量估算 | 偏乐观，实际应为 1.5~2 倍 |
| 风险评估 | 不充分：未识别 CC_USE_D3D12 宏注入、Shadow 重写、缓存失效三项风险 |
| 对当前内存优化任务的指导意义 | 缺失：未说明 shader 源码内存已被现有机制控制 |

**最终结论**：维持 glsl4 不变。若文档要进入正式决策，需补充上述 5 项遗漏并重新评估工作量。当前状态下作为参考可以，作为决策依据不够完整。
