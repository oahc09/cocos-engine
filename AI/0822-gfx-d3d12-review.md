# gfx-d3d12 渲染后端代码审查报告

- 审查日期：2026-08-22（同日修订：吸收人工复核意见，重新分级）
- 代码基线：本报告描述 `8a50662161`（centralize format mappings）、`75ac03961e`（harden resource view mappings）**之后**的代码状态，早于该基线的已修复问题不计入遗留项
- 审查范围：`native/cocos/renderer/gfx-d3d12/`（约 1.7 万行自研代码，不含第三方 D3D12MemAlloc v3.2.0）
- 审查方式：按 5 个子系统并行深度审查（设备/交换链、命令缓冲/状态跟踪、资源/内存、描述符/根签名、着色器/PSO），关键结论已做源码交叉验证
- 标注说明：✅ 表示已亲自核实源码；未标注项来自子系统审查，建议复核
- 分级结论：**当前没有已确认的新 P0**。原 High 列表经复核后，部分降级为 P1（需 runtime 验证）或 P2，详见第六节

---

## 一、P1 —— 已确认问题与需 runtime 验证的风险（原 High，按复核证据修订）

### 1. ✅ Present 忽略 vsync 设置【P1】
`D3D12Swapchain.cpp` L48-L49 将 sync interval 定义为 `constexpr UINT D3D12_PRESENT_SYNC_INTERVAL = 0`，L208 以 flags=0 直接使用，整个文件没有任何对 `_vsyncMode` 的引用——上层的垂直同步设置无效。实际表现取决于窗口模式与 DWM 合成器（合成路径下未必可见撕裂），不能断言所有模式都撕裂，但「设置被忽略 + 不限帧」本身已违反 gfx-base 契约。

**修复方向**：present 时按 `_vsyncMode` 映射，需覆盖 gfx-base `VsyncMode` 全部枚举值（`GFXDef-common.h` L535：OFF / ON / RELAXED / MAILBOX / HALF），而非仅 ON/OFF：OFF→syncInterval=0（先经 `CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)` 探测再加 ALLOW_TEARING flag）、ON/RELAXED→1、HALF→2、MAILBOX 在 DXGI flip 模型下无独立队列，退化为 ON（按枚举注释的降级约定）；无法精确表达的值需按枚举定义的 fallback 链处理并告警。

### 2. ✅ 计算管线（Compute Pipeline）未实现【待专项实现】
`D3D12PipelineState.cpp` 中 compute PSO 被显式拒绝（`CC_LOG_ERROR` + 提前返回）。**复核修正**：`D3D12Device.cpp` L342 已显式设置 `Feature::COMPUTE_SHADER = false`，caps 上报是如实的，上层可据此提前规避——原文「没有 feature 关闭位」的说法过时，不成立。剩余问题是与 Vulkan/GLES3 后端的功能差距本身。

**修复方向**：专项补齐 compute PSO + Dispatch 路径（描述符/root signature 侧 UAV 逻辑已有基础），完成后再打开 feature 位。

### 3. ✅ 跨 CommandBuffer 资源状态：机制已存在，需 runtime 验证【P1·验证项】
每个 `CommandRecordingContext` 持有独立的 `D3D12ResourceStateJournal`，submit 时 `commit()` 回写 `backing->states`。**复核修正**：`D3D12ResourceState.cpp` L191 的 `appendSubmissionFixupBarriers()` 已在提交时校验 backing/epoch/subresource 数量，并按「committed → 本 CB 记录的 initial state」逐子资源生成修复 barrier，配合 commit 机制覆盖了顺序提交场景——原「缺乏全局真值」的 High 定级证据不足。剩余风险是多 CB 对同一资源交错做细粒度（分子资源）状态转换时的正确性，属静态分析无法确认的部分。

**验证方向**：编写多 CB、同资源交错状态转换的 runtime 测试（含 secondary CB `execute()` 路径），开 debug layer 观察 barrier 校验错误。

### 4. ✅ Buffer 更新语义：条件风险，需时序测试【P1·验证项】
`D3D12Buffer.cpp`（已核实设计注释）：DEFAULT 堆 buffer 的 `update()` 只保留**最新一份** `pendingData`，由 flush 统一录制拷贝。**复核修正**：flush 会在多个命令录制节点触发，若两次 update 之间发生了 flush，则各自生效——不能直接断言所有「同帧 update→draw→update→draw」都退化为 last-write-wins，只有落在同一 flush 窗口内的二次 update 才会覆盖。另外 UPLOAD 堆路径的 Map/memcpy/Unmap 无锁，多线程调用 `update()` 是数据竞争（单线程录制约定下不触发）。

