# gfx-d3d12 渲染后端支持历程与技术复盘

> 文档日期：2026-08-01  
> 代码范围：`native/cocos/renderer/gfx-d3d12/` 及与其直接相关的 GFX、渲染管线、Win32 帧循环和测试代码  
> 目标版本：Cocos Creator 3.8.8 定制引擎  
> 当前结论：D3D12 后端已完成项目 test cases 的效果对齐，画面与 GLES3/Vulkan 一致；在目标测试环境中性能达到可交付水平。

## 1. 写在前面

gfx-d3d12 不是一次“照着 Vulkan 接口翻译成 D3D12”的短期移植，而是一段从最小 PoC、完整材质管线、渲染正确性，到资源生命周期、CPU 热路径和场景级性能逐层收敛的工程过程。

从 2026-04-25 首次提交 Windows D3D12 后端，到 2026-08-01 完成正确性与资源生命周期加固，仓库中共有 28 个提交直接修改 `gfx-d3d12`，当前后端由 38 个 C++ 头/源文件、约 1.82 万行代码构成。期间经历过黑屏、GPU validation error、UI 顶点错乱、半透明与粒子异常、RenderTexture/离屏渲染失败、CSM 阴影错误、首次加载卡顿、数 FPS 的高 draw-call 场景、descriptor 生命周期错误、重启崩溃风险等问题。

最终得到的不只是“能跑”的后端，还包括一套相对完整的工程资产：

- D3D12 Device、Swapchain、Queue、CommandBuffer 和全部主要 GFX 资源对象；
- GLSL/SPIR-V 到 HLSL/DXBC 的材质 Shader 编译链路；
- Root Signature、PSO、Descriptor、资源状态和 Frame Resource 管理；
- RenderDoc、D3D12 Debug Layer、静态契约测试和 C++ 单元测试组成的验证体系；
- Shader/PSO 缓存、descriptor 精确复用、批量绘制、自动实例化和 Win32 单一帧节流等性能方案；
- 对失败方案、测量误区和 D3D12 特有语义陷阱的可复用经验。

需要特别说明：这里的“完成”是指当前项目 test cases 的渲染效果和目标性能已经满足验收，不等于 D3D12 所有可选能力均已实现。例如 Compute 在完整执行链路具备之前选择 fail-closed，不对外虚假声明支持。这种边界控制本身也是本轮工程成熟度的一部分。

## 2. 最终交付概览

| 维度 | 最终状态 |
|---|---|
| 后端接入 | Windows 可编译并显式选择 D3D12，Device/Swapchain/Queue/CommandBuffer 链路完整 |
| 基础渲染 | 清屏、三角形、普通 2D/3D、材质、纹理、Sampler、UBO、索引绘制可用 |
| 高级场景 | 半透明、粒子、RenderTexture、离屏 framebuffer、阴影/CSM 等项目 test cases 已对齐 |
| Shader | SPIRV-Cross 转 HLSL，D3DCompile 生成 DXBC，具备反射、缓存和编译调度 |
| 资源管理 | 三帧资源、fence 驱动复用、提交上下文保活、owner/view 共享 backing、资源状态按子资源追踪 |
| 性能 | 高 draw-call 场景从早期数 FPS 收敛到接近 60 Hz；历史正式样本 58.767 FPS，归因短样本 59.650 FPS |
| 批处理 | 2,500 逻辑 draw 可形成一次 native-IA-compatible `ExecuteIndirect` 批次；支持受控自动实例化 |
| 回归资产 | 76 个 D3D12 性能/契约静态测试、23 个热路径静态测试、50 个 D3D12 C++ 单元测试 |

## 3. 开发历程

### 3.1 第一阶段：从零接入到最小清屏（2026-04-25）

首个阶段的目标不是立即实现完整渲染，而是建立一条可验证的最小闭环：

1. 在 GFX API 枚举和 TS 定义中加入 D3D12；
2. 增加 `CC_USE_D3D12` CMake 开关并挂入源文件；
3. 在设备工厂中加入 D3D12 创建分支；
4. 建立 Device、Swapchain、Queue、CommandBuffer 和资源对象骨架；
5. 从 Win32 句柄创建 DXGI swapchain；
6. 完成 command allocator/list、fence、backbuffer 状态转换和 Present；
7. 在 WebGPUDemo 中完成真实窗口清屏、运行稳定性和退出路径验证。

