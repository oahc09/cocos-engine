# gfx-d3d12 性能、功耗与内存关键优化点审查

> 日期：2026-08-08  
> 范围：当前 `v3.8.8_custome` 分支、2026-08-01 已提交基线，以及 2026-08-08 工作区中的未提交 D3D12 性能实验  
> 目标：在相同画面、分辨率、帧率和运行条件下，使 gfx-d3d12 的吞吐、能效和内存占用均优于 gfx-gles3  
> 本轮性质：只识别关键优化点和测量门禁，不修改现有实验代码

## 1. 第一轮结论

当前 D3D12 后端已经解决了主要画面正确性和 draw-call 性能问题，但若目标从“性能不差”升级为“性能、功耗、内存都必须优于 GLES3”，仅继续缩小 heap/page/slot 常量不会成功。

GLES3 驱动会在内部完成资源子分配、命令合并、状态缓存和提交调度。D3D12 把这些责任交给引擎；只有显式实现下列能力，才能把低层 API 的理论优势变成稳定优势：

1. **统一的 GPU 内存分配器与 FrameGraph 瞬态资源别名复用**；
2. **持久化 upload ring、批量上传和 Copy Queue**；
3. **稳定 shader-visible descriptor heap、静态 descriptor 常驻和动态 CBV 直绑**；
4. **DXGI 可等待交换链与面向功耗的帧调度**；
5. **原生 D3D12 Render Pass、DISCARD/store/resolve 语义**；
6. **离线 Shader/PSO 产物，消除发布版运行时编译链**；
7. **并行 command-list recording 与更通用的 draw batching**。

其中前 3 项是同时改善 CPU、功耗和内存的核心工程；第 4、5 项主要改善帧稳定性、GPU 带宽和能效；第 6、7 项负责拉开启动和高 draw-call 场景与 GLES3 的差距。

### 当前未提交实验的结论

当前实验中，释放已完成 Shader 的 HLSL source、关闭非诊断 DXBC hash retention、清理 scheduler callable 等改动方向正确；但以下改动暂时不应作为最终方案合入：

- 每帧对多个 `vector` 调用 `shrink_to_fit()`；
- 每帧对多个 `unordered_map/set` 调用 `rehash(0)`；
- 每帧释放全部 upload pages；
- 把 shader-visible descriptor 每帧预算从 65,536 直接降到 512；
- 把 sampler heap 降到 16、CPU descriptor heap 降到 64；
- 在没有 acquire-wait 数据的前提下把三帧资源改为两帧；
- 只提供一个不可回收、满后不扩容的 64 KB shared upload pool。

这些方案会用堆分配、`CreateCommittedResource`、`CreateDescriptorHeap` 和更频繁的 heap switch 换取较低的瞬时常驻内存，容易同时损伤 CPU 时间、功耗和 P99 帧时间。

## 2. 先建立公平的 GLES3 对比门禁

“必须优于 GLES3”需要拆成两种运行模式。只看一个 FPS 或一个 `PrivateMemorySize64` 数字无法得出结论。

| 模式 | 控制条件 | 主要指标 | 通过标准 |
|---|---|---|---|
| 吞吐模式 | 同场景、同分辨率、同画质、前台、关闭 VSync/限帧、Release | CPU/GPU frame time、uncapped FPS、P95/P99 | D3D12 平均和 P99 均优于 GLES3 |
| 能效模式 | 相同 60 FPS 或显示器刷新率、相同画面与 Present 语义 | CPU/GPU 功耗、J/frame、CPU time/frame、GPU busy | D3D12 在不掉帧前提下 J/frame 更低 |
| 内存模式 | 相同场景路径、相同资源驻留时间、相同预热 | Private Bytes、Working Set、DXGI local/non-local usage、分配器 committed/requested | 稳态、峰值和碎片率均不高于 GLES3 |
| 启动模式 | 清缓存/热缓存分别测试 | 首帧、首个可交互帧、Shader/PSO 编译时间、峰值内存和能耗 | 冷/热启动均不劣于 GLES3 |

当前 `measure*.ps1` 只能读取最小化进程的 `PrivateMemorySize64`，存在四个问题：

1. 最小化窗口可能进入 background/pause 路径；
2. 没有固定 test case、相机、资源加载完成标志；
3. Private Bytes 混合了引擎、运行库、驱动和缓存，不能代表 GPU local/non-local memory；
4. 没有同协议的 GLES3 样本，也没有 GPU/CPU 能耗和帧时间。