**验证方向**：增加时序测试覆盖「同 flush 窗口内二次 update」场景，确认引擎上层是否存在该用法；可在 flush 前检测并告警。

### 5. ✅ copyBuffersToTexture 压缩格式路径：仅测试缺口，无越界证据【P1/P2·验证项】
**复核修正**：`D3D12CommandBuffer.cpp` L4580-L4606 已核实——footprint 来自 `GetCopyableFootprints`（封装在 `getD3D12TextureUploadFootprint`），copyRows 按 `formatAlignment` 的 block height 折算并被 `rowCount` 钳制，逐行 memcpy 宽度取 `min(copyRowBytes, rowSizeInBytes)`。静态看不存在越界写，原 High 定级（内存安全）证据不足。剩余为测试缺口：BCn/ASTC 各 block 尺寸、非 block 对齐宽高、3D/array 纹理未经系统性实测。

**验证方向**：validation layer + 各类压缩纹理、非对齐尺寸实测。

### 6. ✅ Swapchain resize / 设备丢失（TDR）错误处理不完整【P1】
- `ResizeBuffers` 失败后（`D3D12Swapchain.cpp` L292 附近）尝试重建 RTV 恢复，但未区分 `DXGI_ERROR_DEVICE_REMOVED/RESET`，无设备恢复路径，swapchain 也未标记为不可用，后续 present 会持续失败。
- `waitIdle` 检测到 device removed 后仅记日志返回 false，没有向上层传播「需要重建设备」的信号。Windows 上 TDR 并不罕见（驱动升级、超时），当前实现会表现为黑屏或崩溃且不可恢复。

### 7. ✅ 着色器编译等待无超时【P2】
**复核修正**：`s_inFlightDXBCCompiles`（`D3D12Shader.cpp` L1033）由 `s_inFlightDXBCMutex` 保护，per-compile 状态由各自的 mutex + condition_variable 保护，`finishInFlightDXBCCompile` 的 erase 会校验 `iter->second == ticket.compile` 后才移除——原报告描述的「erase 与 shared_ptr 释放竞态窗口」未被证实，不成立。真实的剩余风险是 `waitForInFlightDXBCCompile` 的 `wait` 无超时：owner 线程异常退出（未走到 finish）会永久挂起等待方。建议改为 `wait_for` + 超时降级为本线程自行编译，并保留 TSan 压测。

### 8. ✅ UAV barrier：设计意图明确，需 runtime 测试确认【P2·验证项】
**复核修正**：`needsD3D12UavOrderingBarrier()`（`D3D12ResourceState.cpp` L257）的职责是专门处理**未发生 state transition** 的 UAV 写序（前后都停留在 `UNORDERED_ACCESS` 时补 UAV barrier）；发生 state 变化的写后读/写后写由 transition barrier 本身保证顺序。原「条件受限 → 可能遗漏」的判断仅凭静态条件，偏武断。剩余为验证项：UAV 写后读、写后写、同 subresource 交错场景需 runtime 测试（compute 补齐后优先级上升）。

---

## 二、Medium —— 功能缺口与潜在缺陷（最终分级见第六节）

