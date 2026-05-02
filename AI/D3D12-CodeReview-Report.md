# D3D12 GFX 后端代码审查报告

**审查日期**: 2026-04-30
**审查范围**: `native/cocos/renderer/gfx-d3d12/` 全部 16 个类，共 8653 行
**当前状态**: 12 draw calls 执行中，窗口黑屏

---

## 严重级别定义

| 级别 | 含义 |
|------|------|
| **Critical** | 导致黑屏或崩溃的缺陷，必须立即修复 |
| **Important** | 逻辑错误或潜在问题，应尽快修复 |
| **Minor** | 代码质量/性能优化建议 |

---

## Critical 问题

### C1. 🏆 DescriptorHeapPool 双重 reset — 描述符在渲染前被清零

**文件**: `D3D12Device.cpp:506-512` + `D3D12CommandBuffer.cpp:198-204`

**描述**: 每帧 GPU descriptor heap 被 reset 了**两次**：
1. **第一次** — `CommandBuffer::begin()` (行198-204): 重置 heap pool
2. **第二次** — `Device::present()` (行506-512): 再次重置 heap pool

```
时序:
Frame N:
  begin()           → heapPool->reset()  ← 第一次 reset，清零描述符
  renderPass()      → flushDescriptorSets() → 分配新描述符，CopyDescriptorsSimple
  draw()            → SetDescriptorHeaps + SetRootDescriptorTable  ← 描述符有效
  end()
  Queue::submit()   → ExecuteCommandLists + fence wait
  Device::present() → heapPool->reset()  ← 第二次 reset，但此时 GPU 已完成，影响的是下一帧
  swapchain->present()
```

**影响**: 第二次 reset 在 `present()` 末尾（行506-512），实际上清零的是**下一帧**开始时 `begin()` 要分配的描述符的堆状态。虽然 `begin()` 随后也会 reset，但这两次 reset 之间存在一个窗口期——如果渲染管线在 `present()` 的 reset 之后、`begin()` 的 reset 之前尝试分配描述符（理论上不应该发生，因为这是同步的），就会导致问题。

**但更关键的是**：`begin()` 中的 reset 发生在 GPU 执行**前一帧**命令之后、录制**当前帧**命令之前。由于 Queue::submit 是同步的（fence wait），这个时序是安全的。**真正的问题可能在下面 C2。**

**严重程度**: Critical（需要确认描述符在 draw call 时是否真正有效）

---

### C2. 🏆 DescriptorHeapPool reset 清空 GPU 描述符 — draw call 时描述符已被回收

**文件**: `D3D12CommandBuffer.cpp:198-204` + `D3D12DescriptorHeapPool.cpp:250-254`

**描述**: `DescriptorHeapPool::reset()` 将所有 heap 的 `usedCount` 重置为 0 并清空 free list。这意味着**前一帧写入 GPU heap 的描述符数据被标记为可覆盖**。虽然 GPU descriptor heap 的物理内存不会被清零（reset 只改计数），但**新的 `allocate()` 调用会复用同一块内存并覆盖旧数据**。

关键问题：如果同一帧中有多个 `begin()/end()` 循环（多个 CommandBuffer），第一个 CB 的 `begin()` reset 会破坏第二个 CB 正在使用的描述符。

**但在当前实现中**，只有一个 CommandBuffer 实例，且 `begin()` 在帧开始时调用一次，所以描述符在同一帧内是安全的。**需要确认引擎是否确实只使用一个 CommandBuffer。**

**严重程度**: Critical（如果多 CB 存在则直接导致黑屏）

---

### C3. 🏆 Framebuffer::getRTVHandle() 对非 swapchain 附件使用了错误的 handle

**文件**: `D3D12Framebuffer.cpp:182-199`

