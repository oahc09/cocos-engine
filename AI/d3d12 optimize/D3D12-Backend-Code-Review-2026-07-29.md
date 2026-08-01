# D3D12 Backend Code Review

- 审查日期：2026-07-29
- 审查范围：`native/cocos/renderer/gfx-d3d12/`
- 审查基线：`15f44eb31e5c19c636416a1e90626cd31ac89ca0`
- 审查目标：检查 Direct3D 12 官方规范符合性、画面正确性、重启稳定性、CPU/GPU 性能和潜在风险
- 总体结论：**Not Ready**

## 1. 结论摘要

当前 D3D12 后端存在多项确定性的 P0 正确性和生命周期问题，不能继续以扩大 descriptor 缓存、自动合批或跳过绑定为首要方向。

需要优先解决：

1. `game.restart` 销毁顺序与跨设备静态对象复用。
2. Compute feature 虚假声明及缺失的 compute PSO/root binding。
3. 资源状态跟踪、Barrier 状态组合和 UAV ordering。
4. descriptor 缓存复用旧 CBV GPU 地址。
5. Validator actor 所有权和 GPU 资源保活。
6. MSAA resolve、stencil operation、Framebuffer attachment 等确定性画面错误。

D3D12 CPU 占用高于 Vulkan 的主要可疑来源已经能够从代码闭环解释：

- descriptor 动态 offset 改写、恢复和重复 copy；
- descriptor heap 溢出后的持续建堆和 heap 切换；
- Shader/PSO 热路径的大量日志、重复 hash/source 处理和文件访问；
- 绘制路径同步创建动态 PSO；
- 未利用并行 command-list recording；
- 缺少足够的运行时 counters 和场景级性能回归门禁。

## 2. P0：必须优先修复

### P0-1 `game.restart` 未在销毁 GFX 对象前等待 GPU idle

证据：

- `D3D12Device.h:90`：`frameSync()` 为空。
- `Engine.cpp:287`：restart 前仅调用空的 `frameSync()`。
- `RenderPipeline.cpp:166`：设备销毁前先销毁 QueryPool、CommandBuffer 和 FrameGraph transient。
- `Root.cpp:163`：直接 `delete swapchain`。
- `D3D12Swapchain.cpp:95`：真正包含 GPU wait 的 `doDestroy()` 被绕过。

影响：

- GPU 仍在引用 resource、descriptor、query heap 或 backbuffer 时，CPU 已释放对象。
- 可能触发 `OBJECT_DELETED_WHILE_STILL_IN_USE`、CPU UAF、device removal、黑屏或随机崩溃。

修复方向：

- 在 restart 清理任何 GFX 对象前执行一次可失败感知的 device idle。
- Root 必须调用 `swapchain->destroy()` 后再 delete。
- 长期方案是使用 fence 驱动的统一 deferred-release 队列。

### P0-2 静态 PSO/root signature 跨 D3D12 设备重启复用

证据：

- `D3D12CommandBuffer.cpp:243`：Blit/mipmap pipeline 使用进程级静态 map，只按 format 索引。
- `D3D12PipelineState.cpp:282`：empty root signature 使用进程级静态 `ComPtr`。
- Device 销毁时没有清空这些对象。

影响：

- 新设备可能复用旧设备创建的 PSO/root signature。
- 静态 `ComPtr` 还会延长旧设备对象生命周期。

修复方向：

- 缓存移入 `CCD3D12Device::Impl`。
- 缓存键包含 device identity/epoch。
- 在 GPU idle 后、释放 device 前统一清空。

### P0-3 Compute 被声明支持，但执行路径未实现

证据：

- `D3D12Device.cpp:286`：`Feature::COMPUTE_SHADER = true`。
- `D3D12PipelineState.cpp:608`：无条件要求 Vertex Shader。
- `D3D12PipelineState.cpp:617`：只构造 `D3D12_GRAPHICS_PIPELINE_STATE_DESC`。
- 后端不存在 `CreateComputePipelineState` 和 `SetComputeRoot*`。
- `D3D12CommandBuffer.cpp:4562`：仍允许执行 `Dispatch`。
- STORAGE_BUFFER 只创建 SRV，resource 未启用 UAV flag。

影响：

- Compute PSO 初始化失败。
- Dispatch 没有有效 compute PSO、root signature 或 descriptor table。
- Writable storage buffer 实际不可用。

修复方向：

- 按 `PipelineBindPoint` 分离 graphics/compute PSO。
- 增加独立 graphics/compute root-state cache 和 descriptor flush。
- 正确实现 SRV/UAV reflection、resource flag 和 barrier。
- 完成前将 compute feature 设为 false。

官方依据：

- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createcomputepipelinestate

### P0-4 资源状态跟踪不满足 per-resource/per-subresource 规则

证据：

- `D3D12Texture.cpp:330`：Texture view 只在创建时复制 owner 状态。
- `D3D12Texture.h:61`：每个 wrapper 保存独立 `_currentState`。
- `D3D12CommandBuffer.cpp:4702`：能够生成 mip/layer/plane 范围 Barrier。
- `D3D12CommandBuffer.cpp:4744`：范围 Barrier 后把整个 wrapper 标记为新状态。
- `D3D12CommandBuffer.cpp:1671`：`endRenderPass()` 无条件把离屏颜色资源转为 SRV。
- `D3D12CommandBuffer.cpp:1703`：无条件把 depth 转为可读状态。

影响：

- owner/view 交替使用时 `StateBefore` 过期。
- 部分子资源转换后，未转换子资源被错误记录为新状态。
- FrameGraph 声明的前态可能与 `endRenderPass()` 修改后的真实状态冲突。

修复方向：

- 以底层 `ID3D12Resource` 为唯一状态身份。
- 使用 uniform state + sparse subresource override，或完整 per-subresource 状态数组。
- view 与 owner 共享同一个 backing/state object。
- `endRenderPass()` 不推测下一用途，由 FrameGraph 声明驱动下一状态。

### P0-5 Barrier 会生成非法状态并遗漏 Texture UAV ordering

证据：

- `D3D12CommandBuffer.cpp:4606`：AccessFlags 通过 OR 生成 D3D12 state。
- 可能产生 `RENDER_TARGET | PIXEL_SHADER_RESOURCE`、`DEPTH_WRITE | READ` 等非法组合。
- `D3D12CommandBuffer.cpp:4699`：Texture 前后状态相同时直接跳过。
- UAV barrier 只对 Buffer 实现。
- GeneralBarrier 将任意 write access 转换为全局 UAV barrier。
- UPLOAD/READBACK buffer 没有固定状态保护。

影响：

- Debug Layer 报错、CommandList 被丢弃或画面错误。
- 同状态 UAV texture write/read 缺少 ordering。
- UPLOAD heap 资源可能被非法转出 `GENERIC_READ`。

修复方向：

- 写状态必须排他，只允许组合只读状态。
- 对不可原生表达的同子资源读写明确拒绝或使用复制/UAV 方案。
- Texture 和 Buffer 都实现同状态 UAV barrier。
- UAV barrier 只用于 UAV access。
- UPLOAD/READBACK heap 永久保持固定状态。