因此它可以做单分支内的粗筛，不能作为“优于 GLES3”的交付证据。

建议在 D3D12 内新增轻量聚合计数器：

- requested/committed buffer 与 texture bytes；
- heap 数、placed resource 数、fragmentation 和 peak；
- upload bytes、upload page create/reuse、Copy Queue submit 数；
- descriptor copy/write、heap create、heap switch、overflow 数；
- root CBV bind、descriptor table bind、PSO create/hit、dynamic PSO variant 数；
- barrier 数、fixup command list 数、command list 数；
- frame-resource wait 次数和 p50/p95/p99 wait time。

同时使用 `IDXGIAdapter3::QueryVideoMemoryInfo` 记录进程 local/non-local budget/usage。D3D12 当前没有像 GLES3/Vulkan 一样更新 `Device::_memoryStatus`，引擎内存统计会漏掉 D3D12 buffer/texture。

## 3. 按优先级推进关键优化

### P0-1：统一 D3D12 内存分配器与瞬态资源别名

**代码证据**

- `D3D12Buffer.cpp` 和 `D3D12Texture.cpp` 仍以 `CreateCommittedResource` 创建普通资源；
- 后端没有 `CreateHeap`、`CreatePlacedResource`、`GetResourceAllocationInfo` 或 aliasing barrier 的实现；
- Vulkan 后端已经集成 VMA，而 D3D12 没有对应的显式分配层；
- 当前小 buffer shared pool 只有一个 64 KB append-only arena，没有 free list、分页、合并和 fence 退休。

**为什么是最高优先级**

小 buffer 每个 committed resource 都会承担 D3D12 allocation granularity 和对象开销；RenderTexture、shadow、post-process 等 FrameGraph 资源即使生命周期不重叠，也会同时保有独立物理内存。GLES 驱动通常会在内部做这类子分配，D3D12 不实现 allocator 就很难在内存上获胜。

**目标方案**

1. 建立 DEFAULT buffer heap allocator：大页 + buddy/TLSF/slab 子分配；
2. 按 Heap Tier 和资源类别区分 buffer、non-RT texture、RT/DS texture heap；
3. 建立持久化 UPLOAD ring 和 READBACK pool；
4. FrameGraph 输出资源 lifetime interval，非重叠 transient RT/DS 使用 placed resource 重叠区域；
5. 别名切换插入 `D3D12_RESOURCE_BARRIER_TYPE_ALIASING`；
6. 使用 fence 退休 allocation，避免每帧销毁 heap/resource；
7. 记录 requested、committed、internal/external fragmentation；
8. 根据 DXGI budget 做 trim/stream，而不是固定常量硬砍。

Microsoft 的 D3D12 内存管理建议明确将资源按用途分类，并建议对 shadow、post-process 等复用资源使用 heap、placed resource 和帧内别名复用：<https://learn.microsoft.com/en-us/windows/win32/direct3d12/memory-management-strategies>。

**第一步实验**

先只迁移 `<=64 KB` 的静态 DEFAULT buffer 和 FrameGraph transient color/depth attachment，不碰长期 texture streaming。验证收益后再扩展。

**门禁**

- committed/requested 比率显著下降；
- steady/peak DXGI usage 和 Private Bytes 均下降；
- `CreateCommittedResource` 次数在场景加载后趋近 0；
- Debug Layer、RenderDoc 和全部 test cases 无差异；
- 不允许以增加每帧 CPU allocator 时间换内存。

### P0-2：批量上传、持久化 upload ring 与 Copy Queue

**代码证据**

`CCD3D12Device::copyBuffersToTextureImmediate()` 每次调用都会：

1. `CreateCommandAllocator`；
2. `CreateCommandList`；
3. 记录 barrier/copy/mipmap；
4. `ExecuteCommandLists`；
5. `Signal` fence；
6. 保存一个 `PendingUploadCommandContext`。

当前 upload page 实验还会在 frame slot 复用时释放全部 page，下一次上传重新 `CreateCommittedResource + Map`。这对首次加载、场景切换和流式纹理非常不利。

**目标方案**

- 一个持久化 COPY queue；
- 每个 frame/stream batch 使用池化 copy allocator/list；
- 一个或少量持久映射的 upload ring page，按 fence 退休区间；
- 同一加载批次的 buffer/texture copy 合并到一至数个 command list；
- graphics queue 只在首次消费该资源前等待 copy fence；
- mipmap 需要 graphics/compute 时，把 copy 与 mip generation 分成明确的跨 queue 阶段；
- command allocator/list 放入 fence-safe pool，不在每次 texture upload 创建。

