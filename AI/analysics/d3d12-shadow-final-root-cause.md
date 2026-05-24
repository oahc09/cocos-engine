# D3D12 Shadow 最终根因分析

**时间**: 2026-05-24
**Capture**: d3d12-shadow.rdc

---

## 一、已确认事实

### 1.1 阴影 Pass 5 个 draw call 使用两种 shader 变体

| Draw | EID | VS ID | PS ID | PS CB | 深度编码 |
|------|-----|-------|-------|-------|----------|
| 1 | 1443 | 2133 | 2134 | 无 | Simple `z/w` |
| 2 | 1458 | 2136 | 2137 | CCShadow | Packed/LPNN |
| 3 | 1473 | 2136 | 2137 | CCShadow | Packed/LPNN |
| 4 | 1488 | 2136 | 2137 | CCShadow | Packed/LPNN |
| 5 | 1503 | 2136 | 2137 | CCShadow | Packed/LPNN |

**4/5 的 draw 使用 Packed/LPNN 变体！**

### 1.2 阴影贴图格式：R32F

ResourceId::2128 = R32_FLOAT (2048x2048)，只有 R 通道。

### 1.3 Packed 变体的三种编码模式

PS (ResourceId::2137) 根据 `cc_shadowLPNNInfo` 选择：

```
if (LPNNInfo.x > 0.000001 && LPNNInfo.x < 2.0)
    → 模式 A: 光源空间距离编码
else if (LPNNInfo.y > 0.000001)  
    → 模式 B: RGBA 打包编码 ⚠️ BUG!
else
    → 模式 C: 简单 float(v_clip_depth, 1, 1, 1)
```

### 1.4 NS 汇编中模式 B 的证据 (HLSL 行 35-40)

```hlsl
// 将深度打包为 4 通道
mul r1.xyzw, r0.wwww, l(1, 255, 65025, 16581375)
frc r1.xyzw, r1.xyzw              // frac()
mul r3.xyz, r1.yzwy, l(1/255)     // 减法纠正
add r2.xyzw, r1.xyzw, -r3.xyzw    // result = packed - yzw/255
```

**写入 R32F 时只有 R 通道被保留，丢失 GBA 精度数据！**

---

## 二、根因链路

```
D3D12 GFX 后端的 getFormatFeatures(R32F)
  → 可能未正确报告 RENDER_TARGET | SAMPLED_TEXTURE
  → supportsR32FloatTexture(device) 返回 FALSE
  → PipelineUBO: packing = 1  (应为 0)
  → UBO: cc_shadowLPNNInfo.y = 1  (应为 0)
  → PS: LPNNInfo.y > 0.000001 → 真 → 模式 B 激活
  → 写入打包 RGBA 到 R32F → 数据不完整
  → 主场景采样 → 错误深度 → 阴影不显示
```

### 2.1 代码追踪

**格式选择** (ShadowFlow.cpp:286):
```cpp
const auto format = supportsR32FloatTexture(device) ? gfx::Format::R32F : gfx::Format::RGBA8;
```

**Shader 宏设置** (ShadowFlow.cpp:138):
```cpp
const int32_t isRGBE = supportsR32FloatTexture(pipeline->getDevice()) ? 0 : 1;
pipeline->setValue("CC_SHADOWMAP_FORMAT", isRGBE);
```

**UBO packing 设置** (PipelineUBO.cpp:223-224,260):
```cpp
const bool hFTexture = supportsR32FloatTexture(device);
const float packing = hFTexture ? 0.0F : 1.0F;
// ...
shadowLPNNInfos = {DIRECTIONAL, packing, shadowNormalBias, levelCount};
```

**Format 查询** (Define.cpp:640):
```cpp
bool supportsR32FloatTexture(const gfx::Device* device) {
    return hasAllFlags(device->getFormatFeatures(gfx::Format::R32F),
                       gfx::FormatFeature::RENDER_TARGET | gfx::FormatFeature::SAMPLED_TEXTURE);
}
```

### 2.2 矛盾点

Capture 中阴影贴图格式是 R32F → `supportsR32FloatTexture` 在 ShadowFlow 中返回 TRUE。

但 shader 中 `cc_shadowLPNNInfo.y > 0.000001` 为真 → packing 为 1 → `supportsR32FloatTexture` 在 PipelineUBO 中返回 FALSE。

**可能原因**: `ShadowFlow` 和 `PipelineUBO` 使用了不同的 device 指针，或调用时机不同导致 capability 查询结果不一致。

---

## 三、修复方案

### 方案 A: 修复 D3D12 GFX 后端的 R32F 格式支持（根本修复）

检查 `native/cocos/renderer/gfx-d3d12/D3D12Device.cpp` 中的 `getFormatFeatures`:
```cpp
// 确保 R32F 返回 RENDER_TARGET | SAMPLED_TEXTURE
case gfx::Format::R32F:
    return gfx::FormatFeature::RENDER_TARGET | gfx::FormatFeature::SAMPLED_TEXTURE;
```

### 方案 B: 确保 packing 值的一致性（软修复）

在 PipelineUBO 中使用与 ShadowFlow 相同的 device 查询方式：
```cpp
// PipelineUBO.cpp:223
- const bool hFTexture = supportsR32FloatTexture(device);
+ // 使用与 ShadowFlow 相同的逻辑
```

### 方案 C: 禁用 UBO 中的 packing（紧急修复）

在 PipelineUBO.cpp 直接强制 packing = 0:
```cpp
const float packing = 0.0F;  // 强制禁用 RGBA 打包
```

---

## 四、验证步骤

1. **确认** `supportsR32FloatTexture` 在 D3D12 下的返回值
2. **查看** `D3D12Device::getFormatFeatures(gfx::Format::R32F)` 的实现
3. **检查** RenderDoc 中 CCShadow UBO offset 212 的实际值（`cc_shadowLPNNInfo.y`）
4. **对比** GLES3 和 D3D12 的 `supportsR32FloatTexture` 返回差异

---

## 五、相关文件

| 文件 | 作用 |
|------|------|
| `native/cocos/renderer/pipeline/Define.cpp:640` | `supportsR32FloatTexture` 实现 |
| `native/cocos/renderer/pipeline/PipelineUBO.cpp:223-261` | packing 值设置 |
| `native/cocos/renderer/pipeline/shadow/ShadowFlow.cpp:138,286,344` | 格式和 shader 宏选择 |
| `native/cocos/renderer/gfx-d3d12/D3D12Device.cpp` | D3D12 GFX 格式查询 |