官方依据：

- https://learn.microsoft.com/zh-cn/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_type

### P0-6 local static descriptor table 缓存复用旧 CBV

证据：

- `D3D12CommandBuffer.cpp:741`：缓存项不包含 descriptor version、GPU VA 或 CBV size。
- `D3D12CommandBuffer.cpp:2524`：缓存命中依赖 resource identity。
- `D3D12DescriptorSet.cpp:1449`：UNIFORM_BUFFER signature 只混入 Buffer actor 指针。
- `D3D12Buffer.cpp:323`：Transient uniform 更新会改变 GPU VA 并递增 `uniformDescriptorVersion`。

影响：

- 同一个 Buffer actor 更新后，后续 draw 仍可能读取旧 upload slice。
- 表现为旧常量、随机闪烁、材质错误或黑屏。

修复方向：

- 缓存键至少包含 descriptor/static version、CBV GPU VA、size 和 backing resource generation。
- 先增加旧地址复现测试，再修改缓存逻辑。

### P0-7 确定性的渲染语义错误

#### 显式 MSAA resolve 格式错误

- `D3D12CommandBuffer.cpp:4518` 固定向 typed resolve 传入 `DXGI_FORMAT_UNKNOWN`。
- Typed source/destination 必须传实际兼容格式。

官方依据：

- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resolvesubresource

#### Stencil clamp/wrap 映射相反

- `D3D12PipelineState.cpp:422` 将 `INCR/DECR` 映射为 wrap。
- 将 `INCR_WRAP/DECR_WRAP` 映射为 saturate/clamp。

官方依据：

- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_stencil_op

#### Framebuffer 静默替换 attachment

- `D3D12Framebuffer.cpp:207` 根据尺寸、格式、采样数选择“最近创建”的其他颜色资源。
- `D3D12Framebuffer.cpp:324` 在后续访问时还可能刷新为另一个资源。

影响：

- 多个相同尺寸/格式 RT 并存时，渲染目标身份不确定。
- 可能直接渲染到错误纹理。

修复方向：

- 移除按尺寸推断 attachment identity 的逻辑。
- 修复 FrameGraph attachment 构造；非法 framebuffer 应明确失败。

## 3. P1：P0 稳定后处理

### P1-1 Validator actor 所有权错误

- `D3D12CommandBuffer.cpp:825` 使用 `IntrusivePtr<CCD3D12CommandBuffer>` 保活 bundle actor。
- Validator 在 `CommandBufferValidator.cpp:237` 解包为 raw actor。
- Validator 析构又直接 `delete _actor`。

修复方向：

- 不对 Validator-owned actor 建立临时 IntrusivePtr 所有权。
- 保留 wrapper、owner token 或显式的 fence-lifetime object。

### P1-2 Descriptor heap 溢出后持续创建新 heap

- `D3D12DescriptorHeapPool.cpp:164`：frame range 模式只扫描 heap 0。
- 新建 overflow heap 后，下一次分配仍不扫描该 heap。
- `beginFrameAllocationRange()` 也只重置首 heap。

影响：

- `CreateDescriptorHeap` 风暴。
- heap 永久增长。
- 额外 repack、descriptor copy 和 `SetDescriptorHeaps`。
- Bundle 还可能违反“一次 SetDescriptorHeaps 且必须与 caller 匹配”的限制。

修复方向：

- 使用 fence-protected frame overflow pages。
- 每个 frame slot 保存 current page，并复用退休页。
- 加入 overflow 次数、峰值 descriptor 数和 heap switch counters。

官方依据：

- https://learn.microsoft.com/en-us/windows/win32/direct3d12/descriptor-heaps-overview

### P1-3 动态 offset descriptor rewrite 放大 CPU 开销

- `D3D12CommandBuffer.cpp:2787`：每次 miss 先改写 staging descriptor。
- Copy 到 GPU-visible heap 后，再恢复零 offset descriptor。
- fallback 路径会前后两次 `forceUpdate()`。

影响：

- 每个动态 descriptor 可能产生两次 CreateView 和一次 copy。
- offset 高频变化时直接放大 CPU。
- 共享 DescriptorSet 不适合并行 command-list recording。

修复方向：

- CPU staging descriptor 保持 immutable。
- 按 metadata 直接在最终 allocation 或 CommandBuffer 私有 scratch 中创建动态 descriptor。
- 保留动态 offset 正确性测试后再做 copy/cache 优化。

### P1-4 Shader/PSO 热路径诊断和同步工作过重

证据：

- `D3D12Shader.cpp:1254`：额外构造 diagnostic grouping source/key。
- `D3D12Shader.cpp:1330`：每个 stage 无条件输出长 INFO。
- `recordDXBCHashComparison()` 会锁 mutex、hash/canonicalize DXBC 并保留完整 bytecode。
- `D3D12PipelineState.cpp:884`：每个 PSO 同步计算 key 并读取文件。
- `D3D12PipelineState.cpp:926`：cache miss 后同步持久化 PSO。
- `D3D12PipelineState.cpp:966`：绘制绑定路径可能同步创建动态 PSO variant，缓存无上限。

修复方向：

- 生产默认关闭 stage/PSO 详细诊断。
- 只输出按帧或按场景聚合 counters。
- PSO cache 异步、原子持久化。
- 动态 PSO 组合预热、量化并限制缓存大小。

### P1-5 Fence 失败路径仍继续资源复用

- `D3D12CommandBuffer.cpp:932`：wait helper 返回 `void`。
- CreateEvent/SetEvent 失败后，调用者仍清理 retained resources 并 Reset allocator。
- `D3D12Device.cpp:524`：忽略 `WaitForSingleObject` 结果后重置 frame slot。

修复方向：

- wait API 返回 success/device-lost/failure。
- 非成功结果禁止 allocator、descriptor、upload page 和 frame slot 复用。
- 集中处理 `GetDeviceRemovedReason()`。

### P1-6 CommandList 资源保活不完整

- VB/IB 绑定只记录 GPU VA，没有把 native resource 放入 fence-retained 集合。
- DescriptorSet 保存 raw actor/resource identity。
- Buffer/Texture destroy 会立即 Reset COM resource。
- Buffer view 只保存 parent raw pointer。

修复方向：

- 录制时保活所有被命令列表实际引用的 `ID3D12Resource`。
- 随 recording context fence 退休。
- Buffer view 使用共享 backing object，而不是无所有权 parent raw pointer。

官方依据：

- https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles

## 4. P2：性能能力建设

### P2-1 Present 配置不可与 Vulkan 公平比较

- `D3D12Swapchain.cpp:43` 使用 `Present(0, 0)`。
- 未检测 tearing support。
- Swapchain create/resize 也未设置 `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`。

修复方向：

- 检测 `DXGI_FEATURE_PRESENT_ALLOW_TEARING`。
- create、resize、present 使用一致 flag。
- 对比 Vulkan 时统一 vsync、窗口模式、帧率限制和 compositor 条件。

官方依据：

- https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays

### P2-2 未使用 native D3D12 render pass

