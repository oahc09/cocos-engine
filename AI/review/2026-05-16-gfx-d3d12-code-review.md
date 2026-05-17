# D3D12 GFX 后端代码审查报告

**日期**: 2026-05-16
**审查范围**: `native/cocos/renderer/gfx-d3d12/`
**背景**: 排查离屏渲染问题（RT framebuffer 未绑定）
**渲染管线**: ForwardPipeline（传统管线，非 NativePipeline 自定义渲染图）

---

## 概要

共发现问题：**15 个**
- 严重（Critical）：3 个
- 高（High）：4 个
- 中（Medium）：5 个
- 低（Low）：3 个

**核心结论**：D3D12 后端本身不存在阻止离屏 `beginRenderPass` 被调用的 bug。离屏渲染失败的原因在上游的 ForwardPipeline / FrameGraph 层。每帧仅有 1 次 `beginRenderPass`（仅 swapchain），说明管线层从未为离屏相机构建渲染通道。

---

## 严重问题（Critical）

### 问题 1：`Queue::submit` 完全同步阻塞整个帧管线

**文件**：`D3D12Queue.cpp`，第 135-196 行
**严重程度**：严重

`CCD3D12Queue::submit()` 在每次 `ExecuteCommandLists` 之后调用 `WaitForSingleObject`，超时 5 秒。GPU-CPU 管线完全同步。

ForwardPipeline 的 FrameGraph 为每个渲染通道创建独立的 `DevicePass`。每个 `DevicePass` 执行自己的 `begin()` / `end()` / `Queue::submit()` 循环。如果任何早期 pass 失败或超时，后续 pass（包括离屏渲染）将不会执行。

`CommandBuffer::begin()` 会重置命令分配器，这要求 GPU 空闲。同步等待使得这基本安全，但如果某个命令缓冲区没有被提交（例如离屏 pass 被跳过），分配器重置将使用过时的状态。

---

### 问题 2：`copyBuffersToTexture` 对 Swapchain 纹理使用错误的 StateBefore

**文件**：`D3D12CommandBuffer.cpp`，第 1019-1150 行
**严重程度**：严重

`copyBuffersToTexture()` 调用 `d3d12Texture->getCurrentState()` 获取 `StateBefore` 并发出转换屏障。对于 swapchain 颜色纹理，`getCurrentState()` 返回 `D3D12_RESOURCE_STATE_PRESENT`（在 `doInit(SwapchainTextureInfo)` 中设置）。但首帧之后，swapchain 后缓冲区的状态可能已经被之前的渲染通道改变。

`createResource()` 对 swapchain 纹理返回 `false`（第 301 行），因此 `getD3D12ResourceHandle()` 动态返回当前后缓冲区。屏障针对该动态资源发出，但追踪的状态在首帧后已过时。

---

### 问题 3：`endRenderPass` 无条件将深度纹理过渡到 `DEPTH_READ`

**文件**：`D3D12CommandBuffer.cpp`，第 554-571 行
**严重程度**：严重

`endRenderPass()` 总是将深度从 `DEPTH_WRITE` 过渡到 `DEPTH_READ`。如果深度纹理在后续渲染通道中作为深度模板附件使用（需要 `DEPTH_WRITE` 状态），这是不正确的。

虽然 `beginRenderPass` 会将其过渡回来，但无条件过渡增加了不必要的屏障。更重要的是，如果深度纹理在多个 pass 之间共享（在延迟渲染或多遍前向渲染中很常见），状态来回切换可能导致时序问题。

---

## 高优先级问题（High）

### 问题 4：`syncAttachments` 每帧调用 — 纹理资源缺失时 RTV 为空

**文件**：`D3D12CommandBuffer.cpp` 第 274 行 / `D3D12Framebuffer.cpp` 第 186-253 行
**严重程度**：高

`beginRenderPass` 每帧无条件调用 `d3d12Fbo->syncAttachments()`。`syncAttachments()` 在资源指针变化时调用 `CreateRenderTargetView` 和 `CreateDepthStencilView`。