这一阶段最大的坑不是 API 调用本身，而是“PoC 看起来成功”和“真实运行链路正确”之间的差异。早期曾出现程序能创建 D3D12 Device、也能 Present，但在运行或退出阶段崩溃的问题。排查中逐步排除了 Present/fence，最终将问题收敛到资源与对象生命周期。PoC 因此从一开始就加入了真机运行、soak 和退出验证，而不是仅以编译通过作为完成标准。

对应提交：`0a010b745f`、`fbaeff133a`。

### 3.2 第二阶段：三角形与完整材质管线（2026-04-29 ～ 2026-04-30）

4 月 29 日跑通第一个 D3D12 三角形，随后立即进入真正困难的材质适配。Cocos 上层提交的是跨后端 Shader 和 GFX 资源描述，D3D12 需要把它们落到一组强约束对象上：

- 使用 SPIRV-Cross 将 GLSL/SPIR-V 转换为 HLSL；
- 使用 D3DCompile 生成 VS/PS DXBC；
- 保存 Shader reflection 与 binding 信息；
- 将 DescriptorSetLayout/PipelineLayout 映射为 Root Signature；
- 建立 shader-visible CBV/SRV/UAV heap 与 sampler heap；
- 将 Rasterizer、Blend、DepthStencil、InputLayout、RT/DS format 组合成 PSO；
- 在 CommandBuffer 中正确绑定 heap、root descriptor table、PSO、IA 并发出 draw/drawIndexed。

这时遇到的黑屏问题表面上都很相似，但原因完全不同：descriptor heap 被提前 reset、descriptor table 已失效、framebuffer RTV 指向错误资源、command allocator 在 GPU 尚未完成时被重用、copy/blit/resolve/acquire 仍是空实现等。项目因此进行了一轮后端审查，将问题按 Critical/Important/Minor 分级，先修会导致“画面完全不可解释”的基础错误，再继续扩展效果。

对应提交：`46d29f8533`、`c6a3fd3534`、`042af2e78d`。

### 3.3 第三阶段：从“3D 能显示”到常用场景正确（2026-05-02 ～ 2026-05-11）

当普通 3D 场景能够显示后，问题开始表现为局部语义差异：

- 2D UI 顶点或索引异常；
- 半透明混合不正确；
- 粒子效果异常；
- Debug 构建不显示；
- 不同 test case 中出现零散画面差异。

其中最典型的是 2D UI。`D3D12Buffer::resize()` 创建了新资源，但已经创建的 InputAssembler 仍持有旧 GPUVA 和旧 Size。CPU 侧数据看似已经更新，draw 仍然从旧地址取数据，RenderDoc Mesh Viewer 甚至可能看不到有效顶点/index。修复不是简单补一次 upload，而是在资源 resize 后刷新原生 VBV/IBV，保证 view 与 backing resource 同步。

半透明问题则推动我们逐项核对 BlendFactor、BlendOp、ColorWriteMask、blend constants、alpha-to-coverage 和 sample count，而不是把所有透明异常归因到“排序”。粒子问题进一步暴露出 PSO 状态映射和 Shader 变体组合的边界。

这一阶段也做了第一轮性能审查，识别出 Queue submit 同步等待、descriptor 每帧重建、每 draw `forceUpdate()`、所有 Buffer 使用 UPLOAD heap、无帧流水线、纹理上传反复创建 committed resource 等结构性问题。部分低风险热路径分配和诊断 I/O 被立即移除，更大的同步/内存模型调整留到后续有测量基线后处理。

对应提交：`5efb67be19`、`d84963b8b3`、`7c5cee638e`、`c8582e63ec`、`28a54e46e8`。

### 3.4 第四阶段：RenderTexture 与离屏渲染（2026-05-17）

离屏渲染是后端从“只会向 swapchain 画”走向通用渲染的重要门槛。连续几轮修复集中在：

- 非 swapchain Texture 的 RTV/DSV 创建；
- texture view 的 mip/layer 定位；
- framebuffer attachment 与真实资源身份；
- render pass 开始/结束时的资源状态；
- RenderTexture 作为输出后再被采样；
- attachment 尺寸、格式与 sample count 的一致性。

早期代码曾通过尺寸/格式去猜“最近创建的匹配纹理”，这在只有一个 RT 时可能工作，但多个相同规格 RT 共存时会把渲染写到错误纹理。最终原则是：attachment identity 必须来自上层明确传入的对象，不能在后端做模糊替换；非法 framebuffer 要明确失败，而不是静默修补。

对应提交：`8a08b44a59`、`beaef1c3a4`、`c9b0925631`、`75743ad777`。

### 3.5 第五阶段：阴影与 CSM 的深水区（2026-05-22 ～ 2026-05-24）