Microsoft 文档推荐用 fence 管理 upload heap ring 的空间复用：<https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management>。D3D12 也明确支持独立 COPY queue 和跨 queue fence：<https://learn.microsoft.com/en-us/windows/win32/direct3d12/executing-and-synchronizing-command-lists>。

**门禁**

- 场景加载期 `CreateCommandAllocator/List` 次数从“每 texture 一次”降为固定池规模；
- upload submit/signal 次数至少按加载批次合并；
- 首帧和流式加载 P99 改善；
- upload committed bytes 有上限且 steady state 不反复 create/free；
- copy/graphics queue 不出现额外全局等待。

### P0-3：稳定 descriptor heap 与彻底消除动态 descriptor 改写

**代码证据**

- `D3D12CommandBuffer.cpp` 仍有多条 `applyDynamicOffsets() -> CopyDescriptorsSimple() -> restoreDynamicOffsetDescriptors()` 路径；
- fallback 仍可能调用 `forceUpdate()`；
- 当前实验把 GPU descriptor 每帧预算降至 512，但历史 R140 即使完成精确静态缓存后，仍记录 5,115 次 descriptor copy/帧；
- descriptor heap overflow 会创建新 heap，`SetDescriptorHeaps` 会使已有 root descriptor table 失效并触发重新绑定；
- 当前 sampler heap 16、CPU descriptor heap 64 不能由代码常量证明足够。

**目标方案**

1. shader-visible heap 对象长期稳定，按 fence 划分 frame pages；
2. page 大小来自场景 p99 descriptor demand + 余量，不使用拍脑袋常量；
3. material/static SRV/sampler 写入持久区域，一次创建、多帧复用；
4. 所有高频动态 uniform 尽可能使用 root CBV，直接绑定 GPUVA；
5. 必须留在 table 的 dynamic descriptor 直接写入 CommandBuffer 私有最终 slot，不再修改/恢复共享 CPU staging descriptor；
6. descriptor cache key 保留 resource identity/version、heap epoch 和完整 dynamic offsets；
7. overflow 使用可回收 page，不能每次创建永久 heap，也不能每帧销毁。

**门禁**

- `CreateDescriptorHeap` 在稳态为 0；
- `SetDescriptorHeaps` 每 command list 接近 1；
- dynamic descriptor restore 次数为 0；
- descriptor copy/write 数显著低于 GLES3 对应 bind 操作数量；
- heap committed bytes 低于当前基线，但 P99 帧时间不退化。

### P0-4：可等待交换链与能效帧调度

**代码证据**

- `D3D12Swapchain.cpp` 当前使用 `Present(0, 0)`；
- swapchain 未设置 `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`；
- 未调用 `SetMaximumFrameLatency()` 或 `GetFrameLatencyWaitableObject()`；
- 60 Hz 调度依赖外层 Win32 timer，而不是 DXGI 实际可接收下一帧的时机。

**目标方案**

- 创建 waitable swapchain；
- 能效模式在开始一帧前等待 frame-latency handle；
- `MaxFrameLatency=1` 用于最低功耗/延迟，若 CPU+GPU 流水不足再 A/B `2`；
- VSync 模式和 uncapped tearing 模式分开，不能共用结论；
- DXGI pacing 成为 Windows runtime 唯一呈现节拍，上层不再重复 sleep。

Microsoft 的 waitable swapchain 指南指出，在开始渲染前等待 DXGI 接收新帧可以避免额外排队；latency 1 有利于最低延迟和功耗，latency 2 可增加 CPU/GPU 并行：<https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains>。

**门禁**

- 60 FPS 模式平均功耗和 J/frame 低于 GLES3；
- present queue 深度、frame latency 和 P99 不恶化；
- CPU 不忙等；
- latency 1/2 由数据选择，不能与 `D3D12_MAX_FRAMES_IN_FLIGHT` 机械绑定。

### P1-1：原生 D3D12 Render Pass 与 attachment discard/store

**代码证据**

- `beginRenderPass()` 使用 legacy barrier、`OMSetRenderTargets` 和显式 clear；
- `LoadOp::DISCARD` 当前注释为“do nothing”；
- 后端没有 `ID3D12GraphicsCommandList4::BeginRenderPass/EndRenderPass`；
- subpass resolve 通过显式 unbind、barrier、`ResolveSubresource` 和再 barrier 完成。

**目标方案**

