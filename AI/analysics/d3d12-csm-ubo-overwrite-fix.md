# D3D12 Shadow CSM 多 Level UBO 覆盖问题 — 根因与修复总结

**日期**: 2026-05-24
**影响范围**: D3D12 GFX 后端

---

## 一、问题表现

D3D12 渲染后端下，CSM（Cascaded Shadow Map）多个 level 连续录制 draw 时，所有 draw 读到的 CCShadow UBO 矩阵数据都是**最后一组**（{-479, -255, -1220}），导致阴影异常。

## 二、根因分析

### 2.1 直接原因

`PipelineUBO::updateShadowUBO()` 中：

```cpp
ds->update();   // 刷新 descriptor set
cmdBuffer->updateBuffer(ds->getBuffer(UBOShadow::BINDING), _shadowUBO.data(), UBOShadow::SIZE);
cmdBuffer->updateBuffer(ds->getBuffer(UBOCSM::BINDING), _csmUBO.data(), UBOCSM::SIZE);
```

D3D12 的 `updateBuffer` 实现为 **CPU Map/memcpy/Unmap** 直接写入 UPLOAD heap buffer。同一个 buffer 被多个 CSM level 连续调用时：

```
CPU 录制阶段:
  Map/memcpy/Unmap(level0_data) → 写入 buf
  draw0 录制
  Map/memcpy/Unmap(level1_data) → 覆盖 buf (同一块内存)
  draw1 录制

GPU 执行阶段:
  draw0 读取 buf → 拿到 level1_data ❌  (已被覆盖)
  draw1 读取 buf → 拿到 level1_data ✅
```

### 2.2 与 GLES3/Vulkan 的本质差异

| 后端 | 实现 | 执行模型 | CSM 多 level |
|------|------|----------|-------------|
| **GLES3** | `glBufferSubData` | GPU 命令，按序执行 | ✅ 每个 draw 看到自己的数据 |
| **Vulkan** | `vkCmdUpdateBuffer` | GPU 命令，按序执行 | ✅ 每个 draw 看到自己的数据 |
| **D3D12** | `Map/memcpy/Unmap` | **CPU 立即覆写** | ❌ 后写覆盖前读 |

GLES3/Vulkan 的 buffer 更新是 **GPU 命令**，录制在 command buffer 中按时间顺序执行。D3D12 的 Map/memcpy 是 **CPU 操作**，发生在录制阶段，导致后面的调用立即覆盖前面的数据。

### 2.3 已验证排除的假因

| 排查项 | 结论 |
|--------|------|
| `getBuffer()` 返回错误 buffer | ✅ 正确，与 forceUpdate 使用同一个 `_buffers` |
| `forceUpdate()` CBV 未更新 | ✅ 正确，GPU VA 始终指向同一 buffer |
| 深度编码错误 (RGBA packed vs R32F) | ✅ 正确，packing=0 模式 C 简单深度 |
| CullMode FRONT 导致 PS 不执行 | ❌ 独立问题，非本次根因 |
| Model::updateUBOs 未写 cc_matWorld | ✅ 正确，staging→flush 链路完好 |

---

## 三、修复方案

### 设计原则

1. 只改 D3D12 后端文件 (`gfx-d3d12/`)，不动共享代码
2. 不改 Buffer heap 类型，不改 DescriptorSet，不改 PipelineUBO
3. 使 D3D12 行为与 GLES3/Vulkan 等价

### 方案：延迟写入队列 (Deferred Buffer Write Queue)

#### 原理

`updateBuffer` 不立即写入 GPU，而是记录待写数据。在 **draw 前** 一次性 flush，只写入当前 draw 需要的数据。

```
Level 0:
  ds->update()              → CBV 指向 buf GPU VA
  updateBuffer(lv0_data)    → 入队 {buf, lv0_data}（仅记录）
  flushDescriptorSets()     → 准备 GPU heap
  flushDeferredWrites()     → Map/memcpy/Unmap → buf = lv0_data
  draw0()                   → 读到 lv0_data ✅
  队列清空

Level 1:
  ds->update()              → CBV 指向同一个 VA
  updateBuffer(lv1_data)    → 入队 {buf, lv1_data}
  flushDescriptorSets()
  flushDeferredWrites()     → Map/memcpy/Unmap → buf = lv1_data
  draw1()                   → 读到 lv1_data ✅
```