阴影问题是整个正确性阶段最耗时的案例之一。最初围绕 Y-flip、深度编码、viewport/scissor、资源状态、CB register、descriptor 绑定时序等方向逐一排除。RenderDoc 证明阴影 pass 确实执行、shadow map 也有数据，但主场景结果仍不正确。

排查中出现过一个很有迷惑性的中间结论：R32F shadow map 与 RGBA packed depth 变体不一致。Shader 在 packed 模式下把深度拆到 RGBA，但 R32F 只保存 R 通道，理论上会损失数据。继续对比 Shader 宏、format feature 和 UBO 内容后，真正决定 CSM 多 level 正确性的核心问题收敛到 UBO 更新时序。

旧 D3D12 `updateBuffer()` 对 UPLOAD heap 直接执行 `Map/memcpy/Unmap`。同一个 CCShadow UBO 在录制 level 0 draw 后又被 level 1 更新，CPU 会覆盖同一块 GPU 内存；GPU 真正执行时，前面的 draw 也读到了最后一次写入的数据：

```text
CPU: update(level0) -> record draw0 -> update(level1) -> record draw1
GPU: execute draw0 (读到 level1) -> execute draw1 (读到 level1)
```

GLES3 的 `glBufferSubData` 和 Vulkan 的 `vkCmdUpdateBuffer` 都具有命令顺序语义，不能直接类比为 D3D12 的 CPU Map 写入。最终方案经历了三步：

1. Deferred Queue：失败。延后 Map 仍会覆盖相同 GPUVA；
2. `CopyBufferRegion + DEFAULT heap`：方向正确，但已有路径仍尝试 Map DEFAULT heap；
3. snapshot/独立 backing：每次逻辑更新使用独立数据快照和 GPU 地址，并让 descriptor 指向对应 backing；旧资源保活到 fence 完成。

后来这套方案继续演化为稳定 DEFAULT backing、fence-safe transient upload descriptor 和统一的资源保活模型，既保证每个 draw 看到正确版本，也避免每次更新都永久创建 committed resource。

对应提交：`525805bb9f`、`8e7db589b4`。

### 3.6 第六阶段：RenderTexture、粒子与能力补齐（2026-06-06 ～ 2026-06-08）

6 月初继续处理剩余 test cases，重点包括 RenderTexture、粒子 Shader/PSO、QueryPool 和 RenderPass 语义，并开始把人工发现的问题写入自动化测试。

这一阶段形成了两个重要习惯：

- 渲染修复必须同时补最小回归测试；
- 性能修复必须由静态契约约束关键路径，避免下一次重构把同步等待、重复分配或无界缓存带回来。

对应提交：`564b55d97e`、`8f7c7ff111`、`7384d272fc`、`041bf1ee3d`。

### 3.7 第七阶段：首次加载与 Shader/PSO 管线优化（2026-06-28 ～ 2026-07-13）

功能逐步完整后，首次进入场景仍然存在明显卡顿。根因不是一个函数，而是 Shader 变体和 PSO 首次出现时的同步工作叠加：

- SPIRV-Cross/HLSL/DXBC 编译；
- reflection 和 source/key 处理；
- PSO key 计算与 `CreateGraphicsPipelineState`；
- 磁盘缓存读写；
- 逐 Shader/逐 PSO 详细日志；
- 动态 variant 在 draw 热路径创建。

优化中将 Shader bytecode、PSO key、内存/磁盘 cache hit 和原生 PSO 创建分段测量，并引入编译/缓存调度器。Debug 构建保留诊断信息，但不必等同于完全未优化 codegen；持久化任务从前台关键路径移出，并用 session/lifecycle 门禁避免 Device 销毁后异步任务继续写缓存。

同时补上 optimized clear value、descriptor staging heap 恢复、查询对象复用等改进。一个关键经验是必须把“冷启动尖峰”和“预热后的稳态 FPS”分开测量，否则 cache 改动会被误判为 draw 性能变化。

对应提交：`cf4c88cf14`、`f28eb06e8e`、`56a93871ea`。

### 3.8 第八阶段：从数 FPS 到接近 60 Hz（2026-07-18 ～ 2026-07-19）

这是最大的一轮性能攻坚。高 draw-call test case 的早期 D3D12 表现只有数 FPS，后续建立受控基线后，从 descriptor、UBO、IA、command recording、validator、render queue 一直追到 Win32 外层帧循环。

#### 3.8.1 Descriptor 与 UBO

- 删除 `DescriptorSet::update()` 和 draw 前 pending flush 对同一 transient UBO 的重复上传；
- 将 local descriptor table 拆成动态 uniform 前缀和静态资源后缀；
- 缓存 key 不只比较 hash，还比较 layout、heap epoch、资源指针、descriptor version 和完整 dynamic offsets；
- 修复 static table 命中但普通 static UBO identity 未纳入 key 的问题。

