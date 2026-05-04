# D3D12 GFX 后端渲染性能审查报告

**日期**: 2026-05-03  
**范围**: `native/cocos/renderer/gfx-d3d12/` 全部源文件  
**审查人**: AI Code Reviewer  
**基线**: HEAD = 7c5cee638e

---

## 执行摘要

gfx-d3d12 后端已具备完整的功能实现（16个 D3D12 GFX 类、端到端渲染管线已打通），但存在多项影响渲染性能的架构级和实现级问题。按严重程度分为 **Critical（帧率瓶颈）**、**Important（可感知性能损失）**、**Minor（优化建议）** 三级。

### 问题概览

| 级别 | 数量 | 关键问题 |
|------|------|----------|
| Critical | 4 | 同步提交、单帧描述符堆重置、全量 forceUpdate、文件 I/O 诊断泄漏 |
| Important | 6 | 上传堆类型错误、无双缓冲/三缓冲、PSO 无缓存、无 multi-frame 描述符池、ccstd::vector 动态分配、UAV 全局屏障 |
| Minor | 5 | 调试日志未清理、ccstd::vector 栈分配替代、DISCARD 优化缺失、Swapchain 硬编码双缓冲、无 GPU 时间戳查询 |

---

## Critical 级别问题

### C1. Queue::submit 同步等待 — CPU 完全串行化

**文件**: `D3D12Queue.cpp:124-185`

```cpp
void CCD3D12Queue::submit(CommandBuffer *const *cmdBuffs, uint32_t count) {
    // ...
    graphicsQueue->ExecuteCommandLists(...);
    
    // 同步等待 GPU 完成！
    ++_impl->fenceValue;
    graphicsQueue->Signal(_impl->fence.Get(), _impl->fenceValue);
    if (_impl->fence->GetCompletedValue() < _impl->fenceValue) {
        _impl->fence->SetEventOnCompletion(_impl->fenceValue, _impl->fenceEvent);
        WaitForSingleObject(_impl->fenceEvent, 5000);  // CPU 阻塞！
    }
}
```

**影响**: 
- 每次 submit 都会阻塞 CPU 等待 GPU 完成
- 无法实现 CPU/GPU 流水线并行（CPU 生成下一帧命令时，GPU 在执行当前帧）
- 帧率被限制为 `max(GPU渲染时间, CPU命令生成时间)`，而非二者重叠
- 对于有多个 submit 的帧（如 shadow pass → main pass → post-process），每个 submit 都会引入一次 CPU 同步点

**推荐修复**:
- 采用 **双缓冲/三缓冲帧同步** 模型：每帧使用独立的命令分配器和帧围栏，只在帧边界等待
- submit 时仅 Signal 围栏，不等候
- 在 `CommandBuffer::begin()` 时等待上一帧的围栏（表示上一帧 GPU 已完成，可以重用命令分配器）

**预估性能提升**: 30-50% 帧率提升（取决于 CPU/GPU 负载比）

---

### C2. 描述符堆每帧全量 reset — GPU 描述符复用率零

**文件**: `D3D12CommandBuffer.cpp:176-182`

```cpp
void CCD3D12CommandBuffer::begin(...) {
    // 每帧重置 GPU 描述符堆
    if (auto *heapPool = device->getGPUDescriptorHeapPool()) {
        heapPool->reset();  // 所有分配失效！
    }
    if (auto *samplerPool = device->getSamplerDescriptorHeapPool()) {
        samplerPool->reset();
    }
}
```

**影响**:
- 每帧所有描述符分配都是全新分配 → `CopyDescriptorsSimple` 每帧重复拷贝相同数据
- 描述符堆池可能无限增长（只 reset usedCount 不释放 heap 本身）
- 无法利用帧间描述符稳定性（大多数 PSO + DescriptorSet 组合帧间不变）

**推荐修复**:
- 使用 **帧索引环形缓冲**：维护 N 帧的描述符堆，只在帧 N 等待完成后 reset 帧 N 的堆
- 或者使用 **持久化描述符映射**：对每个 DescriptorSet 分配固定 GPU 槽位，只在 dirty 时更新

---

### C3. forceUpdate() 每个 draw call 都重建所有描述符 — 无 dirty 追踪

**文件**: `D3D12CommandBuffer.cpp:557`

```cpp
void CCD3D12CommandBuffer::bindDescriptorSet(uint32_t set, DescriptorSet *descriptorSet, ...) {
    d3d12Set->forceUpdate(); // 每次绑定都全量重建！
    // ...
}
```

`forceUpdate()` 内部遍历所有 binding，为每个描述符调用 `CreateShaderResourceView` / `CreateConstantBufferView` / `CreateSampler`，这些都是 D3D12 API 调用，开销显著。

