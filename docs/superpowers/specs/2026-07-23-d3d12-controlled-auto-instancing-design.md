# D3D12 受控自动实例化设计

## 背景与目标

当前连续 `DrawPacket[]` 已把目标场景从约 45 FPS 提升到 50.20 FPS，但稳态分段计时仍显示每帧约 2.8～2.9 ms 消耗在 2500 个逻辑对象的 RenderQueue 组包、Validator 解包和 D3D12 packet 消费。GPU fence 等待接近 0，D3D12 后端也已经用 `ExecuteIndirect` 合并原生提交，因此继续微调描述符或哈希无法消除 CPU 的 O(N) 对象遍历。

本设计在不要求业务显式设置 `USE_INSTANCING` 的情况下，将严格兼容的连续 opaque draw run 转换为硬件实例化绘制。目标场景的 2500 个相同方块应被压缩为至多 `ceil(2500 / 1024) = 3` 个实例化 draw；任何无法证明兼容的对象继续使用现有 DrawPacket 路径。

性能验收沿用既定规则：可见前台运行，丢弃前 60 条 FPS 日志，取后续 30 条平均值，目标 `>=59 FPS`。

## 方案比较

### 方案 A：修改业务 Pass，自动打开 `USE_INSTANCING`

直接改变 `Pass::_defines` 和 batching scheme，可以复用现有 ForwardStage 显式实例化分支，但会永久改变业务材质状态、shader 缓存键和其他后端行为；PassInstance 当前还会主动把 `USE_INSTANCING` 关闭。该方案侵入性高且难以回退，不采用。

### 方案 B：D3D12 Bundle 或跨帧 ExecuteIndirect 重放

它不要求实例化 shader，但此前精确诊断已经证明 transient CBV 的 GPU VA 会变化，descriptor 内容无法保证跨帧完全一致。Bundle 还需要额外的提交 fence 传播和资源生命周期管理。它不能消除 RenderQueue 每帧逐对象组包，且正确性边界更大，不采用。

### 方案 C：连续 run 的临时实例化 variant（采用）

保持业务 Pass、原 shader 和 batching scheme 不变。RenderQueue 只在 D3D12、opaque、无 occlusion query 的路径中，为严格白名单对象获取带 `USE_INSTANCING=true` 的临时 shader variant，构造实例顶点流，并在原 run 位置提交实例化 draw。无法通过任一条件的对象保留原 DrawPacket 顺序。

该方案复用现有 `InstancedBuffer`、InputAssembler 和 `DrawInfo::instanceCount` 资产，同时把影响范围限制在 D3D12 自动路径。

## 架构

### 1. 无副作用 shader variant

在 `scene::Pass` 增加内部通用接口：基于当前 defines、SubModel patches 和调用方 overrides 的副本取得 shader variant。该接口不得修改 `_defines`、`_shader`、`_batchingScheme` 或 pass hash。

RenderQueue 使用它请求 `USE_INSTANCING=true` variant。若编译失败或 variant 不满足实例属性约束，本 run 回退。

### 2. 严格 eligibility

自动实例化只在以下条件全部满足时启用：

- 当前设备 API 是 D3D12，且支持 `Feature::INSTANCED_ARRAYS`；
- RenderQueue 为 opaque，未启用 occlusion query；
- pass 的原 batching scheme 是 `NONE`，program 是内置 `standard`；
- model 类型是 `DEFAULT`，排除 skinning、baked skinning、morph、粒子及 JS 自定义 model；
- defines/patches 未启用 lightmap、light probe、reflection probe、skinning、morph 或其他需要逐对象 local 数据的功能；
- 临时 shader 的 instanced attributes 恰好是 `a_matWorld0/1/2` 三个 `RGBA32F` 输入，不接受额外实例属性；
- 连续对象具有相同 Pass、原 shader、材质 DescriptorSet、primitive、attributes hash、DrawInfo、VB/IB 资源序列；
- 除 `UBOLocal` 世界矩阵来源外，local DescriptorSet 中的 buffer、texture、sampler 资源完全一致；
- shadow bias、normal bias、receive-shadow 和 directional-light 等影响 local 数据的 Model 状态完全一致；
- 连续兼容对象数量至少为 4。1～3 个对象继续走 DrawPacket，避免实例缓冲开销反而增大。

