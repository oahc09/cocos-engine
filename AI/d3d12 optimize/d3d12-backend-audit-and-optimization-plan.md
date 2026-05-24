# D3D12 渲染后端问题审查与优化方案

日期：2026-05-24

范围：`native/cocos/renderer/gfx-d3d12/`

## 结论

当前 D3D12 后端可以通过 `build/d3d12-poc` 的 Release 编译，但实现仍偏 PoC 状态，不能认为已经完整正确。主要风险集中在 descriptor heap 生命周期、compute 管线、资源读回、storage buffer/UAV、资源状态转换、格式支持声明、blit 语义和诊断 fallback 逻辑。

建议先修复会导致错误渲染或功能静默失效的 P0/P1 问题，再补齐兼容性与性能优化。

## 验证基线

已执行：

```powershell
cmake --build build/d3d12-poc --config Release
```

结果：通过。

说明：编译通过只能证明当前代码可构建，不能证明渲染语义正确。以下问题来自静态代码审查。

## 问题与方案

### P0：多 CommandBuffer 提交时 descriptor heap 可能被覆盖

相关文件：

- `native/cocos/renderer/gfx-d3d12/D3D12CommandBuffer.cpp`
- `native/cocos/renderer/gfx-d3d12/D3D12Queue.cpp`
- `native/cocos/renderer/gfx-d3d12/D3D12DescriptorHeapPool.cpp`

现象：

`CCD3D12CommandBuffer::begin()` 会重置全局 GPU-visible descriptor heap pool。`CCD3D12Queue::submit()` 又支持一次提交多个 command list。若录制顺序为 A begin/record/end，B begin/record/end，然后一起 submit，B 的 begin 会重置并复用 A 已经录进 command list 的 descriptor heap 区间，A 执行时可能读到 B 的 descriptors 或无效 descriptors。

影响：

- 多 command buffer 或 secondary/batched 提交时渲染错乱。
- 资源绑定看似正确，但 GPU 执行阶段读取错误 descriptor。
- 可能出现随机黑屏、材质错乱、采样错误。

解决方案：

1. 不在单个 `CommandBuffer::begin()` 中重置全局 GPU-visible heap。
2. 引入 frame/ring allocator：按帧重置 descriptor heap，确保本帧所有 command list 引用的 descriptor 在 GPU 完成前保持有效。
3. 将 descriptor allocation 生命周期绑定到 fence value，GPU 完成后再回收。
4. 如果短期只支持单 primary command buffer，应在 `Queue::submit()` 或后端能力中明确限制，并 assert `count == 1`，避免静默错。

验证：

- 构造两个 command buffer，分别绑定不同纹理/常量，合并 submit，确认两个 draw 结果都正确。
- 开启 D3D12 debug layer，检查是否出现 descriptor heap 或 root descriptor table 警告。

### P0：Compute 管线未完整实现

相关文件：

- `D3D12Shader.cpp`
- `D3D12PipelineState.cpp`
- `D3D12PipelineLayout.cpp`
- `D3D12CommandBuffer.cpp`

现象：

Shader 支持 compute stage 编译，但 pipeline state 只创建 graphics PSO。`dispatch()` 调用前也只 flush graphics descriptor table，未设置 compute root signature / compute descriptor table / compute pipeline state。

影响：

- compute shader 路径实际不可用。
- 使用 compute 的粒子、后处理、GPU skinning、culling 或自定义 pass 会失败。

解决方案：

1. 在 `CCD3D12PipelineState` 中区分 graphics / compute bind point。
2. compute shader 存在时创建 `D3D12_COMPUTE_PIPELINE_STATE_DESC` 并调用 `CreateComputePipelineState`。
3. `bindPipelineState()` 根据 PSO 类型调用 `SetPipelineState` 后设置 `SetComputeRootSignature` 或 `SetGraphicsRootSignature`。
4. `flushDescriptorSets()` 增加 compute 分支，compute dispatch 前调用 `SetComputeRootDescriptorTable`。
5. `dispatch()` 前校验当前绑定的是 compute PSO，否则记录错误并跳过。