**影响**:
- 每个 draw call 的描述符绑定 = N 个 D3D12 描述符创建 API 调用
- 假设 1000 draw calls × 10 描述符/bindings = 10,000 次不必要的 D3D12 API 调用/帧
- 即使 DescriptorSet 内容未变，也会重建

**推荐修复**:
- 使用 dirty flag 机制：`forceUpdate()` 改为 `update()`，只在 `_isDirty` 时重建
- `bindDescriptorSet` 不应调用 `forceUpdate()`，只标记 pending
- 在 `flushDescriptorSets()` 之前一次性更新所有 dirty set

---

### C4. ~~文件 I/O 诊断泄漏到生产代码~~ — ✅ 已修复 (2026-05-03)

**文件**: `D3D12DescriptorSet.cpp`, `D3D12Buffer.cpp`

**修复内容**:
- 完全移除 `dsDiagLog()` 函数及其 `cstdio`/`cstdarg` 依赖
- 移除 `forceUpdate()` 中所有 `fileDiag` / `s_fileDiagCount` 分支
- 移除 `forceUpdate()` 中 `s_diagDescriptorSetLogCount` / `diagLog` 诊断
- 移除 `D3D12Buffer::update()` 中 `s_diagBufferUpdateCount` 诊断
- `D3D12Queue::dumpQueueDebugMessages` 改为仅 `#ifndef NDEBUG` 启用

---

## Important 级别问题

### I1. 所有 Buffer 都使用 UPLOAD 堆 — 无 GPU 本地内存利用

**文件**: `D3D12Buffer.cpp:161-167`

```cpp
D3D12_HEAP_PROPERTIES heapProperties{};
heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;  // 全部 UPLOAD 堆！
```

所有 Buffer（包括顶点、索引、uniform）都创建在 UPLOAD 堆上。

**影响**:
- UPLOAD 堆位于系统内存（通过 PCIe 映射到 GPU），带宽受限
- 顶点/索引缓冲等大量读取的资源应该在 GPU 本地内存（DEFAULT 堆）
- 对于频繁更新的小 uniform buffer，UPLOAD 堆是合理的，但不应该一刀切

**推荐修复**:
- Uniform/动态 buffer → UPLOAD 堆（Map/Unmap 更新模式）
- 顶点/索引/静态 buffer → DEFAULT 堆 + UPLOAD 中转缓冲
- 存储缓冲 → DEFAULT 堆

---

### I2. 无帧流水线 — 单帧在飞（No Frame Pipelining）

**架构层面**：

整个后端使用单个命令分配器、单个命令列表、同步提交。没有：
- 帧围栏数组（N 帧在飞）
- 每帧命令分配器
- 帧索引环形缓冲

**影响**: CPU 和 GPU 无法重叠执行，GPU 利用率低。

**推荐修复**: 实现 `FrameContext` 结构：
```cpp
struct FrameContext {
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    uint64_t fenceValue = 0;
    // 每帧上传缓冲回收
    // 每帧描述符堆
};
FrameContext frameContexts[FRAME_COUNT]; // 2 或 3
```

---

### I3. 每次纹理上传都创建新的 Upload Buffer — 无上传堆池

**文件**: `D3D12Device.cpp:459-483`, `D3D12CommandBuffer.cpp:906-929`

每次 `copyBuffersToTexture` 都会调用 `CreateCommittedResource` 创建一个新的上传缓冲。

**影响**:
- `CreateCommittedResource` 是重量级 D3D12 API 调用
- 批量纹理上传（如加载场景）时会产生大量临时资源创建/销毁
- GPU 驱动内部堆管理开销大

**推荐修复**:
- 创建一个大的 UPLOAD 缓冲池（如 64MB），从池中子分配
- 或者使用 `ID3D12Device3::CreateCommittedResource3` + `D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT` 延迟驻留

---

### I4. ~~flushDescriptorSets 中大量 ccstd::vector 动态分配~~ — ✅ 已修复 (2026-05-04)

**修复内容**:
- `flushDescriptorSets()`: `cbvEntries` 和 `samplerEntries` 从 `ccstd::vector` 改为固定大小 16 的栈数组
- `bindInputAssembler()`: `vbViews` 从 `ccstd::vector` 改为固定大小 16 的栈数组
- `beginRenderPass()` / `endRenderPass()`: `prePassBarriers` / `postPassBarriers` 改为固定大小 16 的栈数组
- `blitTexture()` / `copyTexture()` / `resolveTexture()`: 所有 barrier vector 改为固定大小 4 的栈数组
- 新增文件级常量 `MAX_PASS_BARRIERS = 16`
- 保留 `pipelineBarrier()` 的 vector（非热路径，数量不确定）
- 编译验证通过

