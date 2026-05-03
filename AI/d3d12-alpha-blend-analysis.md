# D3D12 半透明渲染逻辑排查报告

**日期**: 2026-05-02 (更新 2026-05-03: H1/M1 已修复)
**范围**: `native/cocos/renderer/gfx-d3d12/` + 上层渲染管线
**目的**: 确认 D3D12 后端 alpha blending / 半透明渲染逻辑的正确性

---

## 一、总体结论

| 模块 | 状态 | 严重程度 |
|------|------|----------|
| BlendFactor 枚举映射 | ⚠️ 有已知限制 | 低 |
| BlendState -> PSO 填充 | ✅ 正确 | — |
| BlendConstants (blendColor) 传递 | ✅ **已修复** (2026-05-03) | — |
| Alpha-to-Coverage | ✅ **已修复** (随 M1) | — |
| SampleDesc/MSAA | ✅ **已修复** (2026-05-03) | — |
| DepthWrite (透明物体) | ✅ 由上层正确控制 | — |
| 渲染排序 (back-to-front) | ✅ 上层正确实现 | — |
| BlendOp 映射 | ✅ 完整正确 | — |
| ColorWriteMask 映射 | ✅ 完整正确 | — |

---

## 二、详细分析

### 2.1 BlendFactor 映射 (D3D12PipelineState.cpp:126-145)

**现状**: 所有 15 个 BlendFactor 枚举值都已映射。

**已知限制**:

| Cocos BlendFactor | D3D12_BLEND | 备注 |
|---|---|---|
| `CONSTANT_COLOR` | `BLEND_FACTOR` | ✅ |
| `CONSTANT_ALPHA` | `BLEND_FACTOR` | ⚠️ D3D12 无区分 color/alpha 的常量 |
| `ONE_MINUS_CONSTANT_COLOR` | `INV_BLEND_FACTOR` | ✅ |
| `ONE_MINUS_CONSTANT_ALPHA` | `INV_BLEND_FACTOR` | ⚠️ 同上 |

**分析**: D3D12 的 `BLEND_FACTOR` / `INV_BLEND_FACTOR` 操作的是完整的 RGBA 四元组。D3D12 API 没有类似 Vulkan 的 `VK_BLEND_FACTOR_CONSTANT_COLOR` vs `VK_BLEND_FACTOR_CONSTANT_ALPHA` 的区分。

- **实际影响**: 如果引擎上层使用 `CONSTANT_ALPHA` 并期望只使用 alpha 分量，在 D3D12 上会使用完整的 RGBA 分量。但目前内置 Effect 文件**没有任何一个**使用 `CONSTANT_ALPHA` 或 `CONSTANT_COLOR` 混合因子，所以当前不会出问题。
- **建议**: 如果未来需要支持，可在 `toD3D12Blend` 中添加注释说明此限制。

### 2.2 BlendState -> PSO 填充 (D3D12PipelineState.cpp:327-369)

**结论: ✅ 正确**

```cpp
psoDesc.BlendState.AlphaToCoverageEnable = blend.isA2C ? TRUE : FALSE;
psoDesc.BlendState.IndependentBlendEnable = blend.isIndepend ? TRUE : FALSE;

for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
    auto &rtBlend = psoDesc.BlendState.RenderTarget[i];
    if (i < blend.targets.size()) {
        rtBlend.BlendEnable = target.blend ? TRUE : FALSE;
        rtBlend.SrcBlend = toD3D12Blend(target.blendSrc);      // 颜色源
        rtBlend.DestBlend = toD3D12Blend(target.blendDst);      // 颜色目标
        rtBlend.BlendOp = toD3D12BlendOp(target.blendEq);       // 颜色操作
        rtBlend.SrcBlendAlpha = toD3D12Blend(target.blendSrcAlpha);   // Alpha源
        rtBlend.DestBlendAlpha = toD3D12Blend(target.blendDstAlpha);  // Alpha目标
        rtBlend.BlendOpAlpha = toD3D12BlendOp(target.blendAlphaEq);   // Alpha操作
        rtBlend.RenderTargetWriteMask = toD3D12ColorWriteMask(target.blendColorMask);
    }
}
```

- 颜色通道和 Alpha 通道独立配置 ✅
- 独立混合 (IndependentBlend) 正确 ✅
- ColorWriteMask 正确 ✅
- 超出 targets 数量的 RTV 槽位默认 WriteMask=ALL ✅

### 2.3 ❌ BlendConstants (blendColor) 未自动传递 — 高优先级

**问题**: `BlendState.blendColor` 字段**从未**被自动应用到 `OMSetBlendFactor()`。

**对比**:

| 后端 | blendColor 处理方式 |
|---|---|
| **WebGL** | `bindPipelineState` 时自动调用 `gl.blendColor(bs.blendColor)` ✅ |
| **D3D12** | `bindPipelineState` 时**不调用** `OMSetBlendFactor()` ❌ |

