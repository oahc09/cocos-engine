# D3D12 Shadow 渲染失败根因分析报告（最终版）

**日期**: 2026-05-23  
**问题**: D3D12 后端阴影（Shadow）不显示  
**状态**: 根因已定位，修复已应用  

---

## 1. 问题现象

在自定义 Cocos Creator 引擎的 D3D12 GFX 后端中，Shadow Pass 正常执行（Shadow UBO 数据正确），Camera UBO 数据也正确，但最终渲染结果中阴影不显示。

---

## 2. 排查历程

### 2.1 第一轮：假设排除（H1-H7）+ Camera UBO 悬垂指针修复

系统排除了 7 个假设，最终定位到 `PipelineUBO::updateMultiCameraUBO` resize 分支中 `_cameraBufferView` 只绑定到 `globalDSMgr` 而未绑定 `_pipeline->getDescriptorSet()`。修复后编译通过，但**阴影仍然不显示**。

### 2.2 第二轮：精准诊断 — Camera UBO 数据实际正确

在 D3D12DescriptorSet 的 `forceUpdate` 和 `applyDynamicOffsets` 中添加了精准诊断：

```
[D3D12-FU-CAM] binding=1 type=DYN cbvVA=0xA954000 cbvSize=2304 gfxBufSize=592 gfxBufPtr=000001A381D36700
[D3D12-DYN-CAM] binding=1 dynOff=0 cbvVA=0xA954000 cbvSize=768 bufVA=0xA954000 bufOffset=0 resWidth=2304 first4=(1.000000,0.000000,0.000000,0.000000)
```

**结论**: Camera UBO 数据完全正确（`first4=(1.0,0.0,0.0,0.0)` 是 `cc_matView` 第一行）。之前日志中全零的 "Camera UBO" 是一个无关的 656 字节 buffer。

### 2.3 第三轮：追踪 Shader 编译输出 — 发现真正根因

检查 shader 诊断日志，发现 standard-fs（172K GLSL）编译后只有：
- **3 个 UBO**（UBOGlobal、UBOCamera、model UBO）
- **0 个 sampled_images**
- **没有** UBOShadow、UBOCSM、cc_shadowMap

HLSL 中没有 `register(t4, space0)` 或 `register(b2, space0)` — **Shadow 相关资源在 GLSL→SPIR-V 编译阶段就被 DCE 了！**

追踪 GLSL 源码发现 shadow 代码被 `#if CC_SHADOW_TYPE == CC_SHADOW_MAP` 保护。
检查 pipeline 宏设置发现：

```cpp
// ShadowFlow::activate() 第 88 行
// 0: CC_SHADOW_NONE, 1: CC_SHADOW_PLANAR, 2: CC_SHADOW_MAP
pipeline->setValue("CC_SHADOW_TYPE", 0);  // ← 硬编码为 0！
```

而 `Shadow::activate()` 正确设置了 `CC_SHADOW_TYPE = 2`，但被 `ShadowFlow::activate()` 覆盖回 0。

---

## 3. 根因分析

### 3.1 完整因果链

```
Shadow::activate() → pipeline->setValue("CC_SHADOW_TYPE", 2) ✅
ShadowFlow::activate() → pipeline->setValue("CC_SHADOW_TYPE", 0) ❌ 覆盖！
  ↓
GLSL shader: #if CC_SHADOW_TYPE == CC_SHADOW_MAP → 0 == 2 → false
  ↓
GLSLang 编译: shadow 采样代码 = dead code → 全部剔除
  ↓
SPIR-V: 无 UBOShadow、UBOCSM、cc_shadowMap
  ↓
HLSL: 无 shadow 相关 cbuffer/Texture2D 声明
  ↓
D3D12 层面: descriptor binding 正确，但 shader 不使用这些资源
  ↓
结果: 无阴影渲染
```

### 3.2 根因代码

**文件**: `native/cocos/renderer/pipeline/shadow/ShadowFlow.cpp`  
**函数**: `ShadowFlow::activate()`（第 88、94 行）

```cpp
// 0: CC_SHADOW_NONE, 1: CC_SHADOW_PLANAR, 2: CC_SHADOW_MAP
pipeline->setValue("CC_SHADOW_TYPE", 0);  // 硬编码！覆盖了 Shadow::activate() 设置的 2

// 0: CC_DIR_LIGHT_SHADOW_PLANAR, ...
pipeline->setValue("CC_DIR_LIGHT_SHADOW_TYPE", 0);  // 同样硬编码！
```