R140 中，每帧 CBV/SRV/UAV descriptor copy 从 37,602 降到 5,115，减少 86.4%；static table 命中 2,499/2,505，正式 FPS 相比对应基线提升约 6.07%。

#### 3.8.2 2,500 draw 的 IA 与 `ExecuteIndirect`

最初以 C++ `InputAssembler` 对象指针判断是否能够合批，发现 2,500 个 draw 对应 2,500 个对象，于是错误地认为 IA 每次都变。进一步记录 native VBV/IBV 后发现，真实 D3D12 IA 状态只在第一个 draw 改变一次。

修复后以 native vertex/index views 判断兼容性：2,500 次尝试全部编码成功，形成 1 个 indirect batch，没有 fallback/capture failure。随后跳过等价 IA 重复绑定，validator/backend IA bind 从约 2,505 次降到 6 次，并把 local descriptor set 选择和 batch draw 编码融合。

这里的核心教训是：引擎对象 identity 不等于 GPU state identity。优化判断必须落到原生语义，否则既可能错失合批，也可能错误复用。

#### 3.8.3 真正的最后瓶颈：Win32 双重节流

当 D3D12 command path 已经大幅收敛，FPS 仍停在 56 左右。外层归因显示 forward pipeline 约 7～8 ms，`Present` 和 D3D12 `frameSync()` 也不是瓶颈。最后发现：

- `Engine::tick()` 按目标 FPS sleep；
- `WindowsPlatform::loop()` 又按同一个 FPS 做一次 60 Hz 节流。

两个节流器叠加，把调度误差累积到一帧。最终 Windows runtime 只保留 `WindowsPlatform::loop()` 作为唯一帧节流器。旧正式口径样本从 R166 的 56.167 FPS 提升到 R171 的 58.767 FPS；R172 的 5 条归因短样本为 59.650 FPS。

对应提交：`2251c9a91f`、`23c6e35c49`。

### 3.9 第九阶段：受控自动实例化（2026-07-23 ～ 2026-07-24）

在后端批处理稳定后，项目加入受控自动实例化，但没有把“看起来相似的 draw”全部激进合并，而是采用 fail-closed 设计：

- 仅对明确允许的 Shader/Pass 和连续兼容 draw run 生效；
- 合并 world matrix 等实例数据；
- 按容量拆分，实例数量与具体测试数值解耦；
- 1024 容量边界下，2,500 实例自然拆为 3 批；
- 任一布局、资源或语义条件不满足时回退原顺序绘制；
- Validator、Agent、GLES3/Vulkan 的 backend-neutral fallback 保持原有语义。

自动实例化的价值不仅是减少 draw，更重要的是把优化能力放在可证明安全的边界内，防止为了性能改变材质、排序或 descriptor 可见性。

对应提交：`79e61f0d54`、`7db6902688`、`62265a4fec`。

### 3.10 第十阶段：正确性与资源生命周期总加固（2026-07-29 ～ 2026-08-01）

性能达标后又做了一次从 D3D12 规范出发的后端审查。审查没有因为 test cases 已经“看起来正常”而停止，而是集中处理隐藏的 P0/P1 风险：

- restart 前 GPU 未 idle，资源可能仍被 GPU 使用；
- 进程级 static PSO/root signature 跨 Device 重启复用；
- Compute 被声明支持但 PSO/root binding/UAV 路径不完整；
- texture owner/view 各自保存状态，且 partial barrier 后错误覆盖整个资源状态；
- 非法 D3D12 state 组合和缺失的 Texture UAV ordering；
- static descriptor table 复用旧 CBV GPU 地址；
- MSAA resolve format、Stencil clamp/wrap、Framebuffer attachment 等确定性语义错误；
- Validator actor 所有权、Bundle/CommandList 资源保活和 fence 失败路径；
- Buffer/Texture view 在 owner resize/destroy 后悬空。

8 月 1 日的加固提交建立了共享 native backing 和 `D3D12ResourceState`，让 owner/view 对同一 `ID3D12Resource` 共享 per-subresource 状态；引入录制 journal，在提交顺序而不是录制顺序上合并状态；补齐 restart idle、设备级 cache 生命周期、提交上下文保活、attachment/resolve/format fail-closed 等修复，并新增大量 C++/静态测试。

对应提交：`4cb364f21e`。

## 4. 关键问题、根因与最终解法