**D3D12 bindPipelineState (line 545-600)**:
```cpp
void CCD3D12CommandBuffer::bindPipelineState(PipelineState *pso) {
    _impl->commandList->SetPipelineState(d3d12PipelineState);
    _impl->commandList->IASetPrimitiveTopology(topology);
    _impl->commandList->SetGraphicsRootSignature(rootSig);
    // ❌ 缺失: OMSetBlendFactor(pso->getBlendState().blendColor)
}
```

**setBlendConstants 方法已存在但无调用者**:
```cpp
void CCD3D12CommandBuffer::setBlendConstants(const Color &constants) {
    _impl->commandList->OMSetBlendFactor(&constants.x);
}
```

搜索整个引擎 TS/JS 代码，**没有任何上层代码调用 `setBlendConstants()`**。

**实际影响**:
- 当前内置 Effect 不使用 `CONSTANT_COLOR` / `CONSTANT_ALPHA` 混合因子，所以**当前不触发此 bug**
- 但如果将来有自定义 Effect 使用 blend factor 常量，D3D12 会使用 D3D12 默认值 `{1,1,1,1}`，而不是预期的颜色

**修复方案**: 在 `bindPipelineState` 中添加:
```cpp
const auto &bs = pso->getBlendState();
float blendFactor[4] = { bs.blendColor.x, bs.blendColor.y, bs.blendColor.z, bs.blendColor.w };
_impl->commandList->OMSetBlendFactor(blendFactor);
```

### 2.4 ⚠️ Alpha-to-Coverage 无效 — 中优先级

**问题**: `AlphaToCoverageEnable` 正确设置，但 `SampleDesc.Count` 硬编码为 1。

```cpp
psoDesc.BlendState.AlphaToCoverageEnable = blend.isA2C ? TRUE : FALSE;  // ✅ 正确设置
// ...
psoDesc.SampleDesc.Count = 1;   // ❌ 硬编码 1
psoDesc.SampleDesc.Quality = 0; // ❌ 硬编码 0
psoDesc.SampleMask = 0xFFFFFFFF;
```

**Alpha-to-coverage 仅在 MSAA (SampleDesc.Count > 1) 时有效**。

**实际影响**:
- 当前内置 Effect **没有任何一个**使用 `isA2C`（搜索全部 `.effect` 文件结果为零）
- 所以当前不会触发问题
- 但如果未来需要 MSAA + alpha-to-coverage，此路径需要修复

**关联问题**: `SampleDesc` 未从 `RenderPass::sampleCount` 获取值，整个 MSAA 管线未连通。

### 2.5 ⚠️ SampleDesc 硬编码 — 中优先级

**问题**: PSO 中 `SampleDesc.Count` 硬编码为 1，未与 RenderPass 的 `sampleCount` 关联。

```cpp
// D3D12RenderPass 存储 sampleCount
// D3D12PipelineState 未读取它
psoDesc.SampleDesc.Count = 1;  // 应该从 renderPass->sampleCount() 获取
```

**实际影响**: D3D12 后端**不支持 MSAA 渲染**。半透明渲染在非 MSAA 场景下不受影响。

### 2.6 上层透明渲染排序 — ✅ 正确

引擎上层对半透明物体的处理是正确的:

1. **分类**: `pass.blendState.targets[0].blend === true` → 透明物体
2. **排序**: 透明队列使用 `back-to-front`（从远到近）排序
3. **深度**: 透明 technique 设置 `depthWrite: false`
4. **混合因子**: `src_alpha / one_minus_src_alpha`（标准 alpha 混合）
5. **渲染顺序**: 先渲染不透明，后渲染透明

### 2.7 BlendOp 映射 — ✅ 完整

```cpp
D3D12_BLEND_OP toD3D12BlendOp(BlendOp op) {
    case BlendOp::ADD:     return D3D12_BLEND_OP_ADD;
    case BlendOp::SUB:     return D3D12_BLEND_OP_SUBTRACT;
    case BlendOp::REV_SUB: return D3D12_BLEND_OP_REV_SUBTRACT;
    case BlendOp::MIN:     return D3D12_BLEND_OP_MIN;
    case BlendOp::MAX:     return D3D12_BLEND_OP_MAX;
}
```
5 种 BlendOp 全部正确映射。

### 2.8 ColorWriteMask 映射 — ✅ 完整

R/G/B/A 四通道逐位映射到 D3D12_COLOR_WRITE_ENABLE，正确。

---

## 三、标准半透明渲染的完整数据流验证

以 `builtin-standard.effect` 的 `transparent` technique 为例：

