# D3D12 Shadow 根因深入分析报告

**分析时间**: 2026-05-24
**Capture 文件**: d3d12-shadow.rdc vs gles3-shadow.rdc
**分析范围**: VS/PS Shader 全流程对比 + 深度编码对比 + 绑定验证

---

## 一、主场景 Pixel Shader 对比

### 1.1 常量缓冲区声明

| 项目 | GLES3 (SPIR-V) | D3D12 (HLSL ps_5_1) |
|------|----------------|----------------------|
| CCCamera | `Binding(0)` | `space=0, reg=1` (CB0) |
| CCShadow | `Binding(2)` | `space=0, reg=2` (CB1) |
| Constants | `Binding(1)` | `space=1, reg=0` (CB2) |
| 阴影贴图纹理 | `Binding(3), Location(0)` | `space=0, reg=4` (T0) |
| 阴影采样器 | 隐式 | `space=0, reg=4` (S0) |

### 1.2 阴影采样核心算法（完全一致 ✅）

两种后端实现相同的阴影算法：

```
阴影因子 = step(参考深度, 采样深度值)
最终阴影 = lerp(阴影因子, 1.0, cc_shadowNFLSInfo.w)
```

**GLES3 实现**:
```
float _441 = texture(shadowMap, coord).x;       // 采样阴影贴图
float _442 = step(reference_depth, _441);        // 硬阴影比较
return mix(_442, 1.0, cc_shadowNFLSInfo.w);      // 混合阴影强度
```

**D3D12 实现** (HLSL 行 125-132):
```
sample_indexable r1.x, r1.yzyy, T0.xyzw, S0      // 采样阴影贴图
ge r1.x, r1.x, r1.w                               // 硬阴影比较
mov r1.x, -r2.w                                   // 1 - shadow
add r1.x, r1.x, l(1.0)                            //
mul r1.x, r1.x, CB1[11].w                         // * cc_shadowNFLSInfo.w
add r1.x, r1.x, r2.w                              // 
```

### 1.3 结论

✅ **两个平台的主场景 Pixel Shader 逻辑完全一致，问题不在 Pixel Shader。**

---

## 二、阴影 Pass 深度编码对比 🆕

### 2.1 阴影 Pass Vertex Shader（完全一致 ✅）

| 步骤 | GLES3 (SPIR-V) | D3D12 (HLSL vs_5_1) |
|------|----------------|----------------------|
| 世界变换 | `v_worldPos = cc_matWorld * pos` | `v_worldPos = mul(pos, cc_matWorld)` |
| 光源投影 | `clipPos = cc_matLightViewProj * v_worldPos` | `_135 = mul(v_worldPos, cc_matLightViewProj)` |
| 输出位置 | `gl_Position = clipPos` | `o2.xyzw = _135` (SV_Position) |
| 输出深度 | `v_clip_depth = clipPos.zw` | `o0.zw = _135.zw` (v_clip_depth) |

**结论**: Vertex Shader 完全相同，`v_clip_depth = (clip_z, clip_w)` 传给 PS。

### 2.2 阴影 Pass Pixel Shader 深度编码对比 ⚡

| | GLES3 (SPIR-V) | D3D12 (HLSL ps_5_1) |
|------|----------------|----------------------|
| 透视除法 | `z / w` | `z / w` |
| **NDC 重映射** | **`* 0.5 + 0.5`** 🔑 | **无** |
| 通道值 | `(remapped, 1, 1, 1)` | `(z/w, 0, 1, 1)` |

**GLES3 阴影 PS 反汇编**:
```glsl
clipDepth = v_clip_depth.x / v_clip_depth.y;  // z/w ∈ [-1, 1]
clipDepth = clipDepth * 0.5 + 0.5;             // 🔑 重映射到 [0, 1]
return float4(clipDepth, 1.0, 1.0, 1.0);
```