**离屏渲染关键洞察**：如果离屏纹理从未正确调整大小，或 `createResource` 返回 false，则 RTV 在 `doInit` 期间使用空资源创建。`syncAttachments` 会因 `resource` 为空而跳过它。`OMSetRenderTargets` 收到零值句柄，离屏渲染目标永远不会被绑定。

这是 D3D12 层面导致离屏渲染失败最可能的原因。

---

### 问题 5：`getPostTransferTextureState` 对 SAMPLED + COLOR_ATTACHMENT 的优先级错误

**文件**：`D3D12CommandBuffer.cpp`，第 59-71 行
**严重程度**：高

`getPostTransferTextureState` 按优先级检查使用标志：先 SAMPLED，再 DEPTH_STENCIL，最后 COLOR_ATTACHMENT。离屏渲染目标通常同时具有 `SAMPLED` 和 `COLOR_ATTACHMENT` 标志。该函数返回 `PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE` 而非 `RENDER_TARGET`。

由于 `endRenderPass` 无论如何都会过渡到 `PIXEL_SHADER_RESOURCE`，这在实践中可能无害。但优先级顺序在语义上是错误的——COLOR_ATTACHMENT 在传输后应优先使用 RENDER_TARGET 状态。

---

### 问题 6：`D3D12Device::copyBuffersToTexture` 硬编码 `COMMON` 作为 StateBefore

**文件**：`D3D12Device.cpp`，第 433 行
**严重程度**：高

设备层的 `copyBuffersToTexture` 将 `D3D12_RESOURCE_STATE_COMMON` 硬编码为过渡到 `COPY_DEST` 时的 `StateBefore`。这对新创建的纹理（初始状态为 `COMMON`）是正确的，但对之前已在渲染通道中使用过的纹理是错误的。

**具体序列**：
1. 纹理创建于 `COMMON` 状态
2. 在渲染通道中使用 -> 过渡到 `RENDER_TARGET`
3. `endRenderPass` -> 过渡到 `PIXEL_SHADER_RESOURCE`
4. 调用 `Device::copyBuffersToTexture` -> 硬编码屏障假设 `COMMON`，实际状态为 `PIXEL_SHADER_RESOURCE`
5. D3D12 调试层报告屏障不匹配
6. 上传完成后，纹理过渡到传输后状态，但 `_currentState` 仍然记录为 `PIXEL_SHADER_RESOURCE`

这同时导致运行时屏障不匹配和下一个渲染通道的追踪状态过时。

---

### 问题 7：`updateBuffer` 映射时未检查堆类型

**文件**：`D3D12CommandBuffer.cpp`，第 999-1017 行
**严重程度**：高

`updateBuffer` 调用 `resource->Map()` 时未验证堆类型。如果传入在 `D3D12_HEAP_TYPE_DEFAULT`（GPU 端）上创建的缓冲区，`Map` 调用将因 `E_INVALIDARG` 失败。目前所有缓冲区使用 UPLOAD 堆，不会触发此问题，但缺少类型保护。

---

## 中优先级问题（Medium）

### 问题 8：`GetDeviceRemovedReason` 预检代价过高

**文件**：`D3D12Texture.cpp`，第 313-345 行
**严重程度**：中

每次 `createResource` 调用都执行 `GetDeviceRemovedReason()`，该操作代价高昂，在某些驱动上可能导致 GPU 停顿。信息队列转储最多迭代 50 条消息，每条使用 `malloc`/`free`。此逻辑应受调试标志控制，不应在生产代码中执行。

---

### 问题 9：PSO 创建忽略背面模板掩码

**文件**：`D3D12PipelineState.cpp`，第 401-402 行
**严重程度**：中

PSO 使用 `ds.stencilReadMaskFront` 作为 `StencilReadMask`，使用 `ds.stencilWriteMaskFront` 作为 `StencilWriteMask`。D3D12 仅支持一个读写掩码，正面和背面共享。如果引擎设置了不同的正面/背面掩码，背面掩码会被静默忽略。

---