```
Effect 定义:
  blendState:
    targets[0]:
      blend: true
      blendSrc: src_alpha        (BlendFactor::SRC_ALPHA = 2)
      blendDst: one_minus_src_alpha (BlendFactor::ONE_MINUS_SRC_ALPHA = 4)
      blendDstAlpha: one_minus_src_alpha

    ↓ Pass.fillPipelineInfo()

Pass._bs:
  targets[0].blend = true
  targets[0].blendSrc = SRC_ALPHA
  targets[0].blendDst = ONE_MINUS_SRC_ALPHA

    ↓ PipelineStateManager.getOrCreatePipelineState()

PipelineStateInfo.blendState = pass.blendState

    ↓ device.createPipelineState()

D3D12PipelineState.doInit():
  rtBlend.BlendEnable = TRUE          ✅
  rtBlend.SrcBlend = SRC_ALPHA       ✅
  rtBlend.DestBlend = INV_SRC_ALPHA  ✅
  rtBlend.BlendOp = ADD              ✅
  rtBlend.SrcBlendAlpha = ONE (默认) ⚠️ (effect 未设置 blendSrcAlpha，默认 ONE)
  rtBlend.DestBlendAlpha = INV_SRC_ALPHA ✅
  rtBlend.BlendOpAlpha = ADD         ✅

    ↓ D3D12 GPU 执行

  最终混合公式:
  color_out = src_color * src_alpha + dst_color * (1 - src_alpha)  ✅
  alpha_out = src_alpha * 1 + dst_alpha * (1 - src_alpha)          ✅
```

**结论**: 标准半透明物体的 alpha 混合逻辑在 D3D12 后端是**正确的**。

---

## 四、建议修复清单

### 高优先级 (建议立即修复)

| # | 问题 | 文件 | 修复 |
|---|------|------|------|
| H1 | `blendColor` 未自动传递到 `OMSetBlendFactor` | `D3D12CommandBuffer.cpp:545` | 在 `bindPipelineState` 中添加 `OMSetBlendFactor` 调用 |

### 中优先级 (建议后续修复)

| # | 问题 | 文件 | 修复 |
|---|------|------|------|
| M1 | `SampleDesc.Count` 硬编码为 1 | `D3D12PipelineState.cpp:464` | 从 RenderPass 获取 sampleCount |
| M2 | Alpha-to-Coverage 因 MSAA 未连通而无效 | 同上 | 依赖 M1 |

### 低优先级 (可选)

| # | 问题 | 文件 | 修复 |
|---|------|------|------|
| L1 | `CONSTANT_ALPHA` 映射不精确 | `D3D12PipelineState.cpp:141` | D3D12 API 限制，添加注释说明 |
| L2 | 诊断日志过多 | `D3D12PipelineState.cpp:332-351` | 条件编译或减少输出频率 |

---

## 五、H1 修复代码参考

```cpp
// 在 D3D12CommandBuffer::bindPipelineState 中，SetPipelineState 之后添加:

// Apply blend constants from PSO's BlendState
const auto &bs = pso->getBlendState();
// 检查是否有任何 target 使用了 CONSTANT_COLOR/CONSTANT_ALPHA 混合因子
bool needsBlendConstants = false;
for (size_t i = 0; i < bs.targets.size(); ++i) {
    const auto &t = bs.targets[i];
    if (t.blendSrc == BlendFactor::CONSTANT_COLOR || t.blendSrc == BlendFactor::CONSTANT_ALPHA ||
        t.blendDst == BlendFactor::CONSTANT_COLOR || t.blendDst == BlendFactor::CONSTANT_ALPHA ||
        t.blendSrcAlpha == BlendFactor::CONSTANT_COLOR || t.blendSrcAlpha == BlendFactor::CONSTANT_ALPHA ||
        t.blendDstAlpha == BlendFactor::CONSTANT_COLOR || t.blendDstAlpha == BlendFactor::CONSTANT_ALPHA ||
        t.blendSrc == BlendFactor::ONE_MINUS_CONSTANT_COLOR || t.blendSrc == BlendFactor::ONE_MINUS_CONSTANT_ALPHA ||
        t.blendDst == BlendFactor::ONE_MINUS_CONSTANT_COLOR || t.blendDst == BlendFactor::ONE_MINUS_CONSTANT_ALPHA ||
        t.blendSrcAlpha == BlendFactor::ONE_MINUS_CONSTANT_COLOR || t.blendSrcAlpha == BlendFactor::ONE_MINUS_CONSTANT_ALPHA ||
        t.blendDstAlpha == BlendFactor::ONE_MINUS_CONSTANT_COLOR || t.blendDstAlpha == BlendFactor::ONE_MINUS_CONSTANT_ALPHA) {
        needsBlendConstants = true;
        break;
    }
}
if (needsBlendConstants) {
    float blendFactor[4] = { bs.blendColor.x, bs.blendColor.y, bs.blendColor.z, bs.blendColor.w };
    _impl->commandList->OMSetBlendFactor(blendFactor);
}
```

或更简单的无条件版本（与 WebGL 行为一致）：
```cpp
const auto &bs = pso->getBlendState();
float blendFactor[4] = { bs.blendColor.x, bs.blendColor.y, bs.blendColor.z, bs.blendColor.w };
_impl->commandList->OMSetBlendFactor(blendFactor);
```