| 问题 | 表面现象 | 根因 | 最终原则/解法 |
|---|---|---|---|
| Descriptor 被清空 | 黑屏、随机纹理 | heap reset 时机早于 GPU 使用完成 | frame slot + fence 驱动复用，heap epoch 纳入缓存身份 |
| CommandAllocator 过早 reset | validation error、随机崩溃 | GPU 尚在执行上一批命令 | allocator/descriptor/upload page 统一绑定提交 fence |
| UI 顶点错乱 | 2D 不显示或 mesh 无效 | Buffer resize 后 IA 仍引用旧 GPUVA/Size | backing 版本变化时刷新 VBV/IBV，view 跟随 owner backing |
| 半透明异常 | alpha 混合结果错误 | blend constants、sample state 或 blend 映射不完整 | 逐项映射 GFX BlendState，动态状态显式下发 |
| RenderTexture 错误 | 离屏为空或采样错误 RT | attachment identity 被尺寸/格式推断替换 | 资源身份精确传递，非法 framebuffer 明确失败 |
| CSM level 数据相同 | 阴影缺失/错位 | 多次 Map 覆盖同一 UBO GPUVA | 每次逻辑更新有独立可见版本，资源保活到 fence 完成 |
| R32F/packed depth 矛盾 | shadow map 有值但采样错误 | format capability、宏和 UBO packing 可能不一致 | 能力查询必须全链路一致；用 RenderDoc 验证实际 UBO/Shader 分支 |
| Particle 差异 | 粒子颜色/混合异常 | Shader variant 与 PSO state 组合不一致 | PSO key 覆盖完整渲染语义，修正状态映射 |
| 首次加载卡顿 | 首帧/首次变体尖峰 | Shader/PSO 编译、缓存 I/O、日志在前台串行 | 编译/持久化调度、内存/磁盘缓存、生产关闭逐项诊断 |
| Descriptor CPU 开销高 | FPS 低、CPU 热点 | 每 draw rewrite/copy/restore，static identity 不精确 | 动静拆分、精确 key、增量 dirty/version、root CBV 快路径 |
| 2,500 draw 无法合批 | C++ 调用和 IA bind 爆炸 | 用 actor 指针代替 native IA state 判断 | 比较 VBV/IBV，兼容 run 使用 ExecuteIndirect |
| FPS 卡在 56 左右 | 后端已快但达不到 60 | Engine 与 Win32 loop 双重 sleep | Windows runtime 只保留一个帧节流器 |
| restart 风险 | 重启后随机崩溃/UAF | teardown 前未等待 GPU，static cache 跨设备 | waitIdle 后销毁；PSO/root cache 归 Device 所有 |
| Barrier 偶发错误 | 特定 mip/layer 或 UAV 场景错误 | wrapper 级粗状态、非法 state OR、无 UAV ordering | backing 级 per-subresource tracker + fail-closed 映射 |
| 资源提前释放 | GPU device removed | command list 只保留 GPUVA/raw actor | recording context 保存 native backing，提交 fence 后退休 |

## 5. 性能优化复盘

### 5.1 测量协议比单次 FPS 更重要

历史过程中曾出现多个“优化看似倒退”或“突然只有 30 FPS”的假象。最后固定的性能协议是：

- 必须使用可见、前台且保持焦点的窗口；
- 慢加载标记后忽略 60 条 FPS 日志，再取连续 30 条算术平均；
- `formalMean >= 59 FPS` 作为验收门槛，同时记录相对 60 FPS 的差值；
- Debug Layer 60+30 只做正确性门禁，不作为性能结论；
- 正式性能样本关闭 Debug Layer；
- build、stdout、stderr、焦点状态和残留进程一起归档。

隐藏窗口会触发 SDL background/pause 路径并产生约 30 FPS 的假回归，因此这类样本已明确判无效。

### 5.2 代表性收敛节点

| 轮次 | 数据 | 结论 |
|---|---:|---|
| R58 | 28.354 / 28.181 FPS | 早期受控基线 |
| R60 | 52.003 FPS | 正常重建后证明此前结果受环境/构建影响 |
| R70 | 53.549 FPS | 删除重复 UBO 上传 |
| R80 | 52.104 FPS | 建立可见前台标准，废弃隐藏窗口数据 |
| R100 | 45.799 FPS，回退 | Root Signature 1.1 static slot 在目标驱动更慢 |
| R140 | 56.565 FPS | 精确 static UBO identity，descriptor copy -86.4% |
| R161 | 56.006 FPS，诊断 | 2,500 draw 合并为一个 indirect batch |
| R166 | 56.167 FPS | local set/batch 融合完成，后端不再是唯一瓶颈 |
| R171 | 58.767 FPS | 移除 Win32 双重帧节流后的正式旧口径样本 |
| R172 | 59.650 FPS，5 条短样本 | 外层 loop 归因，证明已接近 60 Hz |