- 查询 D3D12 render-pass tier；
- 将 GFX load/store/resolve 映射到 beginning/ending access；
- `DISCARD` 映射为 DISCARD，CLEAR 在 pass beginning 声明；
- resolve 使用 ending resolve；
- unsupported adapter 保留当前 legacy fallback；
- 与 FrameGraph transient alias 一起使用，减少无意义 load/store 和显存流量。

D3D12 Render Pass 的目标之一就是减少 on-chip 与主存之间不必要的 load/store，降低内存流量：<https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-12-render-passes>。

**门禁**

- GPU bandwidth、GPU busy 和 J/frame 下降；
- shadow、deferred、post-process、MSAA 场景收益优先验证；
- tile-based/UMA 和 discrete GPU 分开统计；
- 所有 load/store/resolve test case 像素一致。

### P1-2：发布版离线 Shader 与 PSO 管线

**代码证据**

当前 cache miss 路径在运行时执行完整链路：

```text
GLSL -> glslang SPIR-V -> SPIRV-Cross HLSL -> D3DCompile SM 5.1 DXBC -> PSO
```

即使 cache 能改善第二次运行，首次安装、cache 失效和新 variant 仍会支付编译、临时内存和后台线程成本。动态 depth bias/stencil mask PSO variant 还会在绑定路径同步 `CreateGraphicsPipelineState`，且 map 没有 budget/LRU。

**目标方案**

- 构建/打包阶段生成 D3D12 bytecode 与 reflection；
- 发布包不再链接或初始化完整 glslang/SPIRV-Cross runtime，或仅保留开发 fallback；
- 生成场景/资源包级 PSO manifest，加载阶段批量预热；
- 评估 Pipeline Library/统一 cache blob，避免大量小 `.pso` 文件；
- dynamic PSO key 量化为真实有限集合，设置 budget/LRU；
- 受支持硬件上 A/B DXC/DXIL，不以“新 Shader Model”直接假设更快。

**门禁**

- 冷启动不发生 runtime GLSL/SPIR-V/HLSL 编译；
- 首个可交互帧和峰值 Private Bytes 明显改善；
- PSO 创建不出现在 draw 热路径；
- bytecode/PSO cache identity 包含 adapter、driver、schema 和完整渲染状态；
- DXBC/DXIL 必须跨目标 GPU 做画面、性能和功耗 A/B。

### P1-3：并行 command-list recording 与通用批处理

**代码证据**

- D3D12 已能创建 secondary bundle，但 pipeline 未形成常规并行录制；
- `beginRenderPass()` 直接忽略 `secondaryCBs` 与 `secondaryCBCount`；
- 已有 `ExecuteIndirect` 和受控 auto-instancing 只覆盖特定兼容路径/Shader；
- 当前主要渲染仍是一条 RenderQueue -> Validator/Agent -> D3D12 CommandBuffer 流。

**目标方案**

1. 先按 shadow、opaque、transparent、UI 等 pass 切分 direct command list；
2. 每个录制线程拥有独立 allocator/list、descriptor page、scratch arena 和 resource-state journal；
3. 主线程只做依赖排序、必要 fixup 和一次批量 submit；
4. 静态或高度重复 draw run 使用可复用 bundle；
5. 将受控实例化从 `legacy/standard` 扩展为基于明确 Pass opt-in 和兼容 key；
6. 进一步评估 GPU culling/indirect，但不在本轮第一批实现。

D3D12 的核心 CPU 优势来自可并行录制多个 command list；Microsoft 文档也明确指出 command-list building 是主要 CPU 成本之一：<https://learn.microsoft.com/en-us/windows/win32/direct3d12/command-queues-and-command-lists>。

**门禁**

- 仅在单线程 command recording 已确认是瓶颈的场景启用；
- wall-clock render submit time 下降，而总 CPU energy/frame 不显著上升；
- 每线程 allocator/list 固定池化；
- state fixup command list 不随线程数线性增长；
- transparent/order-dependent pass 保持顺序。

### P2：CPU 数据结构和边界热点

这部分应在 P0 聚合计数器确认后再做，不能替代结构性改造：

- resource retention 的 `unordered_set + ComPtr vector` 改为 backing generation stamp 或 frame arena；
- resource-state journal 从通用 hash map 改为 dense resource ID + small sparse overrides；
- barrier/operation 临时 `vector` 使用 CommandBuffer scratch arena/SmallVector；
- submission state fixup allocator/list 改为 fence-safe pool，禁止 submit 时创建；
- Query/texture readback 使用 pooled readback pages 和异步 ticket；
- dynamic PSO map、sampler/static-table cache 设置显式容量和生命周期；
- 只在场景卸载、内存压力或高水位连续下降时 trim，绝不每帧 `shrink_to_fit/rehash(0)`。