---

### I5. DescriptorSet 每次创建新的 CPU 描述符堆

**文件**: `D3D12DescriptorSet.cpp:274-302`

每个 DescriptorSet 初始化时都会 `CreateDescriptorHeap` 创建自己的 CPU 暂存堆。

**影响**:
- 大量小型描述符堆 → 驱动开销
- 描述符堆不适合按 DesriptorSet 粒度管理

**推荐修复**: 使用全局 CPU 描述符堆分配器（线性分配器），所有 DescriptorSet 共享。

---

### I6. UAV 全局屏障 — 不必要的管线刷新

**文件**: `D3D12CommandBuffer.cpp:1526-1532`

```cpp
b.UAV.pResource = nullptr; // nullptr = 全局 UAV 屏障
```

当任何 buffer 有写入操作时，插入全局 UAV 屏障。

**影响**: 全局 UAV 屏障会导致 GPU 管线完全刷新（所有 UAV 操作必须完成才能继续），相当于一个隐式的全管线同步点。

**推荐修复**: 指定具体的 `pResource`，让 GPU 只等待相关资源的 UAV 操作完成。

---

## Minor 级别问题

### M1. 调试日志未清理

多个文件中存在 `CC_LOG_INFO` / `CC_LOG_WARNING` 的频繁调用（如 `blitTexture`、`copyTexture`、`resolveTexture` 每次调用都打印日志），在 Release 构建中仍会执行字符串格式化。

### M2. ~~ccstd::vector 用于资源屏障~~ — ✅ 已修复 (2026-05-04)

与 I4 一并修复。所有 render pass / texture 操作的 barrier vector 已替换为栈数组。

### M3. DISCARD 优化缺失

`beginRenderPass` 中 `LoadOp::DISCARD` 被注释为"D3D12 DISCARD optimization could be added later"。应使用 `DiscardResource` API 避免不必要的加载。

### M4. Swapchain 硬编码双缓冲

`BACK_BUFFER_COUNT = 2`，应考虑支持三缓冲以降低延迟和提高吞吐量。与 I2 帧流水线配合使用效果更佳。

### M5. 无 GPU 时间戳查询

`D3D12QueryPool` 支持 OCCLUSION 查询，但没有 TIMESTAMP 查询的 GPU 端实现。无法进行精确的 GPU 性能 profiling。

---

## 优化优先级建议

### 第一阶段（最高 ROI）
1. **实现帧流水线**（修复 C1 + I2）：三缓冲 + 异步 submit
2. ~~**移除诊断文件 I/O**（修复 C4）：立即收益~~ ✅ 已修复
3. **dirty flag 优化**（修复 C3）：减少 D3D12 API 调用
4. ~~**栈分配替代堆分配**（修复 I4 + M2）：消除 draw call 路径上的 malloc~~ ✅ 已修复

### 第二阶段（显著提升）
4. **描述符堆持久化**（修复 C2 + I5）：避免每帧重建
5. **Buffer 堆类型分化**（修复 I1）：GPU 本地内存利用
6. **上传缓冲池化**（修复 I3）：减少 CreateCommittedResource

### 第三阶段（精细优化）
7. **栈分配替代堆分配**（修复 I4 + M2）：消除 draw call 路径上的 malloc
8. **精确 UAV 屏障**（修复 I6）
9. **DISCARD 优化**（修复 M3）

---

## 性能量化预估

假设一个典型的 Cocos Creator 场景（~1000 draw calls，~10 个描述符/set，~3 个 set/draw）：

| 优化 | 当前开销/帧 | 优化后 | 预估提升 |
|------|-------------|--------|----------|
| 帧流水线 | CPU+GPU 串行 | CPU/GPU 重叠 | +30-50% FPS |
| dirty flag | 30,000 次 CreateCBV/SRV | ~1,000 次 | -90% API 调用 |
| 移除文件 I/O | 2,000 次 fopen/fprintf | 0 | 消除卡顿 |
| 栈分配 | 2,000 次 malloc/free | 0 | -5-10% CPU 开销 |
| 上传缓冲池 | N × CreateCommittedResource | 池子分配 | 加载时间 -50% |

---

## 结论

gfx-d3d12 后端功能完整但性能架构存在根本性的串行化瓶颈。最关键的优化是 **将同步 submit 改为异步帧流水线**（C1），仅此一项就能带来 30-50% 的帧率提升。其次是 **描述符管理优化**（C2 + C3 + I4），减少每帧 D3D12 API 调用数量。建议按三个阶段逐步实施，每阶段都有明确的可测量收益。