这些数据来自不同诊断轮次，只用于展示收敛过程；严格 A/B 只能比较同一构建、同一前台协议下的样本。R172 是定位短样本，不替代 60+30 正式归档。

### 5.3 最终保留的优化

- submit/present 不在每帧后立刻等待 GPU；
- 三帧 frame resource，allocator、descriptor range、sampler、upload page 按 fence 退休；
- DEFAULT heap 稳定 backing 与 fence-safe transient upload；
- descriptor table 动静拆分、增量 dirty/version、精确资源 identity；
- root CBV 用于高频 local uniform，静态 SRV/sampler 保持 table 复用；
- command list 资源保活按唯一 backing 做常数时间去重；
- Shader 编译和 cache 持久化调度，PSO/Shader 热路径诊断默认关闭；
- 重复 graphics state/IA bind 跳过；
- draw packet 数组一次穿过 RenderQueue、Validator/Agent 到 D3D12；
- native IA compatible 的 `ExecuteIndirect`；
- fail-closed 的受控自动实例化；
- Win32 runtime 单一帧节流器。

### 5.4 被证伪或回退的方案

- 隐藏窗口采样：会进入 background/pause，数据无效；
- Root Signature 1.1 static descriptor range：功能正确但目标驱动明显变慢；
- 只增加 root descriptor：不能替代 descriptor identity 与 ordering 正确性；
- Deferred UBO Map：仍覆盖相同 GPUVA；
- 直接把全部 UBO 改为 DEFAULT heap：旧的直接 Map 路径立即失效；
- 额外 sleep guard、yield 或末段忙等：未解决双重节流，且可能增加 CPU；
- 继续微调 descriptor hash：在 R140 后已不是最后 3～4 FPS 的决定因素；
- 关闭 Validator 作为交付优化：只用于归因，不能牺牲验证层换性能结论。

## 6. D3D12 后端中最容易踩的坑

### 6.1 录制顺序不等于内存版本顺序

D3D12 command list 记录的是 GPU 命令；CPU 对 persistently mapped UPLOAD 内存的写入不会自动形成 draw 之间的版本。只要多个 draw 需要不同数据，就必须提供不同 GPU 可见地址/区域，或通过真正的 GPU copy 命令建立顺序。

### 6.2 Descriptor 是资源地址的快照，不是智能引用

Buffer resize、资源替换或 texture view 变化后，已经写入 heap 的 CBV/SRV/UAV 不会自动更新。descriptor cache key 必须包含 backing identity/version；失败时宁可阻止 draw，也不能复用旧 GPUVA。

### 6.3 CPU 对象相同/不同都不能代表 GPU 状态

不同 InputAssembler actor 可能包含完全相同的 VBV/IBV；同一个 DescriptorSet actor 在 update 后也可能代表不同 GPU 资源版本。优化和正确性判断都应使用原生状态及版本，而不是裸指针捷径。

### 6.4 Texture view 必须与 owner 共享 backing 和状态

view 不能只在创建时复制一次 resource/state。owner resize/destroy 后，view 要么跟随共享 backing 的新版本，要么明确失效；partial mip/layer/plane barrier 必须更新对应 subresource，而不是覆盖整个纹理状态。

### 6.5 Fence 是资源复用许可，不只是“等 GPU”的工具

command allocator、descriptor heap range、upload page、query、bundle backing、VB/IB、PSO、texture/buffer 都必须在关联 fence 完成后才能复用或释放。Signal/Event/Wait 失败不能继续走“假装完成”的回收路径。

### 6.6 Fail-closed 优于静默猜测

- 不完整的 Compute 路径就不声明支持；
- 不支持的 vertex format 不替换成“最接近”的格式；
- framebuffer attachment 不按尺寸猜；
- 非法 barrier state 不强行 OR；
- 不支持的 depth/stencil resolve 不录制命令；
- descriptor 构建失败就阻止 draw 并输出聚合错误。

这种做法短期可能暴露更多错误，但长期显著降低“画面偶尔不对却无法定位”的成本。

### 6.7 Debug Layer、RenderDoc 和静态测试各自解决不同问题

