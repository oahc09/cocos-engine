# PipelineUBO::updateShadowUBO → D3D12 updateBuffer 实现分析

**范围**: `PipelineUBO.cpp:587-588` 中的 `cmdBuffer->updateBuffer()` 调用链
**对比**: D3D12 vs GLES3/Vulkan

---

## 一、调用链追踪

### PipelineUBO.cpp:587-588
```cpp
ds->update();                                                                // ① 刷新 DescriptorSet
cmdBuffer->updateBuffer(ds->getBuffer(UBOShadow::BINDING), _shadowUBO.data(), UBOShadow::SIZE);  // ②
cmdBuffer->updateBuffer(ds->getBuffer(UBOCSM::BINDING), _csmUBO.data(), UBOCSM::SIZE);
```

### Buffer 创建（PipelineUBO.cpp:478-484）
```cpp
auto *shadowUBO = _device->createBuffer({
    gfx::BufferUsageBit::UNIFORM | gfx::BufferUsageBit::TRANSFER_DST,
    gfx::MemoryUsageBit::DEVICE,
    UBOShadow::SIZE,           // 256 bytes
    UBOShadow::SIZE,
    gfx::BufferFlagBit::NONE,  // ← 无 ENABLE_STAGING_WRITE
});
descriptorSet->bindBuffer(UBOShadow::BINDING, shadowUBO);
```

---

## 二、D3D12 完整实现链

### Step 1: `ds->getBuffer(UBOShadow::BINDING)`
- 返回基类 `DescriptorSet::_buffers[UBOShadow::BINDING]` 中的 Buffer 指针
- 这个 Buffer 在 init 时创建，整个生命周期不变（除非 resize）

### Step 2: `D3D12CommandBuffer::updateBuffer(buff, data, size)` (行 1009-1027)
```cpp
auto *resource = d3d12Buffer->getD3D12ResourceHandle(); // ID3D12Resource*
resource->Map(0, &readRange, &mappedData);              // Map UPLOAD heap
std::memcpy(mappedData, data, size);                     // CPU copy
resource->Unmap(0, &writeRange);                         // Unmap
```

### Step 3: D3D12 Buffer 资源属性
```cpp
// D3D12Buffer::createResource (行 156)
heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;  // CPU 可直接读写
resourceDesc.Width = (size + 255) & ~255;      // 256 字节对齐
```

### Step 4: GPU Shader 读取
- DescriptorSet 的 CBV 指向 `buffer->getD3D12GPUVirtualAddress()`
- GPU 通过 Root Signature → Descriptor Table → CBV 读取此地址
- UPLOAD heap 同时可被 CPU 写 + GPU 读

---

## 三、与 GLES3/Vulkan 差异对比

| 项目 | D3D12 | GLES3 | Vulkan |
|------|-------|-------|--------|
| **更新方式** | `Map/memcpy/Unmap` | `glBufferSubData` | `vkMapMemory/memcpy/vkUnmapMemory` |
| **执行模型** | 直接 CPU 写 | GPU 命令（延迟） | 直接 CPU 写 或 staging copy |
| **内存类型** | UPLOAD heap | GL 驱动管理 | HOST_VISIBLE |
| **同步机制** | UPLOAD heap 无同步需要 | `glBufferSubData` 隐含同步 | Memory barrier |
| **调用时机** | CommandList 录制期间 | CommandBuffer 录制期间 | CommandBuffer 录制期间 |

### 关键差异分析

**D3D12**: `Map`/`memcpy`/`Unmap` 是纯 CPU 操作。UPLOAD heap 允许 CPU 随时写，GPU 随时读，两者无冲突。数据立即对 GPU 可见。

**GLES3**: `glBufferSubData` 是 GPU 命令，加入命令队列。驱动保证后续 draw 看到新数据。

**Vulkan**: 与 D3D12 类似，`vkMapMemory` 映射 host-visible 内存，memcpy 写，`vkUnmapMemory` 解除映射。需要 `VK_ACCESS_UNIFORM_READ_BIT` 屏障。

**三者等价性**: ✅ 功能等价。都是在 draw 前将 CPU 数据同步到 GPU 可见内存。

---

## 四、准确性验证

### ✅ 1. Buffer 资源类型正确
- D3D12: `D3D12_HEAP_TYPE_UPLOAD` → CPU 可 Map，GPU 可直接读
- GLES3: `GL_ARRAY_BUFFER` / `GL_UNIFORM_BUFFER` → glBufferSubData 可写
- 两者都是正确的 UBO 资源类型

### ✅ 2. 数据大小正确
- `UBOShadow::SIZE` = 256 bytes（CCShadow UBO 定义大小）
- D3D12 资源对齐到 256 bytes → 实际分配 256 bytes
- memcpy 大小 = `UBOShadow::SIZE` → 数据完整复制

### ✅ 3. 偏移量正确
- UBOShadow buffer 是独立 Buffer（非 BufferView）
- `_impl->resourceOffset = 0` → memcpy 写到资源起始位置
- 如为 BufferView（非本场景），offset 会被忽略 ← **潜在隐患但此处不触发**

### ✅ 4. 描述符集一致性
- `ds->update()` (forceUpdate) → 刷新 CBV 描述符
- CBV 指向 `buffer->getD3D12GPUVirtualAddress()` = 资源基地址 + offset
- `updateBuffer` 写入同一地址 → CBV 读取最新数据
- 调用顺序 `ds->update()` → `updateBuffer()` 不影响正确性（GPU VA 不变）

### ✅ 5. 无竞态条件
- `updateShadowUBO` 在 CommandList **录制阶段**调用
- GPU 尚未执行此 CommandList → 无读写冲突
- UPLOAD heap 无隐式同步 → 不需要额外 barrier

### ⚠️ 6. 隐式假设（不构成问题但需注意）
- `getBuffer()` 返回的 Buffer 对象生命周期必须覆盖 `updateBuffer` 执行 → ✅ 持有在 `_ubos` vector
- 如在 `ds->update()` 和 `updateBuffer()` 之间发生 buffer resize → buffer 被替换为新资源，但 CBV 仍指向旧 VA → ⚠️ **理论上的时序漏洞，但此场景不发生**

---

## 五、结论

**D3D12 的 `updateBuffer` 实现是正确的，与 GLES3/Vulkan 功能等价。**

`PipelineUBO.cpp:587-588` 在 D3D12 后端的执行流程：
1. UPLOAD heap 资源的 `Map/memcpy/Unmap` 成功将 `_shadowUBO` CPU 数据写入 GPU 可见内存
2. DescriptorSet 的 CBV 指向该内存地址，GPU shader 可正确读取
3. 不存在数据未到达、偏移错误、描述符失效等问题

**阴影不显示的根因不在这段代码路径中。** 问题应在其他 D3D12 特定环节（如 DescriptorSet 的 Root Signature 映射、CommandList 中的 setDescriptorSet 绑定、或 PSO 管线状态）。
