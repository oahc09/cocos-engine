# D3D12 代码审查修复任务计划表

**基于**: AI/D3D12-CodeReview-Report.md (2026-04-30 审查)
**创建日期**: 2026-04-30
**目标**: 按优先级修复 D3D12 GFX 后端缺陷，解决黑屏问题

---

## 总览

| 统计 | 数量 |
|------|------|
| 总任务数 | 17 |
| Critical | 6 |
| Important | 6 |
| Minor | 5 |
| 已完成 | 0 |
| 进行中 | 0 |
| 待开始 | 17 |

---

## Phase 1: 黑屏根因修复（立即执行）

> 目标：确认并修复黑屏最可能的根因

### Task-01 | C5-验证 copyTexture/blitTexture 是否被调用
- **问题编号**: C5
- **严重程度**: 🔴 Critical
- **状态**: ⬜ 待开始
- **修改文件**: `D3D12CommandBuffer.cpp`
- **任务描述**: 在 `copyTexture()`、`blitTexture()`、`resolveTexture()` 三个空实现函数中添加诊断日志（CC_LOG_WARNING + drawDiagLog），记录调用参数（srcTexture/dstTexture 指针、regions count）。运行 WebGPUDemo，查看日志确认这些函数是否在渲染循环中被调用。
- **验收标准**: 日志输出确认这三个函数的调用情况（是否调用、调用频率、参数内容）
- **预计影响**: 确认或排除黑屏根因假设

### Task-02 | C5-实现 copyTexture()
- **问题编号**: C5
- **严重程度**: 🔴 Critical
- **状态**: ⬜ 待开始（Task-01 完成后开始）
- **依赖**: Task-01
- **修改文件**: `D3D12CommandBuffer.cpp`
- **任务描述**: 使用 D3D12 `CopyTextureRegion` API 实现纹理到纹理拷贝。需要处理：① src/dst D3D12_TEXTURE_COPY_LOCATION 构建 ② 子资源索引计算 ③ 资源状态转换（src→COPY_SOURCE, dst→COPY_DEST, 然后恢复）④ 多 region 循环
- **验收标准**: copyTexture 被调用时不再静默忽略，纹理内容正确拷贝
- **预计影响**: 如果 Task-01 确认 copyTexture 被调用，此修复可能直接解决黑屏

### Task-03 | C6-修复 acquire() 未更新 back buffer 索引
- **问题编号**: C6
- **严重程度**: 🔴 Critical（降为 Important）
- **状态**: ⬜ 待开始
- **修改文件**: `D3D12Device.cpp`
- **任务描述**: 在 `acquire()` 函数中，遍历 swapchains 列表，对每个 swapchain 调用 `_impl->swapChain->GetCurrentBackBufferIndex()` 更新 `currentBackBufferIndex`。确保 render 阶段使用的 back buffer 索引是 Present 后正确的值。
- **验收标准**: `acquire()` 调用后 `currentBackBufferIndex` 反映当前帧应渲染的 buffer
- **预计影响**: 消除 back buffer 索引不同步的隐患

### Task-04 | C3-修复 Framebuffer::getRTVHandle() swapchain 检查逻辑
- **问题编号**: C3
- **严重程度**: 🔴 Critical（当前场景影响有限）
- **状态**: ⬜ 待开始
- **修改文件**: `D3D12Framebuffer.cpp`
- **任务描述**: 修改 `getRTVHandle()` 的逻辑：不再仅检查 `_swapchain` 成员，而是检查 `index` 对应的 color texture 是否为 swapchain 纹理。如果是 swapchain 纹理，返回 `swapchain->getCurrentRTVHandle()`；否则返回 `_impl->rtvHandles[index]`。需要存储每个 color attachment 是否为 swapchain 的标记。
- **验收标准**: 非 swapchain color attachment 的 RTV handle 不再被 swapchain RTV 覆盖
- **预计影响**: 修复 MRT 场景下的渲染错误

---

## Phase 2: 架构稳定性修复（高优先级）