| # | 问题 | 位置 | 说明 |
|---|------|------|------|
| 9 | Root signature 无 64-DWORD 成本核算 | ✅ `D3D12PipelineLayout.cpp`（全文无校验） | set 数量多时 Serialize 失败，仅有事后 HRESULT 日志；建议序列化前预估成本 |
| 10 | InputAssembler 缓存的 VBV/IBV 在 buffer resize 后可能过期 | `D3D12InputAssembler.cpp`（indirect 执行见 `D3D12CommandBuffer.cpp` L3904 / L4376 / L4382） | ✅ 复核修正：indirect draw **已实现**——command signature 已创建，常规 draw 与 local root-CBV batch 两条路径均有 `ExecuteIndirect`，非支持缺口；剩余风险为 resourceVersion 检测与 `refreshBufferViews()` 刷新时机是否覆盖所有 draw 前路径（含 indirect buffer 引用）的 runtime 验证 |
| 11 | Dynamic offset 无 256 对齐校验、与 slots 顺序无断言 | `D3D12DescriptorSet.cpp` | CBV BufferLocation 非 256 对齐是未定义行为；建议加断言 |
| 13 | `AccessFlagBit::SHADING_RATE` 未映射到 `D3D12_RESOURCE_STATE_SHADING_RATE_READ` | ✅ 全后端 grep 无匹配 | VRS 功能启用时才触发，当前为休眠缺陷 |
| 14 | QueryPool readback 的 fence 同步边界与析构时序 | `D3D12QueryPool.cpp` | 建议析构时等待未完成查询 |
| 15 | Queue::submit 中 Signal 失败后状态不一致 | `D3D12Queue.cpp` | 应设置设备错误标志阻断后续提交 |
| 16 | DescriptorHeapPool / DescriptorSet 脏标记均无锁 | ✅ pool 中无任何 mutex | 当前单线程录制下安全，但属于未受保护的隐含约定，建议文档化或加锁 |
| 17 | Root signature 无全局缓存去重 | `D3D12PipelineLayout.cpp` | 已有 `rootSignatureHash` 但未用于查表，重复创建浪费 |
| 18 | LINE_LOOP / TRIANGLE_FAN 被近似为 LIST | `D3D12PipelineState.cpp` | 与 GL 系后端渲染结果不一致，属静默降级 |
| 19 | Framebuffer 依赖 generation 计数防悬空，但 `getRTVHandle()` 在 swapchain 为空时静默返回空句柄 | `D3D12Framebuffer.cpp` | 机制本身合理（快照+generation 校验），但失败路径无错误上报，排查困难 |

---

## 三、Low / 其他观察

- 热路径上 `flushDescriptorSetsIncremental` 存在多次 unordered_map 查找与 `SetBindingInfo` 内 vector 分配，draw call 密集场景有性能开销。
- Null descriptor 填充在多条路径中实现方式不一致（dummy buffer CBV / dummy texture SRV / `CreateConstantBufferView(nullptr, ...)`），建议统一封装。
- Buffer/Texture 销毁后 DescriptorSet 中仍保留裸指针引用，依赖上层不再 update 已销毁资源，建议在 update 时做有效性（generation）校验。
- QueryPool 的 `activeIndices`/`completedIds` 若长期不 reset 会缓慢增长。
- Shader 文件缓存（v3→v4 迁移）无损坏检测与恢复机制。
- Present 失败（`DXGI_ERROR_ACCESS_LOST` 等）仅记日志，未标记 swapchain 不可用。

---

## 四、误报澄清（子审查中提出、经核实不成立或已缓解）

1. **「nextSubpass 未实现」不成立** ✅：`D3D12CommandBuffer.cpp` L4122 已实现 `nextSubpass()`，含 subpass RT 重绑，并对不可表达的 input/color 自依赖给出警告。MSAA resolve 也有 `ResolveSubresource` 调用（两处）。subpass 支持是「有限但存在」，非缺失。
2. **「register space 与着色器反射不匹配」不成立** ✅：root signature 侧 `RegisterSpace = setIndex`（`D3D12PipelineLayout.cpp` L180 等），shader 侧 SPIRV-Cross 显式配置 set=N→space=N，两端约定一致。这是脆弱的双边约定（无集中定义），但当前是对齐的。
3. **「uboOffsetAlignment 硬编码 256 需查询」不成立**：D3D12 的 CBV 对齐就是固定常量 `D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT`(256)，硬编码正确。
4. **「DescriptorSet 销毁立即回收导致 GPU 悬空」基本缓解** ✅：`doDestroy()` 回收的是 CPU staging（非 shader-visible）堆；GPU 实际读取的是每帧 ring 式 GPU heap 中的拷贝，帧 fence 保护其生命周期。
5. **initializeD3D12Context 的历史修复仍然有效** ✅：所有失败路径调用 `cleanupContext`，WARP 走 `EnumWarpAdapter`，D3D12MA allocator 在上下文稳定后、adapter 非空守卫下创建。资源 backing 的统一拥有者架构（resource 先于 allocation 析构、resize 延迟释放）也确认被遵守。
6. **「Local Root CBV 的 ShaderRegister=0 可能错配」已缓解** ✅：分区逻辑要求 `binding.binding == 0` 才会选为 local root CBV（`D3D12PipelineLayout.cpp` L213-L214），与 `ShaderRegister = 0` 配对正确。
7. **「Depth clamp 未映射」不成立（原 #12，修订时移入本节）** ✅：gfx-base 的 `RasterizerState` 只有 `isDepthClip` 字段（`GFXDef-common.h` L1514），不存在 `depthClamp` 字段，D3D12 侧用 `isDepthClip` 驱动 `DepthClipEnable` 已是完整映射，无漏映射对象。
8. **「in-flight 编译 map 存在 erase/shared_ptr 竞态」未被证实（原 #7 描述，已修订）** ✅：map 有 `s_inFlightDXBCMutex` 保护，erase 前校验指针相等；真实风险仅为等待无超时（见第一节 #7）。