- D3D12 Debug Layer：发现状态、descriptor、生命周期和 API 规范错误；
- RenderDoc：确认某个 draw 的真实 Shader、CBV 数据、descriptor、RT 和 Mesh；
- PIX/运行时 counters：定位 CPU/GPU 时间与调度瓶颈；
- 静态契约测试：防止关键实现模式在重构中退化；
- C++ 单元测试：验证 resource state、cache scheduler、framebuffer/resolve、owner/view 生命周期等可隔离语义；
- test cases 跨后端截图/人工对比：作为最终场景级效果验收。

任何单一工具都无法替代其他层。

## 7. 当前架构要点

```text
GFXDeviceManager
    -> CCD3D12Device
        -> DXGI Adapter / ID3D12Device / Device-scoped caches
        -> Swapchain + frame resources + fences
        -> Queue submission contexts
        -> CPU/GPU descriptor heap pools

RenderQueue
    -> DrawPacket[] / controlled auto-instancing
    -> Validator / Agent backend-neutral forwarding
    -> CCD3D12CommandBuffer
        -> Pipeline/Root Signature binding
        -> Descriptor static/dynamic split
        -> Native IA comparison
        -> Draw / ExecuteIndirect / copy / resolve / barrier

Buffer / Texture owner and views
    -> shared native backing
    -> backing identity + descriptor version
    -> D3D12ResourceState (per subresource)
    -> recording context retention
    -> submission fence retirement
```

这里最重要的三个“身份”必须保持分离：

1. GFX actor identity：上层对象身份；
2. native backing identity：实际 `ID3D12Resource`/PSO/heap 身份；
3. submitted lifetime identity：某次录制/提交期间必须保活的版本。

大量早期 bug 都来自把这三个身份混为一谈。

## 8. 验证与回归体系

### 8.1 当前自动化资产

- `native/tests/unit-test/d3d12_perf_static_test.py`：76 个契约测试，覆盖同步、三帧资源、descriptor、Shader/PSO、批处理、自动实例化和平台帧节流等；
- `native/cocos/renderer/gfx-d3d12/D3D12HotPathStaticTest.py`：23 个热路径约束；
- `native/tests/unit-test/src/d3d12_render_pass_test.cpp`：50 个 C++ 测试，覆盖资源状态、subresource、Shader cache/scheduler、descriptor heap、render pass、framebuffer、resolve、owner/view 生命周期、skinning 等。

### 8.2 推荐回归顺序

1. 运行两个 Python 静态测试；
2. 构建并运行 D3D12 C++ unit tests；
3. 使用 D3D12 Debug Layer 跑 test cases，确保 API error、stderr、device removal 为零；
4. 对 GLES3、Vulkan、D3D12 做同场景效果对比，重点检查透明、粒子、阴影、RenderTexture、reflection probe、resize/restart；
5. 正常模式运行可见前台 60+30 性能采样；
6. 若回退，先按 outer frame、RenderQueue/Validator、descriptor、upload、IA/batch、Present/loop 分层归因，不直接猜热点；
7. 为新发现的问题补最小静态或 C++ 回归后再提交修复。

## 9. 工程方法复盘

### 9.1 做对的事情

- 先建立最小清屏和三角形闭环，再扩展完整材质；
- 用 GLES3/Vulkan 作为行为参照，但不机械照搬它们的内存模型；
- 将黑屏拆成 Shader、PSO、descriptor、IA、RT、resource state 等可独立验证环节；
- 对阴影等复杂问题使用 RenderDoc 逐 draw 排除假设；
- 把负结果和回退方案也记录下来，避免重复消耗；
- 性能优化先建立可信采样协议，再做 A/B；
- 在性能达标后仍进行规范审查，补生命周期和 fail-closed 语义；
- 让关键经验进入测试，而不是只留在聊天记录和个人记忆里。

### 9.2 如果重新做一次，可以更早执行的事情

- 第一版就明确 owner/view/backing/submission 四类生命周期；
- 第一版就采用 frame resources + fence retirement，而不是功能完成后再重构；
- 在 Shader/PSO 接入时同时设计 cache identity、设备 epoch 和异步任务销毁协议；
- 在 `updateBuffer` 设计阶段先写清楚 GLES3/Vulkan/D3D12 的可见性差异；
- 在 framebuffer API 接入时禁止任何 attachment 猜测；
- 从第一次性能测试开始就强制 visible foreground、预热和固定样本窗口；
- 从第一次优化开始就同时记录“为什么回退”，而不是只保存成功改动；
- 把跨后端截图/像素差异测试进一步自动化，减少最终人工比对成本。

## 10. 当前边界与后续建议

当前项目 test cases 已达到 GLES3/Vulkan 效果一致，性能也满足本轮目标。后续工作建议按“回归防护优先于扩展能力”排序：