验证：

- 添加最小 compute shader 写 UAV texture/buffer 的测试。
- 通过 readback 或渲染采样验证 UAV 写入结果。

### P0：`copyTextureToBuffers()` 是空实现

相关文件：

- `D3D12Device.cpp`

现象：

`CCD3D12Device::copyTextureToBuffers()` 当前只 `(void)` 参数，未执行任何 readback。

影响：

- 截图、像素读回、测试验证、纹理导出相关功能静默失败。
- 自动化渲染测试无法可靠比较结果。

解决方案：

1. 创建 `D3D12_HEAP_TYPE_READBACK` buffer。
2. 使用 `GetCopyableFootprints()` 计算 row pitch 和总大小。
3. 将源 texture 从当前状态 transition 到 `COPY_SOURCE`。
4. 调用 `CopyTextureRegion()` 拷贝到 readback buffer。
5. submit 并等待 fence。
6. map readback buffer，按 `BufferTextureCopy` 的 `buffStride/buffTexHeight/buffOffset` 拷回用户 buffer。
7. 恢复 texture 原状态或根据 tracked state 更新。

验证：

- 上传一张已知颜色/棋盘纹理后读回，逐像素比较。
- 对 render target 渲染纯色后 readback，确认颜色一致。

### P1：Storage buffer / UAV 语义不完整

相关文件：

- `D3D12Buffer.cpp`
- `D3D12DescriptorSet.cpp`
- `D3D12PipelineLayout.cpp`
- `D3D12CommandBuffer.cpp`

现象：

所有 buffer 都创建在 `D3D12_HEAP_TYPE_UPLOAD`，且 `resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE`。`DescriptorType::STORAGE_BUFFER` 被 root signature 映射成 SRV，descriptor 写入也创建 raw SRV，不支持 shader 写 storage buffer。

影响：

- storage buffer 只读路径可能勉强可用。
- shader write storage buffer / RWStructuredBuffer / ByteAddressBuffer UAV 语义不可用。
- compute 或高级渲染路径会失败。

解决方案：

1. 根据 `BufferUsage` 区分 upload/default/readback heap。
2. 对 storage buffer 创建 default heap resource，并设置 `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`。
3. storage buffer 若 shader 只读可用 SRV；若包含写访问应映射为 UAV。
4. 在 descriptor layout 中为 writable storage buffer 使用 `D3D12_DESCRIPTOR_RANGE_TYPE_UAV`。
5. 更新 barrier：storage write 后插入 UAV barrier 或 transition 到后续读取状态。

验证：

- compute shader 写 buffer，再 readback 校验。
- graphics shader 读取 storage buffer，确认和 Vulkan/Metal 结果一致。

### P1：`CommandBuffer::updateBuffer()` 忽略 buffer view offset 且不裁剪大小

相关文件：

- `D3D12CommandBuffer.cpp`
- `D3D12Buffer.cpp`

现象：

`CCD3D12Buffer::update()` 会使用 `resourceOffset` 并 clamp copy size；但 `CCD3D12CommandBuffer::updateBuffer()` 直接写入 `mappedData` 起点，且按传入 size memcpy。

影响：

- 更新 buffer view 时写错位置。
- size 大于 buffer 逻辑大小时可能越界覆盖同一 resource 内其他数据。

解决方案：

1. 在 `CommandBuffer::updateBuffer()` 中复用 `CCD3D12Buffer::update()`。
2. 如果保留 command buffer 版本，必须使用 `getD3D12ResourceOffset()`，并用 `min(size, buff->getSize())` 裁剪。
3. 对 default heap buffer，后续需要改成 upload staging + copy，而不是直接 map。

验证：

- 创建 buffer view，更新 view 后确认 parent buffer 对应 offset 被修改。
- 用 debug layer 检查 map/unmap range。

### P1：格式能力表和实际 DXGI 支持不一致

相关文件：

- `D3D12Device.cpp`
- `D3D12Texture.cpp`
- `D3D12RenderPass.cpp`
- `D3D12PipelineState.cpp`
- `D3D12InputAssembler.cpp`

