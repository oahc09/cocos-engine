# D3D12 性能优化复盘与交付记录

日期：2026-07-19  
范围：`native/cocos/renderer/gfx-d3d12/`、渲染前端、Win32 帧循环与性能采样工具。  
场景：`test-cases.sln` `Debug|x64` 中的高 draw-call 测试场景；GLES3 在同机明显高于早期 D3D12 表现。

## 1. 交付结论

本轮工作将问题从“D3D12 后端只有数 FPS”的功能/资源问题，收敛为一个可重复的 CPU 前端、描述符绑定和 Win32 帧节流问题，并完成了以下保留修复：

- 消除热路径重复 UBO 上传、重复描述符准备和不安全的 descriptor 生命周期用法。
- 对 local descriptor set 做精确静态资源复用，将每帧 CBV/SRV/UAV descriptor copy 从 **37,602** 降至 **5,115**（R140，减少 **86.4%**）。
- 将 2,500 个逻辑 draw 的 local root-CBV 路径合并为一次 `ExecuteIndirect` 批次；不再以 C++ `InputAssembler` 对象身份误判原生 IA 状态变化。
- 跳过等价 IA 的重复绑定，并将 local set 选择与 root-CBV 批次编码合并，保持 validator 和 D3D12 immediate fallback 语义。
- 删除 Win32 运行时的重复帧率限制：`WindowsPlatform::loop()` 是 Win32 运行时唯一的 60 Hz 节流器，`Engine::tick()` 不再再次 sleep。
- 将最终采样口径固定为：**慢加载标记后先忽略 60 条 FPS 日志，再取连续 30 条算术平均；验收阈值为 >=59 FPS，同时记录相对 60 FPS 的差值。**

用户已确认场景目标达成。仓库内最后一份归因短样本为 R172：`59.650 FPS`（5 条正式样本），它用于定位而非替代新口径的 60+30 最终归档。今后回归必须用新的 60+30 规则重新产生正式测量文件。

## 2. 最终验收与复现协议

历史上“隐藏窗口”会触发 SDL 的 background/pause 路径，得到约 30 FPS 的假回归；因此仅允许可见前台采样作为性能结论。

```powershell
$runner = 'D:\cocos_custome\cocos-engine\d3d12_perf_records\run_round.ps1'
& $runner -Round <N> -Label <name> -Visible -PerfLog `
    -ReadinessCount 60 -FormalCount 30 -TargetFps 59
```

采样器默认值已更新为上述 `60 / 30 / 59`。报告会同时写入：

- `formalMean`：第 61 至第 90 条稳态日志的平均 FPS；这是验收值。
- `targetFps` / `targetMet`：与 59 FPS 门槛比较。
- `referenceFps=60.000` / `referenceDelta`：与设计目标 60 FPS 的差值。
- 前台焦点验证、错误日志数、stderr 和残留进程状态。

安全验证与性能验证必须分开：先以 `-DebugLayer` 跑 60+30，确认 D3D12 错误和 stderr 均为零；再关闭 Debug Layer 做正式性能样本。Debug Layer FPS 不用于性能结论。

标准构建：

```powershell
& 'D:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' `
  'D:\Work\CocosProjects\cocos-test-projects\build\windows\proj\test-cases.sln' `
  /m:1 /t:Build /p:Configuration=Debug /p:Platform=x64 /v:minimal
