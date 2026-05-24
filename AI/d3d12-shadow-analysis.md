# D3D12 Shadow Rendering Analysis Report

## Date: 2026-05-22
## Status: Root Cause Still Unknown — All "Obvious" Hypotheses Eliminated

## Summary

通过 RenderDoc capture (`C:\temp\d3d12_capture_capture.rdc`) 深入分析了 D3D12 后端的阴影渲染管线。经过完整的 shader 反汇编和代码审查，**所有"明显"的假设均被排除**，但阴影仍不工作。

## RenderDoc Capture 结构

| Pass | Marker | EID Range | 内容 |
|------|--------|-----------|------|
| Pass #1 | Colour Pass #1 | 1415-1496 | Shadow Map 生成 (5 draw calls) |
| Pass #2 | Colour Pass #2 | 1497-1921 | 场景渲染 (17+ draw calls) |

- Shadow Map Texture: ResourceId::2826 (2048×2048, R32_FLOAT)
- Shadow Depth Texture: ResourceId::2827 (2048×2048, D24_UNORM_S8_UINT)

## 已排除的假设

### H1: Y-flip — ❌ 排除
- D3D12 `combineSignY = 1`（clipSpaceSignY=+1, screenSpaceSignY=-1）
- Shader 中 `CC_HANDLE_NDC_SAMPLE_FLIP` 正确触发 UV.y 翻转
- Pass #2 PS 指令 106-110: `if (cc_cameraPos.w == 1.0) { UV.y = 1.0 - NDC.y; }`
- 与 Metal/WebGPU 行为一致，翻转逻辑正确

### H2: 深度编码不匹配 — ❌ 排除
- Pass #1 PS: `clipDepth = z/w`（D3D12 不做 Z 重映射，保持 [0,1]）
- Pass #2 PS: `shadowNDCPos.z = z/w`（同样不重映射）
- 两者都在 [0,1] 范围内，`step()` 比较语义正确
- `CC_USE_D3D12` 宏在 `fs.chunk` 和 `shadow-map.chunk` 中正确处理

### H3: Shadow Map 为空 — ❌ 排除
- Pass #1 有 5 个 draw call（EID 1436, 1451, 1466, 1481, 1496），各 600 indices
- Clear RT (EID 1418) 和 Clear DS (EID 1419) 正常执行
- Clear color = (1,1,1,1)，初始深度为 1.0（最远）
- beginRenderPass 正确设置了 viewport 和 scissor

### H4: Viewport/Scissor 问题 — ❌ 排除
- `beginRenderPass` 从 `renderArea` 正确设置 viewport 和 scissor
- `RSSetViewports` 和 `RSSetScissorRects` 调用正确

### H5: 资源状态转换 — ❌ 排除
- Pass #1 `endRenderPass`: shadow map 从 `RENDER_TARGET` → `PIXEL_SHADER_RESOURCE` ✓
- Pass #2 `beginRenderPass`: shadow map 不受影响（RT 是 swapchain）✓
- Depth buffer: `DEPTH_WRITE` → `DEPTH_READ` → `DEPTH_WRITE` 转换正确 ✓

### H6: CB Register 映射 — ❌ 排除
- Shader 编译管线: `hlslBinding.cbv.register_space = set, register_binding = binding` ✓
- PipelineLayout: `BaseShaderRegister = binding, RegisterSpace = setIndex` ✓
- 两者完全匹配

### H7: 描述符绑定时序 — ❌ 排除
- 延迟绑定机制（`flushDescriptorSets`）在 draw 前统一 flush ✓
- CPU heap → GPU heap 复制顺序与 Root Signature range 声明顺序一致 ✓
- Shadow map SRV (ResourceId::2826) 在 Pass #2 PS 的 SRV slot 0 正确绑定 ✓

## 已验证正确的机制

1. **Shadow Map 生成**: Pass #1 VS 计算 `v_clip_depth = clipSpace.zw`，PS 输出 `z/w` 到 R32_FLOAT
2. **Shadow Map 采样**: Pass #2 VS 计算 `v_shadowPos = worldPos * cc_matLightViewProj`
3. **深度比较**: `step(currentDepth, sampledDepth)` = 1.0（亮）when sampled ≥ current
4. **Shadow Bias**: `shadowProjDepthInfo = (M_22, M_32, M_23, M_33)` 正确编码正交投影深度参数
5. **Root Signature**: 两个 Pass 使用不同的 root signature，但 register/space 映射一致
6. **描述符布局**: `forceUpdate()` 按 binding 顺序线性写入 CPU heap，与 D3D12 `OFFSET_APPEND` 语义匹配