首版只支持上述可证明正确的内置 standard 子集，不尝试猜测自定义 shader 的 local UBO 语义。

### 3. 实例数据与缓冲生命周期

扩展 `InstancedBuffer`，增加只接收 world matrix 的受控 merge 入口。它直接生成 48 字节的三行 `vec4` 实例数据，不修改 `SubModel::InstancedAttributeBlock`，因此不会让 Model 停止更新其他非实例化 pass 的 local UBO。

沿用现有 `INITIAL_CAPACITY=32`、`MAX_CAPACITY=1024`、DEVICE vertex buffer 和 `updateBuffer()` 上传机制。实例缓冲在 render pass 之前上传，仍由现有 command buffer/fence 资源路径管理。

### 4. RenderQueue 数据流与顺序

`RenderQueue::prepareAutoInstancing()` 在 sort 完成后、进入 render pass 前执行：

1. 清理上一帧 run 计数但保留缓冲容量；
2. 扫描排序后的 `_queue`，只合并连续且兼容的 run；
3. 对长度达到阈值的 run 建立 `AutoInstancedRun {first, count, buffer}`；
4. 将 run 的实例数据上传；
5. `recordCommandBuffer()` 遍历原队列时，在 `first` 位置绘制实例 run，并跳过其余成员；
6. 非实例化项继续写入连续 DrawPacket 数组。

实例 draw 保留在原排序位置，不把所有实例对象提前或延后，从而保持 opaque 队列的状态/深度顺序。一个超过 1024 个实例的 run 由 `InstancedBuffer` 自动拆成多个 draw。

### 5. 回退与失效

以下情况均为普通回退，不打印逐对象日志：shader variant 获取失败、实例属性不匹配、资源/状态不一致、run 小于阈值、buffer 创建失败、occlusion query 开启或非 D3D12 后端。

Pass、shader、geometry、descriptor 或 model 状态每帧重新参与严格比较，因此变化会在下一帧自动拆分 run。缓存只复用 GPU buffer 容量，不缓存 eligibility 结论或资源相等结果。

## 文件边界

- `native/cocos/scene/Pass.h/.cpp`：无副作用 shader variant overrides。
- `native/cocos/renderer/pipeline/InstancedBuffer.h/.cpp`：world-matrix-only merge 和布局验证。
- `native/cocos/renderer/pipeline/RenderQueue.h/.cpp`：连续 run 检测、缓存、上传和原位提交。
- `native/cocos/renderer/pipeline/forward/ForwardStage.cpp`：在 render pass 前调用 prepare/upload。
- `native/tests/unit-test/d3d12_perf_static_test.py`：结构契约、严格回退和顺序测试。

不修改 Vulkan、GLES、Metal 后端实现；通用新增接口在这些后端不会被调用。

## 测试与验收

### 静态/TDD

- shader variant overrides 不修改 Pass 原 defines；
- 自动路径必须受 D3D12、opaque、batching NONE 和 standard 白名单保护；
- layout 只接受三个 world-matrix instanced attributes；
- run 阈值和连续性明确；
- instanced run 在原位置提交，其余对象仍进入 DrawPacket；
- ForwardStage 在 render pass 前触发实例 buffer 上传；
- Vulkan/GLES 路径不会进入自动实例化。

每项先加入失败测试并确认 RED，再写最小实现变 GREEN。

### 构建与运行

1. 构建 `D:\Work\CocosProjects\cocos-test-projects\build\windows\proj\test-cases.sln`，Debug|x64。
2. 开启 D3D12 Debug Layer 前台运行，确认 2500 Boxes/Batched 画面正确；扫描 D3D12/device/HRESULT/resource-state/descriptor 错误为 0。
3. 关闭 Debug Layer重新前台运行，丢弃 60 条、统计后续 30 条平均 FPS。
4. 只有平均值 `>=59` 才达到性能目标；否则保留正确性改造并继续依据分段数据优化，不扩大 eligibility 猜测范围。

## 非目标

- 不自动实例化透明对象；
- 不支持任意自定义 shader；
- 不改变业务材质 defines 或 batching scheme；
- 不做 GPU culling、mesh shader、Bundle replay 或跨帧 draw-list 重放；
- 不移除现有 DrawPacket/ExecuteIndirect fallback。