```

## 3. 关键数据节点

不同轮次可能包含不同的缓存温度、前台状态或诊断宏；下表只用于展示收敛过程，A/B 结论只在同一构建和同一前台协议内成立。

| 节点 | 有效样本结论 | 意义 |
|---|---:|---|
| R58 | 28.354 / 28.181 FPS | 60 FPS 目标的早期受控基线。 |
| R60 | 52.003 FPS | 正常重建后的基线，证明此前 28 FPS 存在环境/构建因素。 |
| R70 | 53.549 FPS | 移除重复 UBO 上传；保留正确性改动，不夸大收益。 |
| R72 | 55.861 FPS | Validator 成本对比；仅作归因，未将关闭 Validator 作为交付方案。 |
| R80 | 52.104 FPS | 建立可见前台标准，废弃隐藏窗口的约 30 FPS 结果。 |
| R87 | 55.472 FPS | direct dynamic-CBV 受控尝试。 |
| R100 | 45.799 FPS，回退 | Root Signature 1.1 static-slot 在目标驱动更慢，完整回退。 |
| R104 | 54.730 FPS | 三帧资源路径验证。 |
| R115 | 46.409 FPS | 当前前端控制样本；用于下一阶段描述符归因，不与异构环境横比。 |
| R140 | 56.565 FPS | 精确 static UBO identity 修复，较 R131 基线 +3.235 FPS（+6.07%）。 |
| R161 | 56.006 FPS（诊断） | 2,500 draw 成功合并为一次 native-IA-compatible indirect batch。 |
| R162 | 55.914 FPS | Debug Layer 通过后的正常前台 30+30。 |
| R165 | 56.093 FPS | 等价 IA bind skip 后的正式样本。 |
| R166 | 56.167 FPS | 融合 local set/batch 后的正式样本；证明剩余瓶颈不在该虚调用。 |
| R169 | 57.770 FPS（Debug） | 帧 pacing 初步修复，显示节流是上层瓶颈。 |
| R171 | 58.767 FPS | Win32 单一帧节流后的正式旧口径样本，较 R166 +4.63%。 |
| R172 | 59.650 FPS（5 条诊断） | Win32 loop 归因样本；证明实际已接近 60 Hz。 |

原始样本、stdout/stderr、构建日志及逐轮说明位于：`D:\cocos_custome\cocos-engine\d3d12_perf_records\round-*`。

## 4. 优化历程

### 阶段 A：风险修复、冷启动与基线建立（R11-R69）

先处理会导致错误渲染、崩溃或污染测量的后端问题：描述符 staging heap 分配失败、资源状态/帧资源重用、默认堆更新、clear value、shader/PSO 缓存与 DXBC 哈希检查。shader 加载流程被拆分为 shader bytecode、PSO key、磁盘/内存缓存命中和 `CreateGraphicsPipelineState` 四段，以避免把首次 shader/PSO 冷启动误当成稳态 FPS。

随后用 buffer upload、descriptor copy、draw/validator、frame graph、forward pipeline、scene culling 等低侵入计数器建立归因图。R61-R69 验证了上层前端和 CPU 调度比 `Present` 更值得优先处理；亲和性和编译优化只作为控制变量，没有被误认为根因。

### 阶段 B：描述符生命周期与前台测量可信度（R70-R99）

R70 发现 `DescriptorSet::update()` 提前上传 transient UBO，而 draw 时的 pending-update flush 又执行同一上传。删除早期重复上传，保留 `forceUpdate()` 的初始化路径，既避免双分配也不改变可见性顺序。

R72-R74 将 validator、延迟 descriptor update 和 forward pipeline O2 分别隔离。它们只能解释局部成本，不能解释剩余的 55 FPS 平台。R80 则发现隐藏启动导致 `HIDDEN -> onPause -> EnterBackground`，其 30 FPS 数据全部标记为无效；由此修复 `run_round.ps1` 的前台激活、焦点轮询和失焦拒绝逻辑。

这一阶段还完成 local root-table 分割：只在 local set 中存在“动态 uniform 前缀 + 静态资源后缀”时拆分；静态复用必须同时匹配 layout、heap epoch 和每个资源指针，哈希碰撞不能复用错误资源。

### 阶段 C：错误候选的证伪与帧资源收敛（R100-R132）

R100 尝试 Root Signature 1.1 static descriptor range。Debug Layer 首先暴露 sampler flags 非法，修正后功能正确，但正式性能降至 45.799 FPS，因此完整回退。这是重要的反例：D3D12 feature 更“新”不等于目标驱动更快。

后续三帧资源环、稳定 CBV heap、frame-resource 复用、root `b0` CBV、动态 descriptor staging fast path、静态 suffix cache、Debug 编译保留诊断但使用优化 codegen 等改动均经过 Debug Layer 和控制样本。它们消除了资源重用风险并减少了每 draw 工作，但尚未穿透 60 FPS。

### 阶段 D：精确资源身份与 local descriptor 深挖（R133-R147）

诊断显示 static table 复用虽“命中”，却遗漏普通 static UBO 的对象 identity。R140 将 UBO identity 和 descriptor version 纳入精确 key；结果是每帧 descriptor copy 从 37,602 降至 5,115，static table 命中 2,499 / 2,505，正式 FPS 达 56.565。

R141-R147 继续检查 root CBV、dirty set、动态资源基数、空 dynamic table cache 和 staging fast path。控制实验表明这部分虽有价值，却不是最后 3-4 FPS 的决定性来源；由此停止微调哈希和 descriptor heap，转向 command recording 结构。

### 阶段 E：2,500 draw 的 IA/indirect 批处理根因（R148-R166）

R150 证明场景存在一个约 2,500 draw 的精确几何组。初版 local root-CBV indirect 路径在 R151 回退，随后 R152-R160 逐层记录分批边界、view、layout 和 IA 来源。

决定性证据在 R160：`InputAssembler` 的 C++ actor 有 2,500 个不同对象，但 D3D12 VBV/IBV 实际只在首个 draw 改变一次。R161 因此改为比较 native vertex/index views，而不是 IA 指针；结果为 `attempts=2500`、`encoded=2500`、`executeBatches=1`、无 fallback/capture failure。

R164 再对同一 local batch 跳过等价 IA bind，validator/backend IA bind 从约 2,505 降至 6。R166 把 local descriptor set 选择与 batch draw 编码融合，保留 CommandBufferValidator 状态、pipeline layout 检查与 D3D12 immediate fallback。Debug Layer 均无错误，但 R166 只到 56.167 FPS，说明 D3D12 command path 已非全帧决定性瓶颈。

### 阶段 F：帧预算与 Win32 双节流根因（R167-R172）

R167 外层 forward pipeline 归因只有约 7-8 ms；R168 `Engine::tick()` 显示 `events::Tick` 常在 13-16 ms，D3D12 `frameSync()` 为 0，而 Engine 内 sleep 再增加约 2-4 ms。此前 `Engine::tick()` 与 `WindowsPlatform::loop()` 都以同一个 `getFps()` 做 60 Hz 节流。

R169 的“sleep guard + yield”和 R170 的“末段忙等”均没有稳定消除问题，因而没有保留。最终修复是从 Win32 非 Editor 的 `Engine::tick()` sleep 条件中移除 Windows，只让 `WindowsPlatform::loop()` 调度。R171 正式旧口径升至 58.767 FPS。

R172 的 Win32 loop 聚合日志给出剩余波动的量化边界：`taskMs` 约 13.4-15.4 ms、`eventMs` 约 1.0-1.4 ms、`dispatchMs` 约 16.67-17.01 ms，`lateMs` 为 0-0.34 ms。它证明主要剩余是事件处理和 OS 调度尾部，而不是 `Present`、GPU fence 或 descriptor heap。该诊断宏默认关闭。

## 5. 最终保留的设计约束

| 领域 | 必须保持的约束 |
|---|---|
| 描述符缓存 | 仅在同一 layout、heap epoch 和逐资源精确 identity 相同时复用。hash 仅是索引，不能单独决定正确性。 |
| root-CBV batch | IA 是否相同必须以 native VBV/IBV views 判断；C++ actor identity 不是 GPU state。 |
| local set 融合 | 必须更新 validator 的 descriptor state/offset/layout dirty 状态，并保留 direct fallback。 |
| 帧资源 | descriptor heaps、upload buffers、command allocators 在关联 fence 完成前不得重用。 |
| Win32 限帧 | release/runtime 仅 `WindowsPlatform::loop()` 限速；不要重新向 `Engine::tick()` 加 Windows sleep。 |
| 性能日志 | `CC_D3D12_*_PERF_COUNTERS` 与 `CC_D3D12_PLATFORM_LOOP_PERF_COUNTERS` 默认均为 `0`。 |
| 验收 | 只认可 visible-foreground、无 Debug Layer 的 60 条预热后 30 条正式均值；Debug Layer 只做正确性门禁。 |

## 6. 失败尝试与可复用经验

- 隐藏窗口、失焦窗口、初始加载期的样本不可与前台稳态样本比较。
- Root Signature 1.1 static slots 在目标适配器显著退化，必须以目标驱动的 A/B 数据决策。
- “更多 root descriptor”或“更多缓存命中”不能替代资源 identity 和 command ordering 的正确性验证。
- 在已有平台节流时增加第二个 sleep/spin 会掩盖根因，甚至提升 CPU 占用；应先追踪 tick/loop 的全帧时序。
- 每个候选都应先写静态 RED 检查，再实现、构建、Debug Layer 验证，最后做前台正式样本；负结果和回退同样需要记录。

## 7. 关键文件与证据索引

- D3D12 descriptor / command 路径：`native/cocos/renderer/gfx-d3d12/D3D12DescriptorSet.cpp`、`D3D12CommandBuffer.cpp`、`D3D12Device.cpp`、`D3D12Buffer.cpp`。
- 前端与 validator：`native/cocos/renderer/pipeline/RenderQueue.cpp`、`native/cocos/renderer/gfx-validator/CommandBufferValidator.cpp`、`native/cocos/renderer/gfx-base/GFXCommandBuffer.h`。
- shader/PSO 冷启动与缓存审查：`native/cocos/renderer/gfx-d3d12/D3D12Shader.cpp`、`D3D12PipelineState.cpp`、相关 cache scheduler。
- 单一 Win32 帧节流：`native/cocos/engine/Engine.cpp`、`native/cocos/platform/win32/WindowsPlatform.cpp`。
- 采样工具：`d3d12_perf_records/run_round.ps1`。
- 回归契约：`native/tests/unit-test/d3d12_perf_static_test.py`。
- 代表性逐轮文件：`round-70/ROUND.md`、`round-80/ROUND.md`、`round-100/ROUND.md`、`round-115/ROUND.md`、`round-140/ROUND.md`、`round-161/ROUND.md` 及 R162-R172 的 measurement/stdout/build logs。

## 8. 后续回归建议

1. 每次改动先运行静态契约和 `test-cases.sln Debug|x64` 构建。
2. 先进行 Debug Layer 60+30；D3D12 错误、stderr 或前台验证失败时，样本直接无效。
3. 再进行正常前台 60+30，以 `formalMean >= 59` 作为门槛，以 `referenceDelta` 观察与 60 FPS 的距离。
4. 若长期回落，优先读取 Win32 loop 的临时归因（默认关闭）和 D3D12 outer-frame 指标，不要直接重启 descriptor hash/heap 微调。
5. 新的正式验收 log 应作为单独的 `round-XXX` 归档保留，避免用短诊断或 Visual Studio 肉眼帧率替代证据。