```cpp
DescriptorPair CCD3D12Framebuffer::getRTVHandle(uint32_t index) const {
    if (!_impl) return {};
    if (index >= _impl->rtvHandles.size()) return {};
    
    // ❌ BUG: 检查 _swapchain 而不是检查 index 对应的纹理是否是 swapchain
    if (_swapchain) {
        uintptr_t rtvPtr = _swapchain->getCurrentRTVHandle();
        return {static_cast<uint64_t>(rtvPtr), 0};
    }
    
    const auto &handle = _impl->rtvHandles[index];
    return {static_cast<uint64_t>(handle.ptr), 0};
}
```

**描述**: 如果 Framebuffer 有 `_swapchain` 成员（即任何一个 color attachment 是 swapchain 纹理），那么**所有** RTV handle 查询都会返回 swapchain 的 RTV，包括非 swapchain 的 color attachment。这意味着如果有多个 color attachment，非 swapchain 的 RTV 也会被替换为 swapchain RTV。

**在当前 Cocos 引擎中**，swapchain framebuffer 通常只有一个 color attachment（即 back buffer 本身），所以实际影响有限。但如果引擎使用了 MRT（Multiple Render Targets）并且其中一个 attachment 是 swapchain 纹理，其他 attachment 的 RTV 会错误。

**严重程度**: Critical（设计缺陷，但在当前使用场景下可能不是根因）

---

### C4. 🏆 CommandBuffer::begin() reset CommandAllocator 在 GPU 可能还在执行时

**文件**: `D3D12CommandBuffer.cpp:186-196`

**描述**: `begin()` 调用 `commandAllocator->Reset()`，这要求**所有使用该 allocator 的命令列表都已完成 GPU 执行**。当前实现依赖 Queue::submit 的同步 fence wait 来保证这一点。

但在 `Device::present()` 中，pixel readback 诊断代码（行396-454）使用了**Device 自己的** command allocator（`_impl->commandAllocator`），而不是 CommandBuffer 的。这不是问题。

**真正的风险是**：如果有多个 CommandBuffer 实例共享同一个 Queue 但没有等待对方完成，`begin()` 的 Reset 会失败或导致 GPU 错误。

**严重程度**: Important（当前单 CB 场景安全，但架构脆弱）

---

### C5. 🏆 blitTexture / copyTexture / resolveTexture 全部是空实现

**文件**: `D3D12CommandBuffer.cpp:1096-1119`

**描述**: 三个关键的纹理操作函数都是空实现（no-op）：
- `blitTexture()` — 纹理缩放/拷贝，UI 渲染可能需要
- `copyTexture()` — 纹理到纹理拷贝
- `resolveTexture()` — MSAA resolve

**如果引擎的渲染管线在后处理阶段使用 `copyTexture()` 将离屏渲染结果拷贝到 swapchain back buffer**，那么这个空实现会直接导致黑屏！

**这是黑屏的最可能根因之一**：NativePipeline 可能先渲染到离屏纹理（offscreen render target），然后用 copyTexture/blitTexture resolve 到 swapchain。如果这些操作被静默忽略，最终 swapchain back buffer 始终是空的。

**严重程度**: 🏆 **CRITICAL — 高度怀疑是黑屏根因**

---

### C6. acquire() 是空实现 — 没有真正获取 back buffer

**文件**: `D3D12Device.cpp:322-328`

```cpp
void CCD3D12Device::acquire(Swapchain *const *swapchains, uint32_t count) {
    (void)swapchains;
    (void)count;
    if (_onAcquire) {
        _onAcquire->execute();
    }
}
```

**描述**: `acquire()` 没有调用 `swapChain->GetCurrentBackBufferIndex()` 来更新当前 back buffer 索引。相反，`currentBackBufferIndex` 只在以下时机更新：
1. `createOrResizeSwapchain()` 创建时（行324）
2. `present()` 后（Swapchain.cpp:237）

这意味着 **acquire → render → submit → present 周期中，render 阶段使用的 back buffer 索引可能是上一帧 present 后更新的旧索引**。