当前仍使用 `OMSetRenderTargets + Clear + ResourceBarrier` 模拟 render pass。

修复方向：

- 检测 `D3D12_FEATURE_D3D12_OPTIONS5::RenderPassesTier`。
- 支持时使用 `ID3D12GraphicsCommandList4::BeginRenderPass/EndRenderPass`。
- 正确映射 load/store/resolve，评估 TBDR 和集成 GPU 收益。

官方依据：

- https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-12-render-passes

### P2-3 未利用并行 command-list recording

当前代码以“D3D12 是 single-threaded COM-based”为理由关闭 detached device thread，但 D3D12 官方设计明确允许多个 command list 并行录制。

修复方向：

- 先把 DescriptorSet staging、heap pool 和状态 tracker 改为每线程或每 CommandBuffer 所有。
- 再按 pass/batch 并行录制多个 command list。
- queue submission 保持有序。

官方依据：

- https://learn.microsoft.com/en-us/windows/win32/direct3d12/design-philosophy-of-command-queues-and-command-lists

## 5. 修复前测试和衡量基线

### 正确性测试

1. Debug Layer + GPU-based validation 全开。
2. `game.restart` 连续执行至少 100 次。
3. 每次 restart 后验证首帧、阴影、后处理、UI、粒子和离屏 RT。
4. Texture owner/view 交替访问测试。
5. 单 mip、单 layer 和多 plane Barrier 测试。
6. 同状态 Texture UAV write/read ordering 测试。
7. MSAA explicit resolve 与 render-pass resolve 测试。
8. Stencil clamp/wrap 边界测试。
9. Compute dispatch、SRV/UAV storage buffer 测试。
10. Descriptor local split、input attachment 和 fallback 故障注入测试。
11. 同一 uniform Buffer 更新 GPU VA 后再次 draw，验证读取新数据。
12. Framebuffer 中创建多个相同尺寸/格式 RT，验证 attachment identity。

### 性能 counters

每帧至少统计：

- `flushDescriptorSets`
- `CopyDescriptorsSimple` 调用数和 descriptor 总数
- 动态 offset descriptor rewrite 数
- `SetDescriptorHeaps`
- graphics/compute root table bind
- transition/UAV/alias barrier 数
- descriptor heap overflow 和新建 heap 数
- fence wait 次数和耗时
- Shader cache hit/miss、compile、PSO create 和文件 I/O 耗时
- 动态 PSO variant 新建数和缓存峰值

### 对比条件

- D3D12、Vulkan、GLES 使用同一分辨率、场景、资源和帧率策略。
- 关闭详细诊断日志。
- 预热 Shader/PSO 后分别测量冷启动与稳态。
- 分离主线程、渲染线程、驱动线程 CPU 时间。
- 同时记录 draw/instance/triangle、descriptor 和 barrier 数，避免只比较总 CPU。

## 6. 当前验证结果

- `D3D12HotPathStaticTest.py`：通过。
- `d3d12_perf_static_test.py`：`64 passed`。
- `test-cases.vcxproj Debug|x64`：集成编译成功。

现有静态测试主要检查源码 token 和结构，不能证明运行时正确性，其中部分测试还明确要求保留当前存在所有权风险的 `IntrusivePtr<CCD3D12CommandBuffer>`。因此必须补充上述 Debug Layer 场景测试。

## 7. 推荐执行顺序

1. 修复 restart idle、swapchain destroy 和 device-scoped static cache。
2. 修复统一 per-subresource 状态 tracker、Barrier 状态映射和 Texture UAV barrier。
3. 修复 descriptor static cache 旧 CBV、fallback 和 Validator actor 所有权。
4. 修复 resolve、stencil、Framebuffer attachment、Compute/STORAGE_BUFFER。
5. 完成 P0 全场景正确性回归。
6. 建立 counters 基线。
7. 修复 descriptor overflow、动态 rewrite 和日志/PSO 热路径。
8. 在同等 Present 条件下重新对比 D3D12、Vulkan、GLES。
9. 最后评估 native render pass、Root Signature 1.1 和并行 command-list recording。

## 8. 逐项评估与优化解决方案

### 8.1 评估口径与优先级校准

| 等级 | 含义 |
|---|---|
| P0 | 会导致崩溃、UAF、设备对象跨 restart 泄漏、黑屏、错误渲染，或对外宣称了实际不支持的核心能力；必须先保证正确性 |
| P1 | 当前正确路径上存在显著 CPU、内存或稳定性风险；必须在 counters 和回归测试就绪后优化 |
| P2 | 架构能力、平台特定优化或测量条件改进；不能阻塞 P0/P1 |
| S/M/L/XL | 预计实现复杂度，分别为小、中、大、跨模块重构 |

本轮重新评估后有两处优先级调整：

1. **Validator actor 所有权由 P1 提升为 P0**。它不是一般资源保留开销，而是可直接形成 UAF、重复释放和 restart 崩溃的生命周期错误。
2. **Compute 支持为条件 P0**。只要 `DeviceFeature::COMPUTE_SHADER` 仍返回 true，就必须按 P0 修复；若短期明确关闭能力并阻止创建/提交 Compute 工作，可将完整 Compute 后端实现延后。

总体依赖顺序：

1. restart idle 和对象生命周期；
2. device-scoped cache 与 command-list 资源保留；
3. 统一资源状态 tracker 和 Barrier；
4. descriptor 正确性与 resolve/stencil/framebuffer；
5. counters；
6. descriptor、PSO、fence 等 CPU 热点；
7. native render pass 和并行录制。

---

### 8.2 P0-1 restart 前未建立 GPU idle 边界

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 触发条件 | `game.restart`、窗口/Swapchain 重建、RenderPipeline 销毁时 GPU 仍引用旧资源 |
| 影响 | UAF、旧资源被新对象复用、随机 Framebuffer/Texture/ComPtr 崩溃、设备移除 |
| 复杂度 | M |
| 性能影响 | 正确实现只在 restart/teardown 等低频路径等待，稳态帧不应增加 wait |

**止血方案**

1. 不要把 D3D12 `frameSync()` 直接实现成每帧 `waitForGpu()`。`Engine` 每帧都会调用该函数，这会把 CPU/GPU 完全串行化。
2. 新增明确的低频 API，例如 `Device::waitIdle()` 或 `Device::prepareForRestart()`，仅在 restart、Device/Swapchain 销毁和重大重建之前调用。
3. `Root` 必须先调用 `swapchain->destroy()`，再删除 Swapchain 包装对象，不能依赖析构顺序碰巧释放。
4. wait 失败时进入 device-lost teardown，不能继续释放并复用 command allocator、descriptor page 和资源。

**完整方案**

1. 统一各后端的 restart 协议：停止提交新帧 -> 等待所有 queue timeline/fence -> 销毁 pipeline/framegraph transient -> 销毁 swapchain -> 销毁 device-owned cache。
2. D3D12 queue 用单调 fence value 建立 drain 点；等待 Direct/Copy/Compute queue 中所有实际启用的 queue。
3. restart 进入状态机，禁止等待期间脚本或渲染线程再次提交。
4. teardown 后执行 Debug Device live-object report，区分应用保留对象和驱动内部对象。