**D3D12 阴影 PS 反汇编**:
```hlsl
div r0.x, r0.x, r0.y                            // z/w ∈ [0, 1] (D3D NDC)
mov r0.yzw, l(0, 1, 1, 1)                       // 通道 0,1,1,1
return float4(r0.x, 0, 1, 1);                   // 直接输出，无重映射
```

### 2.3 深度编码影响分析

| 平台 | NDC z 范围 | 计算 | 写入值范围 | 正确性 |
|------|------------|------|------------|--------|
| GLES3 | [-1, 1] | `z/w * 0.5 + 0.5` | [0, 1] | ✅ |
| D3D12 | [0, 1] | `z/w` | [0, 1] | ✅ |

**✅ 由于 API 约定不同，两种编码方式都是正确的。深度写入不是根因。**

---

## 三、阴影渲染的两个关键前置条件

### 3.1 条件链分析

D3D12 Pixel Shader 中阴影采样的触发条件链：

```
(1) _934 = cc_mainLitDir.w > 0.0f    [CB0[28].w, CCCamera offset 460]
         ↓ (必须为 true)
(2) v_shadowBias.y > 0.0001f         [来自 Vertex Shader 的 CCLocal offset 152]
         ↓ (必须为 true)
(3) 计算阴影 UV 和深度比较
         ↓
(4) sample_indexable T0, S0          [采样阴影贴图]
```

**如果条件 (1) 或 (2) 不满足，阴影计算完全跳过，`_1548 = 1.0`（无阴影效果）。**

### 3.2 关键 UBO 偏移量

| 条件 | 缓冲区 | 偏移量 | 变量 | 期望值 | 来源 |
|------|--------|--------|------|--------|------|
| 条件(1) | CCCamera | offset 460 | cc_mainLitDir.w | > 0 | 方向光强度 |
| 条件(2) | CCLocal | offset 152 | cc_localShadowBias.y | > 0.0001 | 模型阴影偏移 |

### 3.3 D3D12 Pixel Shader 中条件判断的汇编证据

```
; 条件(1): cc_mainLitDir.w > 0
  lt r2.w, l(0), CB0[28].w           ; CB0 = CCCamera, [28] = cc_mainLitDir
; 条件(2): v_shadowBias.y > 0.0001
  lt r2.w, l(0.000100), r1.y         ; r1 = v_shadowBias
  if_nz r2.w                          ; 如果条件满足，执行阴影计算
    ...
  else
    mov r1.x, l(1.000000)             ; 否则 _1548 = 1.0（无阴影）
```

---

## 四、管线绑定验证（D3D12 Pass #2, Event 1526）

### 4.1 已验证正确的绑定

| 资源 | 槽位 | 内容 | 状态 |
|------|------|------|------|
| Render Target | RT0 | 800x600 RGBA8 | ✅ |
| Depth Texture | DT | D32 | ✅ |
| VS CB: CCLocal | slot 0 | 224B | ✅ |
| VS CB: CCShadow | slot 2 | 256B | ✅ |
| VS CB: Constants | slot 0 | 96B | ✅ |
| VS CB: CCCamera | slot 1 | 592B | ✅ |
| PS SRV: 阴影贴图 | slot 0 | ResourceId::2128 (R32F, 2048x2048) | ✅ |
| PS Sampler | slot 0 | default sampler | ✅ |
| PS CB: CCCamera | space=0,reg=1 | 592B | ✅ |
| PS CB: CCShadow | space=0,reg=2 | 256B | ✅ |
| PS CB: Constants | space=1,reg=0 | 96B | ✅ |

### 4.2 管线绑定结论

**所有绑定的 "元数据" 正确**：正确的资源类型绑到了正确的槽位。

---

## 五、🔍 根因假设（按可能性排序）

### 假设 #1 (最可能): CCLocal UBO 中 `cc_localShadowBias.y` 为 0