现象：

`initFormatFeatures()` 将 `RGB8/RGB16F/RGB8I/RGB8UI` 等标成 `F_FULL`，但 D3D12 texture resource 并没有原生 24-bit RGB 或 RGB16F render target 映射。部分 render pass / vertex format 使用 RGBA padded fallback，而 texture 创建没有完整对应。ETC/ASTC 被 fallback 到 RGBA8，但没有实际解压/转码。

影响：

- 上层认为格式可用，实际创建 texture/RTV/SRV 时失败或解释错误。
- ETC/ASTC 数据按 RGBA8 上传会得到错误图像。

解决方案：

1. 重新整理 D3D12 format capability table，只声明真实支持的 DXGI format。
2. 对 D3D12 不支持的 RGB texture 格式：
   - 资源格式改为 RGBA 等兼容格式时，必须同步调整上传 stride 和 shader 采样语义；
   - 或直接标记不支持，让资产管线选择兼容格式。
3. ETC/ASTC 不要简单返回 RGBA8。应：
   - 在加载阶段转码为 RGBA/BC；
   - 或在 D3D12 format feature 中标记不支持。
4. render pass、texture、SRV、RTV、vertex format 映射统一到同一张表，避免各文件 fallback 不一致。

验证：

- 遍历 `_formatFeatures` 中声明支持的 texture/render target/vertex format，逐项创建资源和 view。
- 加载 ETC/ASTC 测试贴图，确认 D3D12 路径不是错误解释压缩数据。

### P1：Barrier 语义过粗，忽略 subresource range

相关文件：

- `D3D12CommandBuffer.cpp`
- `gfx-base/GFXDef-common.h`

现象：

`pipelineBarrier()` 中 texture barrier 全部使用 `D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES`，没有使用 `TextureBarrierInfo::range`。同时 depth read 和 depth write 都映射到 `D3D12_RESOURCE_STATE_DEPTH_WRITE`。

影响：

- mip/layer 局部 transition 不准确。
- depth read-only pass 可能被当成 writable depth 使用。
- 复杂 render graph 中同一 texture 不同 subresource 并行用途会出错。

解决方案：

1. 实现 `ResourceRange` 到 D3D12 subresource index 的转换。
2. 对每个 mip/layer 生成精确 barrier，或维护 per-subresource state tracker。
3. 区分：
   - `DEPTH_STENCIL_ATTACHMENT_WRITE` -> `DEPTH_WRITE`
   - `DEPTH_STENCIL_ATTACHMENT_READ` -> `DEPTH_READ`
   - depth texture sampled -> `DEPTH_READ | PIXEL_SHADER_RESOURCE` 或按实际 shader stage 加 SRV state。
4. 对 read/write 混合状态做合法性检查，避免 D3D12 不允许的状态组合。

验证：

- shadow map 写入后采样。
- depth prepass read-only depth。
- texture array/mipmap 局部更新后采样不同 mip/layer。

### P1：`blitTexture()` 不支持 scaling/filter

相关文件：

- `D3D12CommandBuffer.cpp`

现象：

当 `TextureBlit` 的 src/dst extent 不一致时，仅输出 warning，然后仍使用 `CopyTextureRegion()` 按源尺寸复制，`Filter` 参数被忽略。

影响：

- mipmap 生成、缩放 blit、后处理 resize 等结果错误。
- 上层调用以为完成了 filtered blit，实际只是未缩放 copy。

解决方案：

1. same-size blit 继续用 `CopyTextureRegion()`。
2. scaling/filter blit 使用 fullscreen triangle/quad pass：
   - src SRV
   - dst RTV/UAV
   - point/linear sampler 根据 `Filter` 选择
3. 对 depth/stencil 或 compressed format 单独限制并报告。

验证：

- 4x4 -> 2x2 linear blit，比较预期采样结果。
- 2x2 -> 4x4 point/linear blit，检查边界和过滤。

### P1：PSO diagnostic fallback 会掩盖真实错误

相关文件：

- `D3D12PipelineState.cpp`
- `D3D12CommandBuffer.cpp`