**负面影响与防护**

- 错把等待放入普通 frame loop 会直接造成 D3D12 CPU/GPU 性能下降，应以 counter 断言稳态 `restartWaitCount == 0`。
- 若只等待 Direct queue，而上传使用独立 Copy queue，仍可能释放正在上传的资源；所有活跃 queue 都必须纳入 drain。

**验收**

- Debug Layer + GPU-based validation 下连续 restart 100 次。
- 不出现 object deleted while in use、device removed、旧 Framebuffer/Texture 指针访问。
- restart 后首帧、离屏 RT、阴影、后处理正确。
- 非 restart 场景 fence wait 次数和耗时不增加。

---

### 8.3 P0-2 PSO、Root Signature 和状态表使用进程级 static 生命周期

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 触发条件 | Device restart 后 static cache 命中旧 Device 创建的 COM 对象，或旧 resource 状态表未清理 |
| 影响 | 跨 Device 非法对象、PSO/root signature 绑定失败、黑屏、随机崩溃 |
| 复杂度 | M |
| 性能影响 | device-scoped cache 不降低命中率；只改变所有权和清理时机 |

**止血方案**

1. 为现有 static cache 增加 Device identity/epoch，cache key 必须包含创建对象的 Device。
2. Device 销毁前，在 GPU idle 之后显式 purge blit PSO、empty root signature 和 process-static resource state。
3. restart 后禁止任何旧 epoch 的 cache entry 返回给新 CommandBuffer。

**完整方案**

1. 把 blit PSO、empty root signature、root-signature cache 和资源状态注册表移入 `CCD3D12Device::Impl`。
2. cache entry 由 Device 持有，Device 析构按依赖顺序统一释放。
3. 如存在后台 Shader/PSO 创建，cache 本身必须线程安全，并在 teardown 前停止生产者任务。
4. Texture owner/view 共享一个 device-owned backing/state object；Swapchain backbuffer unregister 也由同一生命周期管理。

**负面影响与防护**

- 仅在 key 中加入裸 `ID3D12Device *` 不足以长期安全，地址可能复用；应使用单调 Device epoch 或明确 cache owner。
- purge 必须在 GPU idle 后进行，否则只是把跨 restart UAF 改成 teardown UAF。

**验收**

- 多次 restart 后新建 PSO/root signature 不引用旧 Device。
- Debug Device 报告中不存在旧 epoch 的应用持有对象。
- blit、copy、mipmap、首帧 PSO 创建正常，无 `E_INVALIDARG`。

---

### 8.4 P0-3 宣称支持 Compute，但没有完整 Compute pipeline

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认，优先级取决于是否继续对外报告支持 |
| 触发条件 | 创建 Compute PipelineState、绑定 storage buffer/UAV、调用 dispatch |
| 影响 | 创建 graphics PSO 失败、root 参数类型错误、dispatch 无有效 PSO、结果错误或黑屏 |
| 复杂度 | XL |
| 性能影响 | 完整实现后才能进行 D3D12/Vulkan Compute 性能比较 |

**止血方案**

1. 短期将 Compute feature 设为 false。
2. 创建 Compute PSO、绑定 Compute descriptor 或 dispatch 时返回明确错误并终止该工作，不能静默落入 graphics 路径。
3. 禁止把 storage buffer 一律映射成 SRV；未实现 UAV 写能力前不应暴露对应 usage。

**完整方案**

1. PipelineState 增加 graphics/compute bind point，Compute 使用 `D3D12_COMPUTE_PIPELINE_STATE_DESC` 和 `CreateComputePipelineState`。
2. Shader 反射区分 CBV/SRV/UAV/Sampler，storage buffer/texture 按读写属性创建正确 descriptor。
3. Buffer/Texture 创建时根据 usage 设置 `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`。
4. CommandBuffer 增加 Compute root signature、PSO 和 root table/root descriptor 的独立脏状态。
5. 使用 `SetComputeRootDescriptorTable`、`SetComputeRootConstantBufferView`、`SetComputeRootShaderResourceView`、`SetComputeRootUnorderedAccessView`。
6. dispatch 前完成 transition/UAV ordering；dispatch 后由下一使用者驱动状态转换。
7. indirect dispatch 需要正确的 command signature、argument buffer 状态和资源保留。

**负面影响与防护**

- Graphics 和 Compute 可以共享 layout 描述，但不能共享同一组“已绑定”缓存，否则 graphics/compute root state 会互相污染。
- 只补 `CreateComputePipelineState` 而不补 UAV descriptor、barrier 和 resource flag，会产生更隐蔽的数据错误。

**验收**

- Compute SRV -> UAV、UAV -> SRV、连续 UAV write、indirect dispatch 测试。
- GPU readback 与 Vulkan 结果逐元素比较。
- Debug Layer 无 root-signature mismatch、invalid state、UAV ordering 警告。

---

### 8.5 P0-4 Texture view 和 owner 没有共享统一的 per-subresource 状态

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 触发条件 | owner/view 交替使用、只转换部分 mip/layer/plane、Swapchain 多 backbuffer |
| 影响 | `StateBefore` 错误、缺失 transition、冗余 barrier、错误渲染和 GPU validation 报错 |
| 复杂度 | XL |
| 性能影响 | 正确设计可同时减少冗余 barrier；错误设计会增加锁和全量状态数组开销 |

**止血方案**

1. 在统一 tracker 完成前，partial range 无法可靠追踪时宁可保守转换整个 resource，也不要更新一个代表全资源的标量状态却只发 partial barrier。
2. owner 和所有 view 查询同一 canonical state；禁止 view 初始化时复制一次状态后独立演化。
3. 移除 endRenderPass 对“下一步大概率是 SRV”的推测性转换，状态由实际下一 use 决定。

**完整方案**

1. 每个原生 `ID3D12Resource` 对应一个共享 backing object，持有 COM 资源、格式/plane 信息和全局已提交状态。
2. 状态表示采用“统一状态 + 稀疏 subresource override”；大多数全资源转换保持 O(1)，partial transition 才展开。
3. Texture view 只保存共享 backing 和自己的 mip/layer/plane range。
4. Command list 录制时使用局部 tracker，生成 barrier 并记录 final states；queue 按提交顺序 commit 到全局状态。
5. Swapchain 每个 backbuffer 是独立 backing，不共享一个状态标量。
6. 为未来并行录制保留 ownership：录制线程不直接无锁修改全局 tracker。

**负面影响与防护**

- 在录制过程中直接写全局 tracker 会阻碍并行 command list，并可能使未提交/丢弃的列表污染状态。
- 全量 `subresourceCount` vector 对大数组纹理有内存成本，应使用稀疏表示。
- 不应依赖隐式 promotion/decay 修复 tracker 错误；先按显式规则正确，再有证据地减少 barrier。

**验收**

- owner/view、单 mip、单 layer、cube face、depth/stencil plane、resolve/copy/render/sampling 循环。
- GPU-based validation 下无 state mismatch。
- barrier counter 与基线对比，正确性修复后冗余 barrier 不应无上限增长。