#### 改动的代码

**文件**: `native/cocos/renderer/gfx-d3d12/D3D12CommandBuffer.cpp`

**1) Impl 结构体新增成员**（行 152 之后）:

```cpp
struct DeferredBufferWrite {
    CCD3D12Buffer *buffer{nullptr};
    ccstd::vector<uint8_t> data;
};
ccstd::vector<DeferredBufferWrite> deferredBufferWrites;
```

**2) `updateBuffer` 改为延迟记录**（替换行 1020-1107）:

```cpp
void CCD3D12CommandBuffer::updateBuffer(Buffer *buff, const void *data, uint32_t size) {
    if (!_impl->commandList || !buff || !data || size == 0) return;

    auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(buff);
    const uint32_t copySize = std::min(size, buff->getSize());
    if (copySize == 0) return;

    DeferredBufferWrite write{};
    write.buffer = d3d12Buffer;
    write.data.assign(static_cast<const uint8_t *>(data),
                      static_cast<const uint8_t *>(data) + copySize);
    _impl->deferredBufferWrites.push_back(std::move(write));
}
```

**3) 新增 `flushDeferredBufferWrites`**:

```cpp
void CCD3D12CommandBuffer::flushDeferredBufferWrites() {
    if (_impl->deferredBufferWrites.empty()) return;

    for (auto &w : _impl->deferredBufferWrites) {
        auto *resource = static_cast<ID3D12Resource *>(w.buffer->getD3D12ResourceHandle());
        if (!resource) continue;

        void *mappedData = nullptr;
        D3D12_RANGE readRange{};
        HRESULT hr = resource->Map(0, &readRange, &mappedData);
        if (FAILED(hr) || !mappedData) continue;

        auto *dst = static_cast<uint8_t *>(mappedData) + w.buffer->getD3D12ResourceOffset();
        std::memcpy(dst, w.data.data(), w.data.size());
        D3D12_RANGE writeRange{
            w.buffer->getD3D12ResourceOffset(),
            w.buffer->getD3D12ResourceOffset() + w.data.size()};
        resource->Unmap(0, &writeRange);
    }
    _impl->deferredBufferWrites.clear();
}
```

**4) `draw()` 中插入 flush 调用**（在 `flushDescriptorSets()` 之后，`DrawIndexedInstanced` 之前）:

```cpp
flushDescriptorSets();
flushDeferredBufferWrites();  // ← 新增
```

同样在 `dispatch()` 中插入。

**5) `begin()` 中清理残留**:

```cpp
_impl->deferredBufferWrites.clear();
```

---

## 四、方案优势

| 对比项 | 现有 snapshot 方案 | 延迟队列方案 |
|--------|-------------------|-------------|
| D3D12 资源创建 | 每次 `updateBuffer` 调用 1 次 `CreateCommittedResource` | 0 次 |
| 内存累积 | `pendingUploadResources` 持续增长至帧末 | 仅队列中的 `vector<uint8_t>` |
| 代码复杂度 | Map/memset/memcpy/Unmap + replaceD3D12Resource + forceUpdate × N | Map/memcpy/Unmap + clear |
| 改动范围 | 已实现（但有过多的资源生命周期管理） | 仅 D3D12CommandBuffer.cpp 一个文件 |

---

## 五、测试验证

1. 编译 D3D12 Debug 构建
2. 运行 CSM 场景（多 level shadow）
3. RenderDoc 抓帧，对比 CSM level 0 和 level 1 的 CCShadow UBO 数据
4. 验证阴影贴图各部分（对应不同 CSM level）内容是否正确

---

## 六、相关文件

| 文件 | 角色 |
|------|------|
| `native/cocos/renderer/gfx-d3d12/D3D12CommandBuffer.cpp` | **修复目标** |
| `native/cocos/renderer/gfx-d3d12/D3D12CommandBuffer.h` | 新增 `flushDeferredBufferWrites` 声明 |
| `native/cocos/renderer/pipeline/PipelineUBO.cpp` | 调用方（不修改） |
| `native/cocos/renderer/gfx-gles3/GLES3Commands.cpp` | GLES3 `glBufferSubData` 参考 |
| `native/cocos/renderer/gfx-vulkan/VKCommands.cpp` | Vulkan `vkCmdUpdateBuffer` 参考 |