> 目标：修复可能导致偶发问题的架构缺陷

### Task-05 | C1-移除 Device::present() 中的重复 heapPool reset
- **问题编号**: C1
- **严重程度**: 🔴 Critical（当前时序安全但冗余）
- **状态**: ⬜ 待开始
- **修改文件**: `D3D12Device.cpp`
- **任务描述**: 删除 `Device::present()` 末尾（行506-512）对 `gpuDescriptorHeapPool` 和 `samplerDescriptorHeapPool` 的 reset 调用。这些 reset 已在 `CommandBuffer::begin()` 中执行，无需重复。保留一处 reset 避免混淆。
- **验收标准**: 每帧只在一处 reset descriptor heap pool
- **预计影响**: 消除双重 reset 的概念性混淆

### Task-06 | C2-评估多 CommandBuffer 场景的安全性
- **问题编号**: C2
- **严重程度**: 🔴 Critical（需确认）
- **状态**: ⬜ 待开始
- **修改文件**: 可能涉及 `D3D12Device.cpp`, `D3D12CommandBuffer.cpp`
- **任务描述**: 确认引擎是否在渲染循环中使用多个 CommandBuffer 实例。如果是：需要将 DescriptorHeapPool 的 reset 时机从 CommandBuffer::begin() 移到 Device 层面（帧开始时统一 reset）。如果否：降级为文档说明即可。
- **验收标准**: 确认 CommandBuffer 实例数量，必要时调整 reset 时序
- **预计影响**: 如果多 CB 存在且未修复，会直接导致黑屏

### Task-07 | C4-增强 CommandAllocator Reset 的安全检查
- **问题编号**: C4
- **严重程度**: Important
- **状态**: ⬜ 待开始
- **修改文件**: `D3D12CommandBuffer.cpp`
- **任务描述**: 在 `commandAllocator->Reset()` 调用前添加 HRESULT 检查和日志。如果 Reset 失败（说明 GPU 还在执行），输出错误日志而不是继续录制命令。考虑添加 GPU 同步等待作为安全网。
- **验收标准**: Reset 失败时有明确错误日志，不会导致 GPU 状态损坏

---

## Phase 3: 代码质量修复（中优先级）

> 目标：修复逻辑错误和诊断代码问题

### Task-08 | I1-评估 swapchain 纹理动态 handle 的安全性
- **问题编号**: I1
- **严重程度**: 🟡 Important
- **状态**: ⬜ 待开始
- **修改文件**: `D3D12Texture.cpp`
- **任务描述**: 确认 `getD3D12ResourceHandle()` 在 swapchain 场景下的调用时机。如果在命令录制期间（begin→end 之间）back buffer 索引可能变化，需要在 beginRenderPass 中缓存 handle 后不再重新查询。当前 `activeSwapchainBackBuffer` 已做缓存，需要确认所有使用路径。
- **验收标准**: 确认同一帧内所有对 swapchain 资源的引用使用相同的 handle

### Task-09 | I2-清理 static 诊断变量
- **问题编号**: I2
- **严重程度**: 🟡 Important
- **状态**: ⬜ 待开始
- **修改文件**: `D3D12CommandBuffer.cpp`, `D3D12Swapchain.cpp`, `D3D12Device.cpp`
- **任务描述**: 将所有 static 局部诊断计数器改为 CommandBuffer/Device 的成员变量，在 `begin()`/`doInit()` 时重置。涉及的变量：
  - `s_drawCallCount`, `s_seenShaders`, `s_endFrameCount`, `s_rpCount` (CommandBuffer)
  - `s_presentCount` (Device)
- **验收标准**: 诊断计数器在帧/会话重启时正确重置