---

### 8.6 P0-5 Barrier 状态组合和 Texture UAV ordering 不完整

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 触发条件 | 多 AccessFlag 组合、同状态 Texture UAV 连续读写、upload/readback heap、split barrier |
| 影响 | 非法 state mask、漏 barrier、数据竞争、Debug Layer 错误、间歇性画面错误 |
| 复杂度 | L |
| 性能影响 | 合法映射和批量 barrier 可减少驱动验证成本；过度全局 UAV barrier 会损失 GPU 并行性 |

**止血方案**

1. AccessFlag 映射先分类为 read/write/CPU-only，而不是简单按位 OR。
2. 多个互斥 write state，或 write 与不兼容 read 的组合，立即断言并报告调用来源。
3. Texture 在 state 不变但存在 UAV write 前后依赖时，发 resource-specific UAV barrier。
4. Upload heap 永久保持 `GENERIC_READ`，Readback heap 永久保持 `COPY_DEST`；不要对它们生成普通 transition。
5. 未完整实现 split barrier 前，明确降级为普通完整 barrier，不能静默跳过 begin。

**完整方案**

1. 建立表驱动 Access classifier，只允许组合多个兼容 read state。
2. attachment、input attachment、copy、resolve、indirect、present、depth read/write 分开映射。
3. HOST access 作为 CPU/GPU 同步语义处理，不映射成 D3D12 resource state。
4. general barrier 只有在真实 UAV write 依赖存在时才发 global UAV barrier；有具体资源时优先 resource-specific。
5. 如果保留 split barrier，严格配对 BEGIN_ONLY/END_ONLY 的 resource、subresource、before/after 状态。
6. 同一批次 barrier 聚合到数组后一次 `ResourceBarrier` 提交。

**负面影响与防护**

- 为保险对每次 draw 发 global UAV barrier 会显著降低 GPU 并行性。
- Access mapping 不能脱离资源 heap type、usage 和 subresource tracker 单独决定。

**验收**

- 建立 AccessFlag 合法/非法组合矩阵单测。
- 连续 Texture UAV write、UAV write -> sample、sample -> UAV write、Buffer UAV 对照测试。
- 记录 transition/UAV/global UAV barrier 数，确保 global barrier 只在必要处出现。

---

### 8.7 P0-6 local static descriptor table 可能复用旧 CBV

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认；DescriptorSet 已有版本，但 CommandBuffer local cache 未使用版本 |
| 触发条件 | 同一资源对象的 GPU VA、range 或 descriptor 内容变化，缓存仍只按资源 identity 命中 |
| 影响 | Shader 读取旧 uniform 地址或旧 view，表现为错误材质、错误矩阵、闪烁或黑屏 |
| 复杂度 | M |
| 性能影响 | 精确版本 key 仍保留安全命中；完全禁用缓存会增加 descriptor copy |

**止血方案**

1. local static table cache entry 增加 `DescriptorSet identity + staticDescriptorVersion`。
2. cache lookup 必须精确匹配 owner 和版本；短期禁止不同 DescriptorSet 之间仅因 resource pointer 相同而共享 table。
3. 对包含 transient uniform CBV 的 table 可先禁用跨 draw 复用，只缓存稳定 Texture/Sampler/default resource。

**完整方案**

1. DescriptorSet 每次 view/GPU VA/range/format/资源版本改变时更新 descriptor version。
2. cache key 至少包含 descriptor-set version、buffer uniform-descriptor version、GPU VA、offset/range、format/view 维度和 frame heap epoch。
3. CPU staging descriptor 只作为不可变模板；最终 shader-visible table 的 cache 生命周期限制在所属 heap epoch 内。
4. cache 失效只影响对应 set/table，不做全局清空。

**负面影响与防护**

- 只比较资源指针不能证明 descriptor 内容相同。
- 只比较 DescriptorSet 通用 version 可能因为无关 binding 更新造成命中率过低；正确后再细分 table version。
- cache key 缺少 heap epoch 会在 descriptor heap reset 后返回失效 GPU handle。

**验收**

- 同一 Buffer 重建/resize/GPU VA 改变后，在同一 CommandBuffer 中再次 draw。
- 两个 DescriptorSet 使用同一 Buffer 但不同 offset/range。
- 动态 offset、frame slot rollover、restart 后 table 不复用旧 handle。

---

### 8.8 P0-7 Resolve、Stencil 和 Framebuffer attachment 语义错误

#### 8.8.1 Explicit resolve 使用 UNKNOWN format，render-pass resolve 索引错误

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 触发条件 | MSAA resolve、多 color attachment resolve、typeless resource |
| 影响 | resolve 调用非法、颜色错误、某些 attachment 未 resolve |
| 复杂度 | M |

**解决方案**

1. 验证 source 为 multisampled、destination 为 single-sampled，尺寸、format family、subresource 数兼容。
2. `ResolveSubresource` 传入实际 typed `DXGI_FORMAT`，不能使用 `DXGI_FORMAT_UNKNOWN`。
3. subresource 使用 `D3D12CalcSubresource`，包含 mip、array slice 和 plane。
4. render-pass resolve attachment 按 color slot 对应的 resolve slot 获取，不能按 source attachment index 反向索引 resolve 数组。
5. resolve 前后状态全部走统一 tracker。

**验收**

- 单 RT、多 MRT、array/cube、HDR/UNORM、depth resolve 支持范围内的用例与 Vulkan 图像比对。

#### 8.8.2 Stencil INCR/DECR clamp 与 wrap 映射反转

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 影响 | 边界值处 stencil 行为与 API 语义相反，导致遮罩、阴影体、UI clipping 错误 |
| 复杂度 | S |

**解决方案**

1. `INCR`/`DECR` 映射到饱和 clamp 操作。
2. `INCR_WRAP`/`DECR_WRAP` 映射到 wrap 操作。
3. front/back face 都覆盖边界值 0、1、254、255。

#### 8.8.3 Framebuffer 按尺寸/格式替换 attachment

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 触发条件 | 同时存在多个尺寸/格式相同的 RT，或 restart 后 registry 中仍有旧对象 |
| 影响 | 渲染到错误纹理、引用旧 Texture、ComPtr 崩溃 |
| 复杂度 | M，可能暴露上层 FrameGraph 生命周期问题 |

**止血方案**

1. 删除“按尺寸/格式寻找最新 Texture”作为自动修复的行为。
2. attachment 无效时记录对象 ID、generation、资源 owner、创建/销毁 epoch，并拒绝创建该 Framebuffer。
3. 不要为了避免黑屏把 attachment 替换成任意同形状资源。

**完整方案**

1. FrameGraph 显式传递实际 attachment owner/backing，不使用全局模糊 registry 推断。
2. transient resource handle 增加 generation，compile 后过期 handle 在 execute 前即可被检测。
3. Framebuffer 持有 attachment backing 的强引用直到最后一次 GPU 使用完成。

**验收**

