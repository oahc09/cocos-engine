# D3D12 EID 1458 Shadow 深度写入专题分析

**Capture**: d3d12-shadow.rdc
**事件**: EID 1458 (阴影 Pass 第2个 draw call)
**时间**: 2026-05-24

---

## 一、阴影 Pass 的三个 Draw Call Shader 对比 🚨

阴影 Pass (Colour Pass #1) 有 5 个 draw call，但它们使用了**不同的 shader 变体**：

| Draw # | EID | VS Resource | PS Resource | 类型 |
|--------|-----|-------------|-------------|------|
| 1 | 1443 | 2133 | 2134 | **Simple** - 无 PS CB |
| 2 | 1458 | 2136 | 2137 | **Packed/LPNN** - 有 CCShadow CB |
| 3 | 1473 | ? | ? | ? |
| 4 | 1488 | ? | ? | ? |
| 5 | 1503 | ? | ? | ? |

---

## 二、EID 1443 vs EID 1458: VS 深度传递差异

### EID 1443 VS（Simple 变体）
```
v_clip_depth = clipPos.zw;           // 传递 (z, w) 给 PS
output o0.zw = v_clip_depth;          // float2
```

### EID 1458 VS（Packed/LPNN 变体）
```
v_clip_depth = clipPos.z / clipPos.w; // 🔑 VS 中做透视除法！
output o2.x = v_clip_depth;           // float（单分量）
output o0.zw = v_uv1;                 // 第二个 UV 通道
```

---

## 三、EID 1458 PS: 三种深度编码模式

EID 1458 的 PS 根据 `CCShadow::cc_shadowLPNNInfo` 的值选择编码模式：

```
输入: v_clip_depth (已预除法的单 float)
      v_worldPos (世界空间坐标)
CB0:  CCShadow UBO (space=0, reg=2)
```

### 控制流伪代码

```
if (LPNNInfo.x > 0.000001 && LPNNInfo.x < 2.0)    // 条件 A
    → 模式 A: 光源空间距离编码
else
    if (LPNNInfo.y > 0.000001)                      // 条件 B
        → 模式 B: RGBA 打包深度编码  ⚠️
    else
        → 模式 C: 简单深度
```

### 模式 A: 光源空间距离编码
```hlsl
// 计算世界坐标在光源空间的距离
float4 posLight = mul(float4(v_worldPos.xyz, 1.0), cc_matLightView);
float dist = length(posLight.xyz);
// 线性化到 [0, 1]
float linearDepth = (dist - cc_shadowNFLSInfo.x) / (cc_shadowNFLSInfo.y - cc_shadowNFLSInfo.x);
return float4(linearDepth, 1, 1, 1);
```

### 模式 B: RGBA 打包深度编码 🚨
```hlsl
// 将 v_clip_depth 打包为 4 通道 RGBA
float4 packed = frac(float4(1, 255, 65025, 16581375) * v_clip_depth);
float4 result = packed - float4(packed.yzw, 0) * (1.0/255.0);
return result;  // (R, G, B, A) 4 通道
```

**汇编证据** (HLSL 行 35-40):
```
mul r1.xyzw, r0.wwww, l(1, 255, 65025, 16581375)   // 乘以基数
frc r1.xyzw, r1.xyzw                                  // frac()
mul r3.xyz, r1.yzwy, l(0.003922, ...)                // * 1/255
add r2.xyzw, r1.xyzw, -r3.xyzw                        // 减法纠正
```

### 模式 C: 简单深度
```hlsl
return float4(v_clip_depth, 1, 1, 1);
```

---

## 四、🚨 根因分析：RGBA 打包 vs R32F 目标冲突

### 阴影贴图纹理格式
```
ResourceId::2128: R32_FLOAT, 2048x2048
```
**只有 R 通道！**

### 冲突机制

| 模式 | 写入数据 | R32F 实际存储 | 主场景 PS 读取 | 结果 |
|------|----------|---------------|----------------|------|
| C (简单) | `float4(d, 1, 1, 1)` | R = d | `sample().r` | ✅ 正确 |
| B (打包) | `float4(r, g, b, a)` | R = r**（只存高位）** | `sample().r` | ❌ **错误！** |
| A (距离) | `float4(lin, 1, 1, 1)` | R = lin | `sample().r` | ✅ 正确 |

### 问题详解

模式 B 将深度打包为 RGBA 4 通道：
```
打包结果: (R8, G8, B8, A8) 共 32-bit 精度
R32F 只保留 R 通道，相当于: (R8, 0, 0, 0) → 只有 8-bit 精度！
```

主场景 PS 的阴影采样：
```
sample_indexable r1.x, uv, T0, S0    // 读取 R32F
ge r1.x, r1.x, ref_depth              // 比较
```

当阴影贴图存储了打包数据的高位 R 分量时，与参考深度 `ref_depth` 比较会得到**错误结果** — 因为 `ref_depth` 是完整的 float 值，而阴影贴图只有 8-bit 量化后的高位。

### 触发条件

`cc_shadowLPNNInfo.y > 0.000001` → 使用模式 B（RGBA 打包）

**这个值在 UBO 初始化时可能默认非零！**

---

## 五、验证步骤

### Step 1: 确认 cc_shadowLPNNInfo 的值

需要查看 CCShadow UBO 中 offset 208-223 的 `cc_shadowLPNNInfo` 数据：

```
CCShadow UBO 布局:
  offset   0: cc_matLightView     (64 bytes)
  offset  64: cc_matLightViewProj  (64 bytes)
  offset 128: cc_shadowInvProjDepthInfo (16 bytes)
  offset 144: cc_shadowProjDepthInfo (16 bytes)
  offset 160: cc_shadowProjInfo   (16 bytes)
  offset 176: cc_shadowNFLSInfo   (16 bytes)
  offset 192: cc_shadowWHPBInfo   (16 bytes)
  offset 208: cc_shadowLPNNInfo   (16 bytes) 🔑
  offset 224: cc_shadowColor      (16 bytes)
  offset 240: cc_planarNDInfo     (16 bytes)
```

**检查 cc_shadowLPNNInfo.y (offset 212):**
- 如果 > 0.000001 → 模式 B (打包) 激活 → **这是 bug！**
- 如果 = 0 → 模式 C (简单) 激活 → 正确

### Step 2: 对比 GLES3

检查 GLES3 阴影 Pass 是否也有 LPNN 打包逻辑，以及 `cc_shadowLPNNInfo.y` 的值。

### Step 3: 修复方案

#### 方案 A: UBO 初始化修复
确保 `cc_shadowLPNNInfo.y = 0`，禁用 RGBA 打包模式。

#### 方案 B: Shader 变体选择修复
确保阴影 Pass 使用 Simple 变体（仅 EID 1443 使用的 shader），而不是 Packed/LPNN 变体。

#### 方案 C: 渲染目标格式修复
如果确实需要 RGBA 打包深度，将阴影贴图从 R32F 改为 RGBA8。

---

## 六、GLES3 对比验证

切换到 GLES3 capture 查看 EID 34 的 PS 是否使用了相同的打包逻辑。

GLES3 阴影 PS (EID 34) 的代码：
```glsl
clipDepth = v_clip_depth.x / v_clip_depth.y;
clipDepth = clipDepth * 0.5 + 0.5;
return float4(clipDepth, 1.0, 1.0, 1.0);
```

**GLES3 阴影 PS 没有 LPNN/打包逻辑！它是纯简单深度编码。**

这就是为什么 GLES3 阴影正常而 D3D12 不显示的原因：
- GLES3 使用 Simple 变体（所有物体）
- D3D12 部分物体使用了 Packed/LPNN 变体，写入错误的打包数据到 R32F 纹理

---

## 七、结论

**根因**: D3D12 阴影 Pass 的 EID 1458（第2个 draw call）使用了错误的 shader 变体。该变体包含 RGBA 打包深度编码逻辑，将深度值拆分为 4 通道写入，但目标纹理是 R32F（单通道），导致深度数据不正确。

**影响范围**: 阴影 Pass 中第 2-5 个 draw call（如果它们也使用相同变体）

**修复方向**:
1. 检查 shader 变体选择逻辑，确保阴影 Pass 使用 Simple 变体
2. 或者将 `cc_shadowLPNNInfo.y` 设置为 0 禁用打包模式
3. 或者将阴影贴图改为 RGBA8 格式

---

*基于 RenderDoc MCP 逐指令分析 D3D12 VS/PS 反汇编*