### Task-10 | I3-修复 drawDiagLog 文件句柄管理
- **问题编号**: I3
- **严重程度**: 🟡 Important
- **状态**: ⬜ 待开始
- **修改文件**: `D3D12CommandBuffer.cpp`, `D3D12Swapchain.cpp`, `D3D12Device.cpp`
- **任务描述**: 将匿名命名空间中的 `static FILE*` 改为受管理的日志设施。添加 `closeDiagLog()` 函数，在 Device::doDestroy() 中调用。或者直接改用 `CC_LOG_INFO` 配合文件输出（引擎日志系统），避免直接管理 FILE*。
- **验收标准**: 无 FILE* 泄漏，日志系统可控

### Task-11 | I4-增强 pipelineBarrier 对 swapchain 纹理的处理
- **问题编号**: I4
- **严重程度**: 🟡 Important
- **状态**: ⬜ 待开始
- **修改文件**: `D3D12CommandBuffer.cpp`
- **任务描述**: 不再完全跳过 swapchain 纹理的 barrier。改为检查当前是否在 render pass 内：如果不在 render pass 内，且需要转换 swapchain 纹理状态（如 COPY 相关），应该执行 barrier。可以添加 `_inRenderPass` 标记来跟踪。
- **验收标准**: swapchain 纹理在 render pass 外的状态转换不被忽略

### Task-12 | I5-修复 beginRenderPass 中 s_rpCount 变量遮蔽
- **问题编号**: I5
- **严重程度**: 🟡 Important
- **状态**: ⬜ 待开始
- **修改文件**: `D3D12CommandBuffer.cpp:314-317`
- **任务描述**: 删除 swapchain 检测块内（行314-317）的 `static uint32_t s_rpCount = 0`，改为使用外层已有的 `s_rpCount`（行296）。
- **验收标准**: swapchain 诊断日志编号与实际 render pass 编号一致

### Task-13 | I6-修复 QueryPool 类型映射
- **问题编号**: I6
- **严重程度**: 🟡 Important
- **状态**: ⬜ 待开始
- **修改文件**: `D3D12CommandBuffer.cpp:1276-1278, 1292-1294`
- **任务描述**: 将 `beginQuery()` 和 `endQuery()` 中的 QueryType 映射改为正确的三目表达式：
  ```cpp
  D3D12_QUERY_TYPE queryType = (d3d12Pool->getType() == QueryType::OCCLUSION)
                                   ? D3D12_QUERY_TYPE_OCCLUSION
                                   : D3D12_QUERY_TYPE_TIMESTAMP;  // 或其他正确类型
  ```
- **验收标准**: QueryType 到 D3D12_QUERY_TYPE 映射正确

---

## Phase 4: 诊断代码清理与功能补全（低优先级）

> 目标：清理诊断代码，完善次要功能

### Task-14 | I7-重构 Device::present() pixel readback 诊断
- **问题编号**: I7
- **严重程度**: 🟡 Important
- **状态**: ⬜ 待开始
- **修改文件**: `D3D12Device.cpp`
- **任务描述**: 将 pixel readback 诊断代码改为可选的、安全的诊断模式：
  1. 用 `#ifdef CC_D3D12_DIAGNOSTIC_READBACK` 包裹
  2. 缓存 readback buffer（不每帧创建新的）
  3. 在 readback 和主 Present 之间添加 fence 同步
  4. 或完全移除，改用外部工具（如 PIX）
- **验收标准**: 生产构建不含 readback 诊断代码

### Task-15 | C5-实现 blitTexture()
- **问题编号**: C5 (续)
- **严重程度**: 🔴 Critical（功能缺失，但非立即阻塞）
- **状态**: ⬜ 待开始
- **依赖**: Task-02
- **修改文件**: `D3D12CommandBuffer.cpp`
- **任务描述**: D3D12 没有原生的 blit 功能，需要实现：
  - 方案 A: 如果 src 和 dst 大小相同，使用 CopyTextureRegion
  - 方案 B: 如果需要缩放，使用全屏四边形渲染 pass（创建临时 PSO、Shader）
  - 方案 C: 使用 compute shader 进行图像处理
  - 优先实现方案 A（等大小拷贝），覆盖大部分场景
- **验收标准**: blitTexture 不再是 no-op，纹理内容正确传递