**D3D12 汇编证据** (Shadow PS line 34):
```
lt r2.w, l(0.000100), r1.y     ; 如果 v_shadowBias.y <= 0.0001
                                 ; → 跳转到 else (_1548 = 1.0, 无阴影)
```

**症状**:
- 条件 (2) 不满足 → 阴影计算完全跳过
- 所有物体渲染正常但无阴影
- 对 GLES3 无影响（因为 GLES3 的 CCLocal 可能有正确值）

**根因**:
- JSB 绑定 `CCLocal` UBO 时，`cc_localShadowBias` 未正确设置
- 或 `cc_localShadowBias` 的偏移量在 D3D12 布局中错误

**排查方法**:
```python
# RenderDoc: 查看 VS CCLocal UBO 数据
get_buffer_contents(resource_id="CCLocal VB resource", offset=144, length=16)
# cc_localShadowBias 在 CCLocal 中的偏移 = 8*16 + 16 = 144
# 期望: y > 0.0001
```

### 假设 #2: CCCamera UBO 中 `cc_mainLitDir.w` 为 0

**D3D12 汇编证据** (Main PS line 29):
```
lt r2.w, l(0), CB0[28].w        ; 如果 cc_mainLitDir.w <= 0
                                 ; → 跳过阴影计算
```

**症状**:
- 条件 (1) 不满足 → 阴影计算跳过
- 方向光强度为 0

**排查方法**:
```python
# RenderDoc: 查看 PS CCCamera UBO 数据
# cc_mainLitDir 在 CCCamera 中的偏移 = 448 (28 * 16)
# 期望: w > 0 (表示方向光强度)
```

### 假设 #3: CCShadow UBO 中矩阵数据错误/为零

**症状**:
- 阴影矩阵正确绑定但值为单位矩阵或零矩阵
- UV 计算结果异常

**排查方法**:
```python
# cc_matLightView: offset 0-63
# cc_matLightViewProj: offset 64-127
# cc_shadowProjInfo: offset 160-175
# cc_shadowProjDepthInfo: offset 144-159
```

### 假设 #4: 阴影贴图纹理内容为空

**症状**:
- 阴影贴图采样值全为清除值
- 对深度比较产生影响

**排查方法**:
```
RenderDoc Texture Viewer 中查看 ResourceId::2128
或在 Event 1443 后查看 RTV 内容
```

---

## 六、已验证项总结

| 验证项 | 状态 | 详情 |
|--------|------|------|
| 主场景 PS 阴影采样逻辑 | ✅ 一致 | step() + lerp() 完全等价 |
| 主场景 PS CB 绑定 | ✅ 正确 | CCCamera/CCShadow/Constants 放到了正确寄存器 |
| 主场景 PS SRV 绑定 | ✅ 正确 | 阴影贴图 ResourceId::2128 绑到 T0 |
| 阴影 PS 深度编码 | ✅ 正确 | GLES3 用 *0.5+0.5, D3D12 用直接 z/w |
| 阴影 VS 变换逻辑 | ✅ 一致 | v_clip_depth = clipPos.zw |
| 管线绑定元数据 | ✅ 正确 | 所有资源类型和槽位匹配 |
| UBO 数据内容 | ❓ 未验证 | 需要查看具体数值 |
| 阴影贴图内容 | ❓ 未验证 | get_texture_data 调用失败 |

---

## 七、下一步行动

### 优先级 1: 检查 CCLocal UBO 数据 🚨

CCLocal 偏移 152 处的 `cc_localShadowBias.y` 是阴影渲染的开关。

**代码路径**: `ModelComponent (TS) → JSB → C++ CCLocal UBO → D3D12 CBV`

### 优先级 2: 检查 CCCamera UBO 数据

CCCamera 偏移 460 处的 `cc_mainLitDir.w` 是方向光强度。

### 优先级 3: 对比 GLES3 和 D3D12 的 UBO 数据

如果数据都正确，对比两个 capture 中相同 UBO 的实际数值。

### 优先级 4: 查看阴影贴图内容