现象：

graphics PSO 创建失败后，会清空 input layout 重试，甚至使用 empty root signature 重试。成功后标记 `diagnosticFallback`，draw 时直接 `DrawInstanced(3, 1, 0, 0)`。

影响：

- 真实材质/mesh 不会被正常渲染。
- 问题从“PSO 创建失败”变成“渲染结果奇怪”，更难定位。
- release/生产路径中不应存在这种替代渲染。

解决方案：

1. 将 fallback 限制到明确的诊断开关，例如 `CC_D3D12_DIAGNOSTIC_FALLBACK`。
2. 默认行为：PSO 创建失败应记录完整 shader/input/root signature 信息并使 PSO 无效。
3. draw 时若 PSO 无效，应跳过并记录一次性错误，而不是画固定三角形。
4. 增加 input layout 反射校验，提前输出属性缺失/格式不匹配。

验证：

- 人为制造 input layout 不匹配，确认默认路径报错且不执行 fallback draw。
- 打开诊断开关时才允许 fallback。

### P2：同步模型过于保守

相关文件：

- `D3D12Queue.cpp`
- `D3D12Device.cpp`

现象：

`Queue::submit()` 每次 submit 后同步等待 fence；`present()` 也等待 fence。该模型简单但会严重限制性能。

影响：

- CPU/GPU 无法并行。
- 帧率受 GPU 完成等待影响明显。

解决方案：

1. 引入多帧 in-flight frame context。
2. command allocator、descriptor allocator、upload resources 都绑定 frame fence。
3. submit 只 signal，不立即 wait；下一次复用 frame 资源前等待对应 fence。
4. present fence 和 submit fence 统一管理。

验证：

- GPUView/PIX 检查 CPU/GPU overlap。
- 压测多 draw call 场景的 frame time。

### P2：QueryPool 只完整覆盖 occlusion

相关文件：

- `D3D12QueryPool.cpp`
- `D3D12CommandBuffer.cpp`

现象：

`QueryType::TIMESTAMP` 和 `PIPELINE_STATISTICS` 没有完整映射；`beginQuery()` 对 timestamp 仍可能走 BeginQuery，而 D3D12 timestamp 只应 EndQuery。

影响：

- timestamp / pipeline statistics 查询不可用或行为错误。

解决方案：

1. 按 QueryType 创建对应 query heap：
   - occlusion -> `D3D12_QUERY_HEAP_TYPE_OCCLUSION`
   - timestamp -> `D3D12_QUERY_HEAP_TYPE_TIMESTAMP`
   - pipeline statistics -> `D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS`
2. timestamp 查询只使用 `EndQuery`。
3. 结果结构按查询类型解析。

验证：

- occlusion query、timestamp query、pipeline statistics query 分别跑最小用例。

## 推荐实施顺序

1. 修复 descriptor heap 生命周期，确保多 command buffer 不污染绑定。
2. 移除默认 PSO diagnostic fallback，改为显式诊断开关。
3. 补齐 `copyTextureToBuffers()`，建立 readback 验证能力。
4. 修复 `updateBuffer()` 的 offset/size 问题。
5. 统一 format capability 与 DXGI 映射，禁止错误 fallback。
6. 完善 barrier 的 depth read/write 和 subresource range。
7. 补 compute PSO、compute root binding 和 UAV/storage buffer。
8. 实现 scaling/filter blit。
9. 优化 fence/frame context，提高性能。
10. 补 query 类型。

## 最小回归测试清单

- Swapchain clear + draw triangle。
- Offscreen render target 写入后作为 texture 采样。
- Shadow map depth 写入后采样。
- Dynamic uniform buffer offset。
- Buffer view update offset。
- Texture upload + texture readback。
- Two command buffers same submit with different descriptors。
- Storage buffer compute write + readback。
- Scaling blit point/linear。
- ETC/ASTC 资产在 D3D12 下走明确 fallback 或被拒绝。

## 备注

本次只整理审查与优化方案，未修改 `native/cocos/renderer/gfx-d3d12/` 实现代码。