### Task-16 | M1~M5-Minor 问题批量修复
- **问题编号**: M1, M2, M3, M4, M5
- **严重程度**: 🟢 Minor
- **状态**: ⬜ 待开始
- **修改文件**: 多文件
- **任务描述**: 批量修复 Minor 级别问题：
  - M1: 评估 vertex/index buffer 是否需要 DEFAULT heap（当前 UPLOAD 性能足够）
  - M2: 将硬编码路径 `"C:\\temp\\d3d12-render-diag.log"` 改为引擎配置
  - M3: 评估条件编译重构的必要性（当前 pImpl 已隔离，低优）
  - M4: 将诊断阈值改为环境变量 `CC_D3D12_DIAG_LIMIT`
  - M5: 为 setLineWidth/setDepthBound/setStencilWriteMask 添加 CC_LOG_WARNING 提示
- **验收标准**: 代码质量提升，无功能回归

---

## Phase 5: resolveTexture 实现（功能补全）

### Task-17 | C5-实现 resolveTexture()
- **问题编号**: C5 (续)
- **严重程度**: 🔴 Critical（仅 MSAA 场景需要）
- **状态**: ⬜ 待开始
- **依赖**: Task-02
- **修改文件**: `D3D12CommandBuffer.cpp`
- **任务描述**: 使用 D3D12 `ResolveSubresource` API 实现 MSAA resolve。需要：① 检查 src 是否为 MSAA 纹理 ② 资源状态转换 ③ 调用 ResolveSubresource
- **验收标准**: MSAA 纹理可以正确 resolve 到非 MSAA 纹理
- **备注**: 当前 WebGPUDemo 可能不使用 MSAA，优先级低于 copyTexture

---

## 执行策略

### 第一轮：快速验证 (Task-01)
```
Task-01 → 运行 WebGPUDemo → 检查日志
```
- 如果 copyTexture/blitTexture 被调用 → **Task-02 是最高优先级修复**
- 如果未被调用 → 黑屏根因在其他地方，继续排查

### 第二轮：根因修复 (Task-02 + Task-03 + Task-04)
```
Task-02 (copyTexture 实现)
Task-03 (acquire 修复) — 可并行
Task-04 (getRTVHandle 修复) — 可并行
```

### 第三轮：编译验证
```
Debug + Release 全量编译 → WebGPUDemo 运行 60+ 秒 → 确认黑屏是否解决
```

### 第四轮：稳定性修复 (Task-05 ~ Task-07)

### 第五轮：代码质量 (Task-08 ~ Task-17)

---

## 状态跟踪

| Task | 问题 | 优先级 | 状态 | 完成日期 | 备注 |
|------|------|--------|------|---------|------|
| Task-01 | C5-验证 | P0 | ⬜ | | |
| Task-02 | C5-copyTexture | P0 | ⬜ | | 依赖 Task-01 |
| Task-03 | C6-acquire | P1 | ⬜ | | |
| Task-04 | C3-getRTVHandle | P1 | ⬜ | | |
| Task-05 | C1-双重reset | P1 | ⬜ | | |
| Task-06 | C2-多CB评估 | P1 | ⬜ | | |
| Task-07 | C4-Reset安全 | P2 | ⬜ | | |
| Task-08 | I1-swapchain handle | P2 | ⬜ | | |
| Task-09 | I2-static清理 | P2 | ⬜ | | |
| Task-10 | I3-文件句柄 | P2 | ⬜ | | |
| Task-11 | I4-barrier跳过 | P2 | ⬜ | | |
| Task-12 | I5-变量遮蔽 | P2 | ⬜ | | |
| Task-13 | I6-QueryType | P2 | ⬜ | | |
| Task-14 | I7-readback | P3 | ⬜ | | |
| Task-15 | C5-blitTexture | P3 | ⬜ | | 依赖 Task-02 |
| Task-16 | M1~M5批量 | P4 | ⬜ | | |
| Task-17 | C5-resolveTexture | P3 | ⬜ | | 依赖 Task-02 |