- 同时创建多个相同尺寸/格式 RT，逐个写入不同颜色并回读。
- restart 前后 FrameGraph transient attachment identity 不串用。
- attachment 失效时稳定报错，不访问已销毁 ComPtr。

---

### 8.9 P0-8 Validator actor 所有权和 secondary CommandBuffer 生命周期

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认，原 P1 提升为 P0 |
| 触发条件 | primary actor 以 IntrusivePtr 保留 secondary actor，而 Validator wrapper 仍直接 delete actor；restart 会集中触发 |
| 影响 | UAF、double delete、`RefCounted::addRef()` 崩溃、任意后续资源指针损坏 |
| 复杂度 | L |
| 性能影响 | 正确 backing-state 所有权按提交保留，成本与 command list 数相关，不应按 draw 增长 |

**止血方案**

1. D3D12 primary actor 不再用 `IntrusivePtr<CCD3D12CommandBuffer>` 拥有 Validator 管理的 actor。
2. Validator primary wrapper 临时强持有被 execute 的 secondary wrapper，直到对应提交 fence 完成。
3. primary 下一次 `begin()` 前先确认上一次 context fence 完成，再释放 secondary 保留集合。
4. Validator 开关两种路径都必须有明确所有者，不能依赖 actor 初始引用计数碰巧避免删除。

**完整方案**

1. 把可提交的原生状态抽成共享 `CommandSubmissionState`：command list、allocator、descriptor pages、retained resources、last fence。
2. wrapper 和 actor 只引用该 backing；primary execute 保留 backing，而不是保留另一个 actor。
3. `notifySubmitted`、allocator recycle 和资源释放都基于 backing 的 fence。
4. wrapper/actor 可先销毁，GPU backing 在 fence 完成后再回收。

**负面影响与防护**

- 只把 IntrusivePtr 改成裸指针会消除 double ownership，但 secondary 若提前销毁仍会 UAF；只能作为非常短期止血且必须由 wrapper 保留。
- 最终模型不要在 Validator 与非 Validator 下拥有两套不同生命周期规则。

**验收**

- Validator 开/关分别执行 secondary 录制、primary execute、立即销毁 secondary。
- restart 100 次，开启 PageHeap/ASan 可用配置。
- 不出现 `RefCounted::addRef`、actor 析构、command allocator recycle 相关崩溃。

---

### 8.10 P0/P1 补充正确性项：descriptor 计数、fallback 和顶点格式

#### 8.10.1 INPUT_ATTACHMENT descriptor 被重复计数

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认；是否触发取决于当前是否生成 INPUT_ATTACHMENT layout |
| 优先级 | P1；若项目实际使用 local input attachment，则提升 P0 |
| 影响 | table 分区偏移错误、未初始化 descriptor slot、local root table 错位 |
| 复杂度 | S |

**解决方案**

1. INPUT_ATTACHMENT 在 CBV/SRV/UAV table 中只计数一次。
2. layout 预计算出每个 binding 的 table offset，DescriptorSet update 按同一 metadata 写入。
3. debug 下断言实际写入数等于分配数，并检查所有 slot 已初始化。

#### 8.10.2 descriptor fallback 提前清 dirty，且不支持 local split roots

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 优先级 | P0（只要 fallback 可在真实运行中触发） |
| 影响 | heap 分配失败后 draw 使用缺失/旧 root table，造成黑屏或错误资源 |
| 复杂度 | M |

**解决方案**

1. dirty bit 只有在所有 table 成功分配、复制并绑定后才能清除。
2. regular/local roots 使用同一份 binding plan 和提交函数，fallback 不能只处理 regular roots。
3. 如果无法完整 fallback，当前 draw 明确失败并保留 dirty 状态，不能继续提交不完整绑定。
4. 通过容量故障注入强制走 fallback，验证所有 root table。

#### 8.10.3 不支持的 vertex format 被静默替换为其他宽度

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 优先级 | P1；项目资产实际使用该格式时提升 P0 |
| 影响 | Input Layout 与真实 stride/数据宽度不一致，顶点属性串位或越界读取 |
| 复杂度 | M |

**解决方案**

1. 建立精确 GFX format -> DXGI format 表，不允许 unknown 默认落到 `R32G32B32_FLOAT`。
2. D3D12 不原生支持的 RGB8 等格式，在资源上传/mesh 构建阶段显式 repack 为 RGBA8，并同步修改 stride/offset。
3. 无法转换时 PipelineState 创建失败并打印具体 semantic、format、stride。

---

### 8.11 P1-1 shader-visible descriptor heap overflow 后持续创建新 heap

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 触发条件 | 单帧 descriptor 超过固定分区，或动态 rewrite 大量消耗 shader-visible 空间 |
| 影响 | 内存增长、heap switch、root table 全部失效重绑、CPU 增长 |
| 复杂度 | L |
| 性能收益 | 高，前提是 counters 证明 overflow/copy/switch 是热点 |

**止血方案**

1. 为每个 frame slot 记录所有 overflow page，并在该 slot fence 完成后统一 reset/reuse。
2. 设置 page 数量上限；耗尽时安全跳过 draw/报错，不无限创建。
3. 每帧统计 peak descriptor、overflow 次数、新建 heap 数和 `SetDescriptorHeaps`。

**完整方案**

1. 使用 frame-slot-owned descriptor page arena，每个 page 有明确 heap、capacity、cursor、epoch。
2. 根据 p99 峰值加余量调整首个 shader-visible heap 容量，尽量保持一帧只使用一个 CBV/SRV/UAV heap 和一个 Sampler heap。
3. direct command list 切 heap 后，显式使所有 root descriptor table 失效并重绑。
4. bundle 录制前预留容量；不要在 bundle 中产生与 primary 不兼容的 heap 切换。
5. reset 时遍历并回收该 frame slot 的全部 page，不能只处理第一个 heap。

**负面影响与防护**

- 单纯扩大 heap 会增加显存/系统内存占用，但可能仍掩盖泄漏；必须同时观察“每帧峰值”和“跨帧 heap 总数”。
- heap epoch 必须进入 descriptor table cache key。

**验收**

- 单帧超过 65,536 descriptor 的压力场景。
- 稳态运行后 heap 总数不继续增长。
- overflow 后画面和 root table 均正确，heap switch 数符合预期。

---

### 8.12 P1-2 dynamic offset 通过 apply/restore 重写 descriptor

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 触发条件 | 大量 draw 使用 dynamic uniform/storage offset |
| 影响 | CPU descriptor create/copy、重复扫描和缓存失效，是 D3D12 CPU 高于 Vulkan 的候选主因 |
| 复杂度 | L |
| 性能收益 | 潜在高，但正确性风险也高 |

**修改前必须采集**

- dynamic offset descriptor rewrite 次数；
- `CopyDescriptorsSimple` 调用数和 descriptor 数；
- 每次 `flushDescriptorSets` 的 set/table 数；
- 相同 set/version/offset 的重复率；
- root table bind 和 heap epoch 变化。

**低风险优化**