1. 将跨后端场景截图或 readback 差异纳入自动化门禁；
2. 定期在不同显卡/驱动上执行 Debug Layer 与 60+30 性能回归；
3. 保持 Compute fail-closed，只有 graphics/compute PSO、root binding、SRV/UAV、barrier 和测试完整后再开启 capability；
4. 对 descriptor heap overflow、dynamic offset、Shader/PSO cache budget 加运行时聚合 counters；
5. 继续用 device-scoped、fence-safe、精确 identity 原则评审任何新缓存；
6. 对 resize、restart、资源销毁后立即提交、设备丢失等生命周期场景做压力测试；
7. 性能若出现回退，先确认前台/节流/构建条件，再分析后端热点。

## 11. 结语

gfx-d3d12 最终能达到与 GLES3/Vulkan 一致的 test case 效果，并保持不差的性能，靠的不是某一个“大优化”或某一次“神奇修复”，而是不断把模糊问题压缩成可验证事实：

- 黑屏被拆成 descriptor、PSO、IA、RT 和 barrier；
- 阴影被拆成 Shader 分支、UBO 内容和 GPU 可见版本；
- 低 FPS 被拆成 descriptor copy、draw recording、IA 原生状态和外层帧调度；
- 随机风险被拆成 backing identity、提交上下文和 fence 生命周期。

这段历程最值得保留的成果，是一套适用于显式图形 API 后端开发的原则：**资源身份必须精确，GPU 可见性必须有顺序，生命周期必须由 fence 证明，优化必须由数据证明，后端能力必须 fail-closed，最终正确性必须用真实场景跨后端证明。**

## 附录 A：关键提交时间线

| 日期 | 提交 | 里程碑 |
|---|---|---|
| 2026-04-25 | `0a010b745f` | Windows 新增 D3D12 后端，PoC 骨架与清屏 |
| 2026-04-29 | `46d29f8533` | D3D12 三角形跑通 |
| 2026-04-30 | `c6a3fd3534` | 材质 Shader、Root Signature、Descriptor、PSO 适配 |
| 2026-05-02 | `042af2e78d` | 3D 场景正常，进入透明等效果修复 |
| 2026-05-03 | `5efb67be19` | 修复 Buffer resize 后旧 GPUVA 导致的 2D UI 异常 |
| 2026-05-03 | `d84963b8b3` | 修复粒子显示异常 |
| 2026-05-17 | `8a08b44a59` 等 | RenderTexture/framebuffer/离屏渲染修复 |
| 2026-05-24 | `525805bb9f`、`8e7db589b4` | CSM 阴影与 UBO 覆盖问题收敛 |
| 2026-06-06 | `564b55d97e` | RenderTexture 剩余问题修复 |
| 2026-06-07 | `7384d272fc` | 粒子 Shader/PSO 进一步修复 |
| 2026-06-28 | `cf4c88cf14` | 首次加载与 Shader/PSO 性能优化 |
| 2026-07-13 | `56a93871ea` | Shader 编译/缓存调度和回归测试增强 |
| 2026-07-18 | `2251c9a91f` | descriptor、upload、command path 大规模优化 |
| 2026-07-19 | `23c6e35c49` | 批量绘制、资源绑定、Win32 帧节流收敛 |
| 2026-07-24 | `62265a4fec` | 受控自动实例化 |
| 2026-08-01 | `4cb364f21e` | 正确性、per-subresource 状态和资源生命周期加固 |

## 附录 B：证据与延伸资料

- `AI/D3D12-GFX-PoC-Detailed-Plan.md`
- `AI/D3D12-Plan-Execution-Status.md`
- `AI/D3D12-Material-Support-Plan.md`
- `AI/D3D12-CodeReview-Report.md`
- `AI/d3d12-backend-audit-report.md`
- `AI/d3d12-alpha-blend-analysis.md`
- `AI/d3d12-rendering-debug-guide.md`
- `AI/analysics/d3d12-csm-ubo-overwrite-fix.md`
- `AI/analysics/d3d12-shadow-final-root-cause.md`
- `AI/d3d12 optimize/D3D12-Performance-Optimization-Retrospective-2026-07-19.md`
- `AI/d3d12 optimize/D3D12-Backend-Code-Review-2026-07-29.md`
- `native/cocos/renderer/gfx-d3d12/D3D12_PERF_ROUNDS.md`
- `native/tests/unit-test/d3d12_perf_static_test.py`
- `native/cocos/renderer/gfx-d3d12/D3D12HotPathStaticTest.py`
- `native/tests/unit-test/src/d3d12_render_pass_test.cpp`