### 问题 10：`DescriptorPair::value` 字段从未使用

**文件**：`D3D12Framebuffer.h`，第 43 行
**严重程度**：中

`DescriptorPair` 结构体的 `value` 字段在 `getRTVHandle()` 和 `getDSVHandle()` 中始终设为 0。这是早期设计的遗留代码，无害但容易产生误导。

---

### 问题 11：空的 DescriptorSetLayout 创建虚拟 CBV 范围

**文件**：`D3D12PipelineLayout.cpp`，第 200-216 行
**严重程度**：中

当 set layout 没有绑定时，代码创建一个虚拟 CBV 范围（在 register 0 处放 1 个描述符）。该虚拟范围占用了该 set 寄存器空间中的 register 0，如果着色器在该空间中使用 register 0，可能会与实际绑定冲突。

---

### 问题 12：`begin()` 重置描述符堆池 — 异步提交下会出问题

**文件**：`D3D12CommandBuffer.cpp`，第 194-201 行
**严重程度**：中

`begin()` 重置 GPU 描述符堆池和采样器池。目前因为 `Queue::submit` 是同步的（再次调用 `begin()` 时 GPU 已空闲），所以是安全的。但如果引擎将来迁移到异步提交，这将销毁 GPU 仍在使用的描述符。

---

### 问题 13：描述符堆池从不回收内存

**文件**：`D3D12DescriptorHeapPool.cpp`，第 238-243 行
**严重程度**：中

`reset()` 仅设置 `usedCount = 0`，从不移除堆。随着时间推移，堆会永久累积。每个 CBV_SRV_UAV 堆有 4096 个描述符，这是应用程序生命周期内的内存泄漏模式。

---

## 低优先级问题（Low）

### 问题 14：冗余的 NOMINMAX 保护

**文件**：多个 .cpp 文件
**严重程度**：低

几乎每个 .cpp 文件都有 `#ifndef NOMINMAX / #define NOMINMAX / #endif`。可以整合到一个统一的预编译头中。

### 问题 15：Swapchain `createRenderTargetViews` 无部分失败处理

**文件**：`D3D12Swapchain.cpp`，第 252-268 行
**严重程度**：低

如果 `GetBuffer` 对某些后缓冲区成功但对其他失败，没有错误处理逻辑。会导致部分状态。

---

## 离屏渲染根因分析

### 诊断证据

1. 每帧仅有 1 次 `beginRenderPass` 调用（swapchain，`swapchain=yes`）
2. 从未出现离屏 framebuffer 的 `beginRenderPass`（`swapchain=no` 从未出现）
3. swapchain pass 的 RTV 句柄有效（非零）
4. 项目使用 ForwardPipeline，而非 NativePipeline

### 结论

**问题不在 D3D12 后端。** D3D12 后端能正确处理传入 `beginRenderPass` 的任何 framebuffer，包括离屏的。问题在于 ForwardPipeline / FrameGraph 层从未为离屏相机调用 `beginRenderPass`。

### 可能的管线层原因

1. **ForwardStage 跳过了离屏相机**：FrameGraph 可能不会为没有 swapchain 的窗口创建 DevicePass
2. **离屏 RenderWindow 未注册**：如果离屏 RenderWindow 不在 `_renderWindows` 中，其相机不会被提取
3. **相机优先级或可见性**：离屏相机可能在到达渲染循环之前被过滤掉了
4. **Framebuffer 纹理未创建**：如果 `Device::createFramebuffer` 被调用时纹理的 D3D12 资源尚不存在，framebuffer 初始化可能静默失败

### 建议的后续步骤

1. 在 `Root::frameMoveProcess` 添加 `[DIAG-ROOT]` 日志，确认离屏相机是否被提取
2. 在 `ForwardPipeline::render()` 添加日志，确认每个相机是否进入渲染循环
3. 在 `ForwardStage::render()` 添加日志，确认 FrameGraph 是否为离屏相机创建了 pass
4. 在 `DevicePass::begin()` 添加日志，确认 `beginRenderPass` 是否以离屏 framebuffer 被调用