### 3.3 为什么 Web 版本不受影响

Web 版本（TypeScript）中：
- `Shadows.activate()` 动态设置 `pipeline.macros.CC_SHADOW_TYPE = 2`
- `ShadowFlow.activate()` 初始化时设为 0，但 `Shadows.activate()` 后续更新
- JS 的调用顺序保证了正确的值最终生效

C++ JSB 版本中：
- `ShadowFlow::activate()` 在 `RenderPipeline::activate()` 中被调用
- `Shadows::activate()` 由 `SceneGlobals.activate()` 触发
- **调用顺序不确定**，且 `ShadowFlow::activate()` 的硬编码 0 会覆盖之前的正确值

---

## 4. 修复方案

### 4.1 修复代码

在 `ShadowFlow::activate()` 中，不再硬编码 `CC_SHADOW_TYPE` 和 `CC_DIR_LIGHT_SHADOW_TYPE` 为 0，改为从场景 shadow 配置读取正确的值：

```cpp
const auto *sceneData = pipeline->getPipelineSceneData();
const auto *shadowInfo = sceneData->getShadows();
if (shadowInfo && shadowInfo->isEnabled()) {
    if (shadowInfo->getType() == scene::ShadowType::SHADOW_MAP) {
        pipeline->setValue("CC_SHADOW_TYPE", 2);
    } else if (shadowInfo->getType() == scene::ShadowType::PLANAR) {
        pipeline->setValue("CC_SHADOW_TYPE", 1);
    }
}
// SHADOW_MAP 模式下默认 CC_DIR_LIGHT_SHADOW_TYPE=1(UNIFORM)
if (shadowInfo && shadowInfo->isEnabled() && shadowInfo->getType() == scene::ShadowType::SHADOW_MAP) {
    pipeline->setValue("CC_DIR_LIGHT_SHADOW_TYPE", 1);
}
```

### 4.2 待确认的同类问题

- `NativePipeline.cpp` 第 1363/1369 行也有硬编码 0，可能需要同样修复

### 4.3 教训

1. **Pipeline 宏值会影响 shader 编译**：CC_SHADOW_TYPE=0 导致 shadow 代码被 DCE，这不是运行时绑定能解决的问题
2. **"数据正确但效果不对"应首先检查 shader**：shadow UBO 数据正确、Camera UBO 数据正确、descriptor binding 正确，但 shader 编译结果没有 shadow 代码
3. **C++ 迁移时不能简单翻译初始化代码**：TS 中的 `pipeline.macros.CC_SHADOW_TYPE = 0` 是初始值，后续会被 `Shadows.activate()` 覆盖；C++ 中 `ShadowFlow::activate()` 可能后执行并覆盖
4. **Shader 编译日志是最直接的证据**：从 SPIR-V 资源列表就能看到 shadow 资源是否被编译器保留

---

## 5. 涉及的关键文件

| 文件 | 角色 |
|------|------|
| `native/cocos/renderer/pipeline/shadow/ShadowFlow.cpp` | **根因文件**，CC_SHADOW_TYPE 硬编码为 0 |
| `native/cocos/scene/Shadow.cpp` | 正确设置 CC_SHADOW_TYPE=2，但被覆盖 |
| `native/cocos/renderer/gfx-d3d12/D3D12Shader.cpp` | Shader 编译管线（GLSL→SPIR-V→HLSL） |
| `native/cocos/renderer/gfx-d3d12/D3D12DescriptorSet.cpp` | forceUpdate/applyDynamicOffsets（数据正确但 shader 不用） |
| `native/cocos/renderer/pipeline/PipelineUBO.cpp` | Camera/Shadow UBO 数据填充（正确） |

---

## 6. 排查方法论总结

1. **假设驱动但保持开放心态**: 第一轮 H1-H7 排除后锁定 Camera UBO，但修复无效后必须重新审视
2. **从结果反向追踪**: 当所有运行时数据都正确时，检查 shader 编译输出
3. **Shader DCE 是隐藏的杀手**: GLSLang 的 dead code elimination 会导致看似正确的 binding 变得无效
4. **对比 TS/C++ 实现**: C++ 迁移时的微妙差异（宏值覆盖顺序）可能导致功能缺失
5. **诊断日志要精准**: binding=1 Camera UBO 的 first4 读取直接证明了数据正确，避免了继续在错误方向深入