## 关键限制

**RenderDoc MCP 的 `GetConstantBuffer` API 不可用**，无法直接验证：
- CCShadow CB 中的 `cc_matLightViewProj` 矩阵实际值
- `shadowProjDepthInfo` 和 `shadowProjInfo` 的实际值
- CCCamera CB 中的 `cc_cameraPos.w` 实际值
- Pass #1 和 Pass #2 的 shadow camera 参数是否一致

## 剩余可能的根因

### 🔴 P0: CB 数据内容不正确（最高优先级）
虽然绑定和映射正确，但 **CB 中实际写入的数据可能有问题**：
- D3D12 后端的 UBO 上传路径是否正确处理了 `setMat4`/`setVec4`？
- CB 的 GPU VA 和大小是否匹配 shader 的期望？
- 是否存在 **UBO 对齐问题**？（D3D12 CBV 要求 256B 对齐）

### 🟡 P1: Shadow Map 纹理采样失败
虽然 SRV 正确绑定，但采样可能返回错误值：
- SRV 的 `Format` 是否正确？（R32_FLOAT → `DXGI_FORMAT_R32_FLOAT`）
- SRV 的 `ViewDimension` 是否正确？（应该是 `TEXTURE2D`）
- Sampler 状态是否正确？（应该使用 clamp + point/linear）

### 🟡 P2: 条件分支错误
PS 中有复杂的 if-else 分支：
- `if (NdotL > 0)` → `if (mainLitDir.w > 0)` → `if (shadowBias.y > 0.0001)` → 采样
- 任何条件不满足都会跳过 shadow 采样，导致 `_1548 = 1.0`（全亮）
- `mainLitDir.w`（阴影开关）是否正确设置？

## 下一步排查建议

1. **添加诊断日志**: 在 `D3D12DescriptorSet::forceUpdate()` 中，当检测到 shadow map SRV (R32_FLOAT, 2048×2048) 时，打印 SRV 描述信息
2. **验证 CBV 数据**: 在 D3D12 后端的 `setMat4`/`setVec4` 中，对 CCShadow UBO 的关键字段添加日志
3. **检查 `cc_mainLitDir.w`**: 这是阴影开关，如果为 0 则所有阴影计算被跳过
4. **检查 `shadowBias.y`**: 如果为 0，则跳过 shadow map 采样
5. **使用新的 RenderDoc capture**: 重新抓帧并在 RenderDoc GUI 中直接查看 CB 值（MCP 不支持）
6. **最小化测试**: 创建一个最简单的 shadow 场景（1个方向光 + 1个物体），减少变量

## Shader 反汇编关键路径

### Pass #1 PS (EID 1436, PS #2832) — Shadow Depth Output
```hlsl
mov r0.xy, v0.zwzz      // v_clip_depth.xy = clipSpace.zw
div r0.x, r0.x, r0.y    // z/w = clip depth [0,1]
mov r0.yzw, l(0,1,1,1)  // (z/w, 1, 1, 1)
mov o0.xyzw, r0.xyzw     // output to R32_FLOAT
```

### Pass #2 PS (EID 1519, PS #2840) — Shadow Sampling
```hlsl
// 指令 61-74: 深度解码 + bias + 重新编码 + NDC 变换
// 指令 106-110: Y-flip (cc_cameraPos.w == 1.0 → UV.y = 1.0 - NDC.y)
// 指令 123-127: Shadow map 采样 + step 比较
sample_indexable r1.x, r1.yzyy, T0.xyzw, S0  // 采样 shadow map
ge r1.x, r1.x, r1.w                            // sampled >= current ?
movc r2.w, r1.x, l(1.0), l(0)                  // 1 = lit, 0 = shadowed
// 指令 128-132: lerp with shadowNFLSInfo.w
// 指令 184: _1548 (shadow factor) 乘以直接光照
```