## 4. 当前未提交内存实验的逐项判断

| 实验 | 判断 | 原因 | 建议 |
|---|---|---|---|
| 完成后释放 HLSL source | 保留候选 | 生命周期明确，稳态内存收益直接 | 补重编译/设备重启测试 |
| diagnostics off 时跳过 DXBC hash map | 保留候选 | 默认路径不再保留 bytecode 副本 | 保证专项诊断开启时功能不变 |
| scheduler 完成/取消后清 callable | 保留候选 | 释放 captured source，风险较低 | 补并发 cancel/demand test |
| shader workers 3 -> 1 | 需 A/B | 降峰值但可能延长启动并增加总能耗 | 同测冷启动时间、峰值内存、J/startup |
| frames in flight 3 -> 2 | 需 A/B | 可降资源副本，但可能增加 acquire wait | 以 wait p99、GPU idle bubbles 决定 |
| GPU descriptor/frame 65536 -> 512 | 暂停 | 与历史 5,115 copy/frame 数量级冲突，易 overflow/heap switch | 先加 demand counter，以 p99+余量定容 |
| sampler 2048 -> 16 / CPU heap 16384 -> 64 | 暂停 | 没有场景级峰值证据，overflow 会造 heap | 使用自适应 page 和长期稳定 primary heap |
| 每帧清空并释放 upload pages | 回退方向 | 稳态上传会每帧重建 committed resource | fence ring，保留高水位页，压力时 trim |
| 每帧 `shrink_to_fit/rehash(0)` | 回退方向 | 反复 heap allocation、cache miss 和锁开销 | frame arena 或容量复用，低频 trim |
| 64 KB shared upload pool | 原型需重构 | append-only、无 free/分页，满后永久 fallback；迁移还从 UPLOAD 内存做 CPU read | 分页 slab/ring + fence/free list；迁移用 shadow/GPU copy |
| lazy uniform DEFAULT resource | 当前未生效 | `isUniformOnlyCandidate()` 未被调用，`uniformOnlyDeferred` 没有置 true | 先修契约和测试，再衡量是否仍需此分支 |
| indirect command capacity 4096 -> 1024 | 需更新测试并 A/B | 可降 reserved upload，但 >1024 draw 会多次 flush | 按历史 run 分布或 growable ring 定容 |

当前静态测试结果为：

- `d3d12_perf_static_test.py`：76 项中 73 通过、3 失败；
- `D3D12HotPathStaticTest.py`：24 项中 23 通过、1 失败。

失败分别对应三帧契约、CPU descriptor heap 容量契约、显式 D3D12 API 选择，以及 indirect command capacity 契约。实验阶段可以临时违反旧常量断言，但只有在新设计经 A/B 验证后才能更新测试；不能先删门禁再证明优化。

## 5. 推荐实施顺序

| 阶段 | 工作 | 预计主要收益 | 复杂度 | 是否阻塞后续 |
|---|---|---|---|---|
| 0 | 建立 D3D12/GLES3 公平基线和聚合 counters | 防止错误优化 | M | 是 |
| 1 | 撤除每帧 shrink/free，恢复稳定池；评估当前低风险 Shader 内存改动 | CPU、功耗、P99 | S-M | 是 |
| 2 | 持久 upload ring + allocator/list pool + 批量上传 | 启动、流式、功耗 | L | 是 |
| 3 | descriptor heap/page 与 dynamic CBV 重构 | 稳态 CPU、功耗 | L | 是 |
| 4 | placed-resource allocator，先 buffer 后 FrameGraph transient alias | 内存、对象开销 | XL | 否 |
| 5 | waitable swapchain 与能效 pacing | 功耗、延迟、稳定性 | M | 否 |
| 6 | native Render Pass/discard/resolve | GPU 带宽、功耗 | L | 否 |
| 7 | 离线 Shader/PSO pipeline | 冷启动、峰值内存 | XL | 否 |
| 8 | 并行 recording、通用 batching/GPU-driven | CPU 吞吐 | XL | 否 |

最短可执行路径是先做阶段 0～3。这四步完成后，才能判断剩余差距主要在内存系统、GPU bandwidth，还是 command recording。

## 6. Evidence → Finding → Path

### Evidence