在 FLIP_DISCARD + syncInterval=0 模式下：
- `present()` 调用后，`currentBackBufferIndex` 被更新为下一个 buffer
- 下一帧的 render 应该使用这个新索引
- 但如果时序不对，可能渲染到了错误的 back buffer

**严重程度**: Important（可能导致偶发性黑屏或闪烁）

---

## Important 问题

### I1. swapchain 颜色纹理没有独立的 D3D12 资源

**文件**: `D3D12Texture.cpp` (SwapchainTextureInfo 初始化)

**描述**: swapchain 颜色纹理的 `getD3D12ResourceHandle()` 动态返回 `swapchain->getCurrentBackBufferHandle()`。这意味着每次调用都可能返回不同的指针（因为 back buffer 索引在 present 后变化）。如果在录制命令列表时索引变化，会导致渲染到错误的 buffer。

在 `beginRenderPass()` 中，back buffer handle 被缓存到 `_impl->activeSwapchainBackBuffer`，所以同一 render pass 内是安全的。但跨 render pass 时需要确认索引一致性。

### I2. Static 变量污染

**文件**: 多个文件

**描述**: 大量使用 `static` 局部变量做诊断计数器：
- `D3D12CommandBuffer.cpp`: `s_drawCallCount`, `s_seenShaders`, `s_endFrameCount`, `s_rpCount`
- `D3D12Swapchain.cpp`: 文件级 `static FILE *s_file`
- `D3D12Device.cpp`: `s_presentCount`

这些 static 变量在多次创建/销毁后不会重置。如果程序重启但进程不退出，计数器会继续累加，导致诊断日志在"错误"的时机启用/禁用。

### I3. drawDiagLog 文件句柄泄漏

**文件**: `D3D12CommandBuffer.cpp:46-57` (匿名命名空间)

```cpp
void drawDiagLog(const char *fmt, ...) {
    static FILE *s_file = nullptr;
    if (!s_file) {
        s_file = fopen("C:\\temp\\d3d12-render-diag.log", "a");
        if (!s_file) return;
    }
    // ...
}
```

`static FILE *s_file` 永远不会被 fclose()。在进程退出时 OS 会清理，但如果日志系统需要滚动或重新打开文件，当前实现不支持。

### I4. PipelineBarrier swapchain 纹理跳过可能导致状态不一致

**文件**: `D3D12CommandBuffer.cpp:1209`

```cpp
if (d3d12Texture->isSwapchainColorTexture()) continue;
```

**描述**: `pipelineBarrier()` 完全跳过 swapchain 纹理。这意味着如果引擎在 render pass 之外调用 `pipelineBarrier()` 对 swapchain 纹理进行状态转换，该操作会被静默忽略。虽然 swapchain 纹理的状态由 `beginRenderPass/endRenderPass` 管理，但如果有中间状态转换需求（如 copy 操作），会被跳过。

### I5. beginRenderPass 中有变量遮蔽（shadowing）

**文件**: `D3D12CommandBuffer.cpp:314-317`

```cpp
// 在外层已经声明了 static uint32_t s_rpCount (行296)
if (swapchain) {
    static uint32_t s_rpCount = 0;  // ❌ 遮蔽了外层的 s_rpCount
    ++s_rpCount;
```

**描述**: swapchain 检测块内声明了另一个 `static uint32_t s_rpCount`，遮蔽了外层的同名变量。这导致两个独立的计数器，swapchain 诊断日志的编号和实际 render pass 编号不一致。

### I6. QueryPool 类型映射重复且无意义

**文件**: `D3D12CommandBuffer.cpp:1276-1278, 1292-1294`

```cpp
D3D12_QUERY_TYPE queryType = (d3d12Pool->getType() == QueryType::OCCLUSION)
                                 ? D3D12_QUERY_TYPE_OCCLUSION
                                 : D3D12_QUERY_TYPE_OCCLUSION;  // ❌ 两个分支完全相同
```

