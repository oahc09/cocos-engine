# RenderDoc Capture 渲染分析报告

**Capture 文件**: `C:/Users/caosh/Desktop/gles3-shadow.rdc`
**分析时间**: 2026-05-23
**API**: OpenGL (GLES3)
**分析工具**: RenderDoc MCP

---

## 1. 帧概览统计

| 指标 | 数值 |
|------|------|
| 总 Actions | 52 |
| Draw Calls | 29 |
| Dispatches | 0 |
| Clears | 16 |
| Copies | 0 |
| Presents | 1 |
| Markers | 6 |
| 纹理资源数 | 17 |
| Buffer 资源数 | 19 |

---

## 2. 渲染流程分析

### 2.1 整体渲染架构

Capture 采用**多 Pass 阴影渲染架构**，包含 6 个主要渲染阶段：

```
[Colour Pass #1] ──> 阴影贴图渲染 (2048x2048)
        ↓
[Colour Pass #2] ──> 主场景渲染 (800x600)
        ↓
[Copy/Clear Pass #1] ──> 中间清除
        ↓
[Colour Pass #3] ──> 后处理/UI 渲染 (800x600)
        ↓
[Copy/Clear Pass #2] ──> 最终清除
        ↓
[Colour Pass #4] ──> 最终合成 (800x600)
        ↓
[eglSwapBuffers] ──> 呈现
```

### 2.2 各 Pass 详解

#### **Pass #1: 阴影贴图渲染** (Event ID: 186)
- **目标**: 渲染阴影贴图
- **输出纹理**:
  - Color: `ResourceId::33177` (2048x2048, R32_FLOAT, 16MB)
  - Depth: `ResourceId::33178` (2048x2048, D32, 16MB)
- **绘制调用**: 5 个 `glDrawElements`
  - 每个绘制 600 索引
  - 使用 Indexed 绘制
- **清除操作**:
  - `glClearBufferfv()` - 清除颜色
  - `glClear(Depth)` - 清除深度
- **Vertex Shader 常量缓冲区**:
  - `CCLocal` (224B): 世界矩阵、法线矩阵、光照UV参数、阴影偏移
  - `CCShadow` (256B): 光源视图/投影矩阵、阴影参数
  - `Constants` (96B): 平铺偏移、反照率、PBR参数
- **Pixel Shader 常量缓冲区**:
  - `CCCSM` (656B): 级联阴影贴图参数
  - `CCCamera` (592B): 相机矩阵、位置、光照、雾效参数
  - `CCGlobal` (80B): 时间、屏幕尺寸
  - `CCShadow` (256B): 阴影矩阵和参数

#### **Pass #2: 主场景渲染** (Event ID: 187)
- **目标**: 渲染主场景几何体
- **输出纹理**:
  - Color: `ResourceId::1000000000000000279` (800x600, RGBA8, 1.92MB)
  - Depth: `ResourceId::1000000000000000280` (800x600, D32)
- **绘制调用**: 16 个 `glDrawElements`
  - 5 个绘制 600 索引（主要几何体）
  - 1 个绘制 36 索引
  - 5 个绘制 6 索引
  - 3 个绘制 54 索引
- **特点**: 最多的绘制调用，包含场景中的所有物体

#### **Pass #3: 中间渲染** (Event ID: 189)
- **目标**: 可能是后处理效果或 UI 元素
- **输出纹理**: 同 Pass #2 (1000000000000000279)
- **绘制调用**: 5 个 `glDrawElements`
  - 每个绘制 6 索引（可能是全屏四边形或简单几何体）
- **清除操作**: 在 pass 开始前清除深度和模板

#### **Pass #4: 最终合成** (Event ID: 191)
- **目标**: 最终渲染合成
- **输出纹理**: 同 Pass #2
- **绘制调用**: 4 个 `glDrawElements`
  - 3 个绘制 54 索引
  - 1 个绘制 6 索引
- **特点**: 可能是合成最终颜色或添加后处理效果

#### **Copy/Clear Pass #1 和 #2** (Event ID: 188, 190)
- **目标**: 清除颜色和深度缓冲区
- **操作**:
  - `glClearBufferfv()` - 清除颜色
  - `glClear(Depth)` - 清除深度
  - `glInvalidateFramebuffer()` - 使帧缓冲区无效（优化）

---

## 3. 纹理资源分析

### 3.1 关键纹理资源

| Resource ID | 尺寸 | 格式 | 类型 | 大小 | 用途 |
|-------------|------|------|------|------|------|
| 33177 | 2048x2048 | R32_FLOAT | Texture2D | 16MB | 阴影贴图（颜色） |
| 33178 | 2048x2048 | D32 | Texture2D | 16MB | 阴影贴图（深度） |
| 1000000000000000279 | 800x600 | RGBA8_UNORM | Texture2D | 1.92MB | 主渲染目标（颜色） |
| 1000000000000000280 | 800x600 | D32 | Texture2D | 1.92MB | 主渲染目标（深度） |

### 3.2 纹理格式分析

1. **阴影贴图**:
   - 使用 `R32_FLOAT` 存储深度值
   - 高分辨率 (2048x2048) 保证阴影质量
   - 单通道浮点纹理，优化深度存储

2. **主渲染目标**:
   - 使用 `RGBA8_UNORM` 标准颜色格式
   - 分辨率 800x600，匹配窗口尺寸
   - 8-bit 每通道，适合最终显示

---

## 4. Shader 绑定分析

### 4.1 Vertex Shader 常量缓冲区 (Event ID: 34)