---

## 五-1、本轮审查期间已发现并修复的 P0

以下问题在本次审查过程中实际发现，修复已包含在提交 `75ac03961e`（fix(d3d12): harden resource view mappings）中，不再计入遗留风险：

- **TEX1D / TEX1D_ARRAY / TEX2D_ARRAY 的 resource / SRV / UAV 维度类型映射错误**（`D3D12Texture.cpp`、`D3D12DescriptorSet.cpp`）。
- **Storage image view 的 mip / layer 范围未按 view 信息裁剪**（`D3D12DescriptorSet.cpp`）。
- **Root parameter visibility：单阶段 stageFlags 退化为 ALL**（非 P0，同在 `75ac03961e` 修复）：`toD3D12ShaderVisibility()`（`D3D12PipelineLayout.cpp` L67-L88）由 `hasAnyFlags(stageFlags, ALL)`（任一阶段位即命中，恒返回 ALL）改为 `hasAllFlags` + 逐阶段计数，单阶段可精确映射到 VERTEX/PIXEL/GEOMETRY/HULL/DOMAIN；多阶段与 COMPUTE 保留 ALL（后者为 D3D12 语义要求）。建议保留回归测试项。

相关前置重构（格式映射集中化）在 `8a50662161` 中完成。

---

## 五、确认无问题的架构要点

- **统一 backing 拥有者**：`D3D12ResourceBacking` 同时持有 `ComPtr<ID3D12Resource>` 和 `ComPtr<D3D12MA::Allocation>`，声明顺序保证 resource 先释放；析构函数 out-of-line 定义。
- **resize 延迟释放**：旧 backing 由 shared_ptr 延迟到所有消费者（views/命令缓冲）释放引用后销毁，命令录制上下文按帧 fence 回收。
- **分配策略**：优先 D3D12MA placed resource，失败回退 committed；设备销毁前用 `GetBudget()` 检测 allocation 泄漏。
- **frame-in-flight 设计**：backbuffer 数量 = frame-in-flight 数量（2），per-frame descriptor heap / upload page 由 acquire 时 fence 等待保护。
- **Queue::submit 提交顺序**：多 CB 按输入顺序传给 `ExecuteCommandLists`，fence signal 在全部执行之后。
- **RenderPass clear 语义**：`beginRenderPass` 正确区分 loadOp CLEAR/LOAD。
- **Secondary CB 嵌套限制**：`execute()` 正确阻止嵌套执行。

---

## 六、最终分级与处理建议（修订版）

- **P0**：当前没有已确认的新 P0（本轮实际发现的 P0 已在 `75ac03961e` 修复，见第五-1 节）。
- **P1**：vsync（#1）、Resize/TDR 错误传播（#6）、跨 CB 状态 runtime 验证（#3）、同 flush 窗口 buffer update 时序验证（#4）、动态 offset 对齐校验（#11）与压缩上传实测（#5）、primitive topology 近似（#18，静默降级需至少告警）。
- **P2**：root signature 缓存去重（#17）、descriptor/录制单线程并发约定的断言与文档化（#16）、着色器编译等待超时（#7）、UAV barrier runtime 验证（#8）、查询池同步/增长（#14）及热路径性能问题（第三节）、SHADING_RATE 映射（#13，休眠缺陷）。
- **待专项实现**：Compute PSO + Dispatch（#2）、UAV storage buffer 完整路径、真正的 TRIANGLE_FAN / LINE_LOOP 拓扑转换。（root visibility 已修复，见第五-1 节，仅保留回归测试；indirect draw 已实现，非缺口，见 #10 修订。）
- **持续**：把「单线程录制」「set=space 约定」「descriptor 回收依赖帧 fence」这几条隐含约定写成断言或文档——本次审查发现的最大共性风险是**大量正确性依赖未受保护的隐含前提**。