1. CPU staging descriptor 保持不可变。
2. 根据 dynamic offset 直接在当前 frame-unique 最终 slot 创建调整后的 CBV/SRV/UAV，不再 apply 后 restore。
3. exact cache key 使用 descriptor-set version、buffer descriptor version、完整 offsets 数组、heap epoch。
4. 只有 key 完全一致时跳过 rewrite/copy。

**进一步优化**

1. 高频动态 CBV 可改为 root CBV，直接绑定 GPU VA，消除 descriptor copy。
2. 高频动态 SRV/UAV 仅在格式、对齐、range 和 root-signature DWORD budget 满足时使用 root descriptor。
3. 按 table 细分 dirty/version，避免一个 binding 改变导致整个 set 重建。

**负面影响与防护**

- dynamic storage range 的 `base + dynamicOffset + logicalRange` 必须严格校验，不能为了通过验证静默缩短错误范围。
- root descriptor 不携带 texture view 等完整 descriptor 语义，不能泛化替换。
- 在 P0-6 static cache 和 heap epoch 未修复前，不应启用“跳过重复 copy”。

**验收**

- dynamic offset 正确性测试先于性能对比。
- counters 证明 rewrite/copy/root bind 实际下降。
- 输出图像、Buffer readback 与优化前/Vulkan 一致。

---

### 8.13 P1-3 Shader/PSO 诊断、创建和文件 I/O 位于热路径

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 触发条件 | Shader variant 首次出现、动态 PSO miss、详细日志开启、缓存文件同步读写 |
| 影响 | 主线程尖峰、冷启动和运行时卡顿、日志锁竞争 |
| 复杂度 | M-L |
| 性能收益 | 冷启动和 variant-heavy 场景高，稳态取决于 miss 数 |

**止血方案**

1. 详细 shader/PSO diag 由编译宏和运行时开关双重控制，Release 默认关闭。
2. 热路径只累计轻量 counter，不逐次拼接长字符串或写文件。
3. hash comparison 只在专项调查模式执行，不能改变完整 Shader source/define 的 cache identity。

**完整方案**

1. PSO cache key 包含 shader bytecode hash、root signature hash、render state、input layout、RT/DS format、sample state。
2. 持久化 cache identity 加入 adapter LUID、driver version、引擎 cache schema version。
3. PSO 文件写入放入后台队列并使用临时文件 + 原子替换；退出时限时 flush。
4. 评估 `ID3D12PipelineLibrary`，减少碎片化文件 I/O。
5. 记录实际出现的 dynamic variant 并在加载阶段预热；设置 cache budget/LRU。

**负面影响与防护**

- 异步写入不能捕获即将销毁的 Device/PSO 裸指针。
- 不能为了减少 variant 任意量化或丢弃影响渲染语义的 state。
- PSO miss 时不要在 draw 中静默绑定 null PSO；受控跳过 draw并给出聚合错误。

**验收**

- 分离冷启动、预热后稳态、首次出现新 variant 三组数据。
- 记录 compile、PSO create、cache hit/miss、文件 I/O 总耗时。
- Release 场景不再输出逐 Shader/逐 PSO 热路径日志。

---

### 8.14 P1-4 fence/event 失败后仍回收资源，上传 fence 未纳入 frame slot

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 触发条件 | Signal/Event/Wait 失败、device removed、上传 queue 尚未完成但 frame slot 被复用 |
| 影响 | allocator/descriptor/upload page 提前复用、UAF、数据损坏、死等 |
| 复杂度 | M |
| 性能影响 | 正常路径只增加少量分支和 counter |

**解决方案**

1. wait API 返回 `Completed/Timeout/DeviceLost/Failed`，检查 `Signal` HRESULT、`SetEventOnCompletion` HRESULT 和 `WaitForSingleObject` 结果。
2. 只有 `Completed` 或 fence 已达到目标值时，才 reset allocator、descriptor arena、upload page 和 retained resources。
3. device removed 后记录 `GetDeviceRemovedReason`，停止新提交并进入受控 teardown。
4. 每个 frame slot 保存 Direct/Copy/Upload 等所有相关 fence 的最大值；slot reuse 前全部达到。
5. 或将 upload page 完全归属独立 upload context，由其自身 fence 回收，避免与 frame fence 混用。
6. fence wait counter 区分 restart wait、frame pacing wait、资源池耗尽 wait。

**负面影响与防护**

- 不能通过超时后继续执行来“避免卡死”，这会把同步错误变成数据破坏。
- device-lost 路径中不要无限等待永远不会完成的 fence。

**验收**

- 对 Signal、event 创建、SetEvent、wait 结果做故障注入。
- 上传后立即跨帧复用压力测试。
- 失败时不释放未完成资源，日志包含 fence value、queue 和 device removed reason。

---

### 8.15 P1-5 command list 对 GPU 使用资源保留不完整

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 触发条件 | 录制后、提交前或 fence 完成前销毁/resize Buffer、Texture、DescriptorSet、PSO |
| 影响 | GPU 使用已释放资源、随机设备移除和 restart 崩溃 |
| 复杂度 | L |
| 性能影响 | 应与每个 command list 的唯一资源数相关，不能按 draw 重复 AddRef |

**解决方案**

1. 将 `pendingUploadResources` 扩展/重命名为通用 `retainedResources`。
2. DescriptorSet 根据自身 version 预计算原生资源 backing 列表；flush 时 CommandBuffer 去重保留。
3. IA 绑定时保留 vertex/index buffer backing。
4. 同样保留 PSO、root signature、indirect argument、query、descriptor heap 和用于 resolve/copy 的资源。
5. queue 提交后，保留集合转移到 submission batch，目标 fence 完成后统一释放。
6. Buffer/Texture view 持有共享 backing，不依赖 parent wrapper 裸指针。

**负面影响与防护**

- 每 draw 遍历全部 descriptors 会放大 CPU；资源列表应按 DescriptorSet version 缓存。
- 用裸 COM 指针列表不足以保留对象，必须使用强引用 backing/ComPtr。

**验收**

- 录制/提交后立即销毁或 resize 所有资源类型。
- 多 command list 同时引用同一资源，分别在不同 fence 完成。
- restart 中未完成 submission 的资源保持到 drain 完成。

---

### 8.16 P1-6 Root Signature 重复序列化和创建

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 影响 | PipelineLayout 创建成本、重复 COM 对象和内存 |
| 复杂度 | M |
| 性能收益 | 主要改善加载/场景切换，不一定改善稳态帧 |

**解决方案**

1. 对规范化后的 descriptor layout、visibility、static sampler、push constant/root constant 生成稳定 hash。
2. 在 Device 内建立线程安全 Root Signature cache，value 为强引用 root signature。
3. 优先使用 Root Signature 1.1，在能力允许时为不会改变的 descriptor range 设置合适 flags。
4. cache 生命周期绑定 Device epoch，restart 后不可复用。
5. 记录 create 次数、命中率和序列化耗时，收益不足时不扩大复杂度。

---

### 8.17 P1/P2 补充 CPU 热点：CPU descriptor allocator 释放时排序