| ID | 不可变观察 | 复现命令 |
|---|---|---|
| E-001 | 普通 D3D12 buffer/texture 使用 committed resource，后端不存在 placed-resource allocator | `rg -n "CreateCommittedResource|CreateHeap|CreatePlacedResource|GetResourceAllocationInfo" native/cocos/renderer/gfx-d3d12` |
| E-002 | 每次 immediate texture upload 创建 allocator/list、执行并 signal | `rg -n "CreateCommandAllocator|CreateCommandList|ExecuteCommandLists|Signal" native/cocos/renderer/gfx-d3d12/D3D12Device.cpp` |
| E-003 | 当前工作区在帧路径执行多次 shrink/rehash，并释放 upload pages | `rg -n "shrink_to_fit|rehash\(0\)|uploadPages.clear" native/cocos/renderer/gfx-d3d12` |
| E-004 | 动态 descriptor 仍有 apply/copy/restore 路径 | `rg -n "applyDynamicOffsets|restoreDynamicOffsetDescriptors|CopyDescriptorsSimple" native/cocos/renderer/gfx-d3d12` |
| E-005 | Swapchain 使用 Present(0,0)，没有 waitable-object/maximum-latency API | `rg -n "Present\(|FRAME_LATENCY_WAITABLE|SetMaximumFrameLatency|GetFrameLatencyWaitableObject" native/cocos/renderer/gfx-d3d12` |
| E-006 | GFX DISCARD 未映射，未使用 native D3D12 Render Pass | `rg -n "LoadOp::DISCARD|BeginRenderPass|EndRenderPass" native/cocos/renderer/gfx-d3d12/D3D12CommandBuffer.cpp` |
| E-007 | `beginRenderPass` 忽略传入的 secondary command buffers | `rg -n "secondaryCBs|secondaryCBCount" native/cocos/renderer/gfx-d3d12/D3D12CommandBuffer.cpp` |
| E-008 | Shader cache miss 运行时走 glslang、SPIRV-Cross、D3DCompile | `rg -n "GlslangToSpv|CompilerHLSL|D3DCompile" native/cocos/renderer/gfx-d3d12/D3D12Shader.cpp` |
| E-009 | 当前实验态有 4 个静态契约失败 | 运行两个 Python 测试模块并逐个捕获 `test_*` 结果 |
| E-010 | D3D12 未更新引擎 `MemoryStatus`，GLES3/Vulkan 有统计 | `rg -n "getMemoryStatus|_memoryStatus" native/cocos/renderer/gfx-{d3d12,gles3,vulkan}` |

### Findings

| ID | 结论 | 证据 | 置信度 |
|---|---|---|---|
| F-001 | D3D12 内存竞争力的首要缺口是缺少显式子分配和 transient alias，而不是 heap 常量偏大 | E-001、E-010 | 高 |
| F-002 | 上传路径存在按 texture 创建和提交 D3D12 对象的结构性启动/流式开销 | E-002 | 高 |
| F-003 | 当前每帧 trim 实验会制造 allocator/driver churn，与性能和功耗目标冲突 | E-003、E-009 | 高 |
| F-004 | Descriptor CPU 成本尚未完全收敛，继续缩 heap 会放大而不是解决该问题 | E-004、E-009 | 高 |
| F-005 | 当前帧调度无法直接利用 DXGI readiness，功耗对比不具备最优条件 | E-005 | 高 |
| F-006 | attachment load/store 信息没有完整传给 D3D12，GPU 带宽优化空间明确存在 | E-006 | 高 |
| F-007 | 当前未利用 D3D12 相比 GLES3 最重要的多线程 command recording 能力 | E-007 | 高 |
| F-008 | 发布版 runtime Shader 翻译链是冷启动、峰值内存和能耗的共同成本 | E-008 | 高 |

### Optimization path

```text
公平基线与 counters
  -> 停止每帧内存抖动
  -> upload ring / 批量上传 / Copy Queue
  -> descriptor 稳定 heap + dynamic root CBV
  -> placed-resource allocator + transient alias
  -> waitable swapchain + native Render Pass
  -> offline Shader/PSO
  -> parallel recording / wider batching
  -> 在同画面下验证 D3D12 同时优于 GLES3
```

这条路径先消除会污染测量的反向优化，再解决 D3D12 必须由引擎显式承担的内存、上传和绑定成本，最后才扩大 API 特有能力。这样能避免继续在单个常量或 hash cache 上消耗大量轮次，却无法形成跨性能、功耗和内存的系统优势。