### I7. Device::present() 中 pixel readback 诊断代码对生产环境不安全

**文件**: `D3D12Device.cpp:351-480`

**描述**: 诊断代码在 `present()` 流程中插入了额外的 GPU 命令（readback copy），这些命令与主渲染命令在同一个 graphics queue 上执行。虽然有 fence wait，但：
1. 每次都创建新的 readback buffer（无缓存复用）
2. 使用 Device 自己的 command allocator/list（与 CommandBuffer 的不同）
3. 在 readback 命令和主 Present 之间没有明确的同步点

---

## Minor 问题

### M1. 所有 Buffer 使用 UPLOAD heap

**文件**: `D3D12Buffer.cpp`

**描述**: 所有 buffer 都使用 `D3D12_HEAP_TYPE_UPLOAD`。对于频繁读取的 uniform buffer，这是正确的（CPU 写入 → GPU 读取）。但对于 vertex/index buffer，使用 DEFAULT heap + upload buffer 中转会更高效。

### M2. 诊断日志使用硬编码路径

**文件**: 多个文件

**描述**: `"C:\\temp\\d3d12-render-diag.log"` 硬编码。应使用可配置的路径或引擎日志系统。

### M3. 大量 `#if defined(_WIN32)` 条件编译

**描述**: 几乎每个函数体都被 `#if defined(_WIN32)` 包裹。这增加了代码复杂度，且与 pImpl 模式的设计意图不一致。建议将所有平台特定代码放在 Impl 中，公共接口层无需条件编译。

### M4. 诊断日志计数器硬编码阈值

**描述**: `s_drawCallCount < 50`, `s_rpCount <= 20`, `s_presentCount <= 5` 等硬编码阈值。应通过环境变量或配置控制。

### M5. setLineWidth/setDepthBound/setStencilWriteMask 是空实现

**描述**: 这些动态状态设置被忽略。虽然 D3D12 确实不支持某些动态状态，但应该在文档中说明，或者提供替代实现（如 PSO 变体）。

---

## 黑屏根因分析

基于代码审查，黑屏的最可能原因按概率排序：

### 1. 🏆 copyTexture/blitTexture 空实现 (概率 80%)

**NativePipeline 的渲染流程可能是**：
```
Render Pass 1: 渲染场景到离屏纹理 (offscreen color texture)
Render Pass 2: 后处理 (tone mapping 等) 从离屏纹理读取，写入 swapchain
或
copyTexture(): 将离屏纹理拷贝到 swapchain back buffer
```

如果引擎使用 `copyTexture()` 来完成最终合成，而该函数是空实现，swapchain back buffer 永远不会被写入，结果就是黑屏。

**验证方法**: 在 `copyTexture()` 中添加日志，确认是否被调用。

### 2. acquire() 未更新 back buffer 索引 (概率 40%)

如果 `currentBackBufferIndex` 在 render 阶段是错误的，所有渲染会写入一个非当前的 back buffer，Present 显示的是另一个（空的）buffer。

### 3. DescriptorHeapPool reset 时序问题 (概率 20%)

如果引擎使用多个 CommandBuffer，第一个 CB 的 begin() reset 会破坏第二个 CB 的描述符。

---

## 建议修复优先级

1. **[立即]** 实现 `copyTexture()` — 至少支持 CopyTextureRegion
2. **[立即]** 在 `copyTexture()` 和 `blitTexture()` 中添加诊断日志
3. **[高优]** 在 `acquire()` 中调用 `GetCurrentBackBufferIndex()`
4. **[高优]** 修复 `getRTVHandle()` 的 swapchain 检查逻辑
5. **[中优]** 移除 `Device::present()` 中的双重 heapPool reset
6. **[中优]** 实现 `blitTexture()` — D3D12 需要使用 compute shader 或自定义渲染 pass
7. **[低优]** 清理 static 变量、文件句柄泄漏
8. **[低优]** 条件编译重构