| 缓冲区 | 大小 | 关键变量 | 用途 |
|--------|------|----------|------|
| CCLocal | 224B | cc_matWorld, cc_matWorldIT | 局部变换矩阵 |
| CCShadow | 256B | cc_matLightView, cc_matLightViewProj | 阴影变换矩阵 |
| Constants | 96B | tilingOffset, albedo, pbrParams | 材质参数 |

### 4.2 Pixel Shader 常量缓冲区 (Event ID: 34)

| 缓冲区 | 大小 | 关键变量 | 用途 |
|--------|------|----------|------|
| CCCSM | 656B | cc_csmViewDir0-2, cc_matCSMViewProj | 级联阴影贴图 |
| CCCamera | 592B | cc_matView, cc_matProj, cc_cameraPos | 相机参数 |
| CCGlobal | 80B | cc_time, cc_screenSize | 全局参数 |
| CCShadow | 256B | cc_matLightView, cc_shadowProjInfo | 阴影参数 |
| Constants | 96B | tilingOffset, albedo | 材质参数 |

### 4.3 Shader 资源绑定

- **Event ID 34** 的 pipeline state 显示：
  - Vertex Shader: `ResourceId::33180`, entry point: `main`
  - Pixel Shader: `ResourceId::33181`, entry point: `main`
  - 无 SRV/UAV/Sampler 绑定（可能使用了默认绑定或纹理单元）

---

## 5. 绘制调用模式分析

### 5.1 索引数量分布

| 索引数 | 出现次数 | 可能用途 |
|--------|----------|----------|
| 600 | 10 | 主要几何体（人物、建筑等） |
| 54 | 7 | 中等复杂度物体（道具、UI元素） |
| 36 | 1 | 简单物体 |
| 6 | 11 | 简单几何体（四边形、平面） |

### 5.2 绘制调用优化建议

1. **批处理机会**:
   - 11 个绘制调用使用 6 索引（可能是相同的几何体）
   - 可以考虑合并为实例化绘制或批次渲染

2. **Pass 合并**:
   - Pass #3 和 Pass #4 都输出到相同的渲染目标
   - 如果可能，考虑合并这两个 pass 减少状态切换

---

## 6. 性能分析

### 6.1 内存占用

- **阴影贴图**: 32MB (16MB color + 16MB depth)
- **主渲染目标**: ~4MB (1.92MB color + 1.92MB depth)
- **总纹理内存**: ~36MB (仅计算主要纹理)

### 6.2 Draw Call 分布

- **Pass #1** (阴影): 5 draws - 17% 总绘制
- **Pass #2** (主场景): 16 draws - 55% 总绘制
- **Pass #3** (中间): 5 draws - 17% 总绘制
- **Pass #4** (最终): 4 draws - 14% 总绘制

### 6.3 潜在性能瓶颈

1. **大量小绘制调用**:
   - 11 个绘制仅使用 6 索引
   - 建议: 使用实例化渲染或合并批次

2. **多次清除操作**:
   - 16 次清除操作可能过多
   - 建议: 合并 pass 或减少不必要的清除

3. **阴影贴图分辨率**:
   - 2048x2048 可能过高
   - 建议: 根据场景需求调整分辨率

---

## 7. 结论与建议

### 7.1 渲染架构评估

✅ **优点**:
1. 使用级联阴影贴图 (CSM) 实现高质量阴影
2. 多 pass 架构清晰，易于维护和调试
3. 使用 `glInvalidateFramebuffer` 进行优化

⚠️ **改进空间**:
1. **绘制调用批处理**: 合并小绘制调用
2. **Pass 合并**: 减少 render target 切换
3. **阴影贴图优化**: 考虑使用更小的分辨率或 PCF 软阴影

### 7.2 具体优化建议

1. **实例化渲染**:
   ```cpp
   // 对于重复的 6 索引绘制，使用实例化
   glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0, numInstances);
   ```

2. **纹理压缩**:
   - 考虑使用 `RGBA4` 或 `RGB5_A1` 减少内存占用
   - 阴影贴图可考虑 `D16` 或 `D24` 替代 `D32`

3. **渲染状态管理**:
   - 减少 `glClear` 调用次数
   - 使用 `glClearBuffer` 选择性清除

---

## 8. 附录：完整 Draw Call 列表

### Pass #1 (Event ID: 186)
| Event ID | Action ID | 名称 | 索引数 |
|----------|-----------|------|--------|
| 12 | 1 | glClearBufferfv() | - |
| 15 | 2 | glClear(Depth) | - |
| 34 | 3 | glDrawElements() | 600 |
| 40 | 4 | glDrawElements() | 600 |
| 43 | 5 | glDrawElements() | 600 |
| 46 | 6 | glDrawElements() | 600 |
| 49 | 7 | glDrawElements() | 600 |
| 50 | 8 | glInvalidateFramebuffer() | - |

### Pass #2 (Event ID: 187)
| Event ID | Action ID | 名称 | 索引数 |
|----------|-----------|------|--------|
| 60 | 9 | glClear(Depth, Stencil) | - |
| 61 | 10 | glInvalidateFramebuffer() | - |
| 67-117 | 11-25 | glDrawElements() | 600/36/6/54 |

### Pass #3 (Event ID: 189)
| Event ID | Action ID | 名称 | 索引数 |
|----------|-----------|------|--------|
| 139 | 30 | glClear(Depth, Stencil) | - |
| 143-151 | 31-35 | glDrawElements() | 6 |

### Pass #4 (Event ID: 191)
| Event ID | Action ID | 名称 | 索引数 |
|----------|-----------|------|--------|
| 173 | 40 | glClear(Depth, Stencil) | - |
| 177-183 | 41-44 | glDrawElements() | 54/6 |

---

**分析完成** ✅

*本报告由 RenderDoc MCP 工具自动生成，基于 capture 文件 `gles3-shadow.rdc`*