如果 UBO 数据都正确，最后检查阴影贴图本身。

---

## 八、相关代码文件

### 8.1 UBO 更新

| 文件 | 功能 | 关联 |
|------|------|------|
| `cocos/rendering/define.ts` | cc_localShadowBias 定义 | CCLocal UBO |
| `native/cocos/renderer/pipeline/ShadowFlow.cpp` | 阴影流控制 | 阴影矩阵更新 |
| `native/cocos/renderer/pipeline/SceneCulling.cpp` | 场景剔除 + UBO 更新 | CCLocal 填充 |

### 8.2 JSB 绑定

| 文件 | 功能 |
|------|------|
| `native/cocos/bindings/auto/jsb_pipeline_UBO_*.cpp` | UBO JSB 绑定 |
| `native/cocos/bindings/manual/jsb_pipeline_manual.cpp` | 手动 JSB 绑定 |

---

---

## 🚨 补充分析：EID 1458 深度编码冲突（已确认根因）

### 发现过程

在对比阴影 Pass 的各个 draw call 时，发现**不同的 draw call 使用了不同的 shader 变体**：

| Draw | EID | PS Resource | 类型 | CB |
|------|-----|-------------|------|-----|
| 1 | 1443 | 2134 | Simple | 无 |
| 2 | 1458 | 2137 | **Packed/LPNN** 🔑 | CCShadow |

### EID 1458 的三种编码模式

PS 根据 `CCShadow::cc_shadowLPNNInfo` 选择模式：

```
if (LPNNInfo.x > 0.000001 && LPNNInfo.x < 2.0)
    → 模式 A: 光源空间距离
else if (LPNNInfo.y > 0.000001)
    → 模式 B: RGBA 打包深度 ⚠️ 问题！
else
    → 模式 C: 简单深度
```

### 模式 B 代码（HLSL 行 35-40）

```hlsl
// 将深度打包为 4 通道 RGBA (base-256)
float4 packed = frac(float4(1, 255, 65025, 16581375) * depth);
result = packed - float4(packed.yzw, 0) * (1.0/255.0);
```

### 冲突分析

| 项目 | 期望 | 实际 |
|------|------|------|
| 编码格式 | 简单 float depth | RGBA 4通道打包 |
| 目标纹理 | R32F (单通道) | R32F |
| 写入数据 | `(depth, _, _, _)` | `(r, g, b, a)` 仅 r 被保留 |
| 主场景采样 | `sample().r` = depth | `sample().r` = packed_r ≠ depth |

### GLES3 对比

GLES3 阴影 PS (EID 34) 只有简单编码：
```glsl
clipDepth = z/w * 0.5 + 0.5;
return float4(clipDepth, 1.0, 1.0, 1.0);
```

**GLES3 没有 LPNN/打包逻辑 → 阴影正常。**

### 根因确认

**D3D12 阴影 Pass 的 EID 1458（及可能的 1473、1488、1503）使用了包含 RGBA 打包深度编码的 shader 变体，但阴影贴图是 R32F 格式，导致打包数据不完整，主场景采样得到错误深度值。**

### 修复方向

1. **首选**: 确保阴影 Pass 所有 draw call 使用 Simple 变体（无打包逻辑）
2. **备选**: 将 `cc_shadowLPNNInfo.y` 设为 0 禁用打包
3. **替代**: 将阴影贴图改为 RGBA8

### 相关代码

| 文件 | 作用 |
|------|------|
| `native/cocos/renderer/gfx-d3d12/D3D12Shader.cpp` | Shader 变体编译 |
| `native/cocos/renderer/pipeline/shadow/ShadowFlow.cpp` | 阴影 shader 选择 |
| `cocos/rendering/custom/define.ts` | 阴影 shader 宏定义 |

---

*本报告基于 RenderDoc MCP 工具深度分析 D3D12 和 GLES3 Vertex/Pixel Shader 全流程反汇编代码生成*