| 维度 | 评估 |
|---|---|
| 确认程度 | 代码路径存在，是否为瓶颈需要 counter/profile 证明 |
| 优先级 | P2；若场景中 descriptor 高频创建销毁且 profile 命中，可提升 P1 |
| 复杂度 | M |

**解决方案**

1. 先统计每帧 allocate/free 次数、free-list 长度和排序耗时。
2. 若确认热点，改为按地址有序 map 做相邻合并，或按固定 page/size class 分配。
3. shader-visible arena 与长期 CPU descriptor allocator 分离，避免为线性 frame allocation 使用通用 free list。

---

### 8.18 P2-1 Present 配置与 Vulkan/GLES 对比条件不一致

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认存在配置风险 |
| 影响 | CPU/GPU 占用比较失真，Present 阻塞日志本身增加开销 |
| 复杂度 | S-M |
| 性能收益 | 主要提升测量可信度和帧 pacing，不保证渲染本体更快 |

**解决方案**

1. 查询 `DXGI_FEATURE_PRESENT_ALLOW_TEARING`。
2. VSync 开启时使用 `Present(1, 0)`；关闭时在支持且 swapchain 创建 flags 一致的前提下使用 `Present(0, DXGI_PRESENT_ALLOW_TEARING)`。
3. resize/recreate 保持 tearing flag 一致。
4. 可评估 frame-latency waitable object 和 `SetMaximumFrameLatency`，统一 CPU ahead 深度。
5. Present 慢调用改为聚合 counter/采样，不逐帧输出日志。
6. D3D12/Vulkan/GLES 对比固定分辨率、VSync、帧率上限、窗口模式和 frame-in-flight。

**验收**

- 分别测 VSync on/off，记录 Present CPU wait。
- 对比报告明确列出 frame pacing 参数。

---

### 8.19 P2-2 未使用 native D3D12 Render Pass

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 前置条件 | P0 状态 tracker、resolve/load/store 语义全部正确 |
| 复杂度 | L |
| 性能收益 | GPU/带宽收益依硬件而定，不能预设一定优于显式 OM 路径 |

**解决方案**

1. 查询 `D3D12_FEATURE_D3D12_OPTIONS5::RenderPassesTier`，保留现有 fallback。
2. 将 load/store/clear/discard/resolve 映射到 beginning/ending access。
3. render pass 内禁止不符合规范的 barrier、clear、resolve 和 attachment 重绑定。
4. 以 FrameGraph pass 为边界，只合并语义完全兼容的 subpass。
5. 分别测试独显、集显/TBDR 风格硬件，测 GPU time、带宽和 CPU call 数。

**负面影响与防护**

- 在 P0-4/P0-5 未完成前引入 native render pass 会放大状态和 resolve 错误。
- 必须保持输出图像与 fallback bitwise 或容差范围一致。

---

### 8.20 P2-3 未利用并行 command-list recording

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 前置条件 | 全局可变状态、descriptor arena、资源 tracker、PSO cache 均具备线程所有权/同步 |
| 复杂度 | XL |
| 性能收益 | draw-heavy 且主线程录制受限时潜在最高；小场景可能因调度开销负收益 |

**解决方案**

1. 先用 PIX/ETW 确认 CPU 主要消耗在 command recording，而不是 Shader 编译、descriptor copy、日志或 fence wait。
2. 将 FrameGraph 中无依赖的 pass/batch 分配到 worker-specific command allocator/list/descriptor arena。
3. 每个 command list 使用局部状态 tracker，提交阶段按 FrameGraph 顺序合并 final state。
4. 初期优先并行录制 direct command list，不以 bundle 作为首选，避免 descriptor heap 限制。
5. queue submission 保持依赖顺序，必要时由主线程集中生成跨 list prologue/epilogue barrier。
6. 设置任务粒度阈值，小 pass 留在主列表，避免线程调度成本高于录制收益。

**负面影响与防护**

- 任何 process-static map、无锁 pool、录制期全局状态写入都会形成数据竞争。
- 并行前先完成资源强引用和 fence 回收，否则崩溃会更难复现。

**验收**

- 记录主线程、worker、驱动线程 CPU，比较串行/并行录制总 CPU 和 frame time。
- Thread Sanitizer 可用平台或自定义并发压力测试不出现数据竞争。
- command list 顺序变化不改变图像结果。

---

### 8.21 P2 补充项：Readback 每区域创建资源并重复等待

| 维度 | 评估 |
|---|---|
| 确认程度 | 已确认 |
| 触发条件 | 截图、GPU readback、测试回读、多区域复制 |
| 影响 | committed resource 创建和 CPU fence wait 放大 |
| 复杂度 | M |

**解决方案**

1. 使用可回收 readback page/ring，按对齐要求分配 footprint。
2. 同一批 readback 录制完只 Signal/Wait 一次。
3. 返回异步 readback ticket；只有调用方真正读取 CPU 数据时才等待。
4. page 按 fence 回收，device lost 时走统一错误路径。

该项通常不影响普通渲染帧，除非引擎场景每帧进行 readback，因此默认保留 P2。

---

### 8.22 实施门禁：先测量，再修改 P1/P2

所有 P1/P2 性能优化必须通过以下门禁，避免再次误伤动态 offset、资源状态或 restart 正确性：

1. **建立基线**：固定场景、分辨率、VSync、frame-in-flight，采集至少 300 个稳态帧。
2. **轻量 counters**：按帧汇总，默认不逐调用日志；至少包含 descriptor flush/copy/rewrite、heap bind/overflow、root bind、barrier、fence wait、PSO/Shader。
3. **一次只改一个变量**：每项优化保留开关，可 A/B。
4. **正确性先行**：Debug Layer、GPU validation、restart、dynamic offset、owner/view、MSAA、stencil 全部通过。
5. **性能验收**：比较 median、p95、p99 CPU frame time，同时检查 GPU time 和内存，不能只看任务管理器总 CPU。
6. **负收益回退**：若 CPU 改善不足 3% 且复杂度/内存显著增加，默认不合入；正确性修复不受该阈值限制。

建议新增统一的 `D3D12FrameCounters`，每个 CommandBuffer 写本地计数，frame end 再归并，避免热路径原子竞争。Release 默认关闭详细文本输出，仅在采样窗口输出一行汇总或写入 profiler event。

### 8.23 最终落地顺序

1. P0-1 restart idle 和 Swapchain 正确 destroy。
2. P0-8 Validator/secondary backing 所有权。
3. P0-2 device-scoped cache 与 P1-5 command-list 资源保留。
4. P0-4/P0-5 统一资源状态和 Barrier。
5. P0-6 descriptor static cache、fallback、input attachment 计数。
6. P0-7 resolve、stencil、Framebuffer identity、vertex format。
7. Compute：短期关闭虚假能力；需要时完成完整 Compute/UAV 支持。
8. 运行全部 P0 正确性回归并冻结基线。
9. 上线轻量 counters，按数据依次处理 descriptor overflow、dynamic rewrite、Shader/PSO 和 fence/upload。
10. 统一 Present 条件后重新对比 D3D12、Vulkan、GLES。
11. 最后评估 native render pass、Root Signature 1.1、readback pool 和并行录制。
