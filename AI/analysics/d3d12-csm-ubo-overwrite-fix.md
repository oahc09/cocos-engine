# D3D12 CSM 多 Level UBO 覆盖问题 — 根因与修复总结

**日期**: 2026-05-24
**影响范围**: D3D12 GFX 后端
**最终方案**: snapshot + replaceD3D12Resource + forceUpdate

---

## 一、根因

### 1.1 直接原因

D3D12 `updateBuffer` 使用 CPU `Map/memcpy/Unmap` 直接写入 UPLOAD heap buffer。同一个 UBO（如 CCShadow）被 CSM 多个 level / forward-add 多个光源连续调用时，后面的写入覆盖同一块 GPU 内存。GPU 执行时所有 draw 读到的都是最后一次写入的数据。

```
CPU 录制阶段:
  updateBuffer(buf, lv0_data) → Map/memcpy → UPLOAD heap
  录制 draw_lv0
  updateBuffer(buf, lv1_data) → Map/memcpy → 覆盖 UPLOAD heap
  录制 draw_lv1

GPU 执行阶段:
  draw_lv0 读取 buf → lv1_data ❌ (已被覆盖)
  draw_lv1 读取 buf → lv1_data ✅
```

### 1.2 与 GLES3/Vulkan 的本质差异

| 后端 | 实现 | 执行模型 |
|------|------|----------|
| **GLES3** | `glBufferSubData` | GPU 命令，按序执行 ✅ |
| **Vulkan** | `vkCmdUpdateBuffer` | GPU 命令，按序执行 ✅ |
| **D3D12(旧)** | `Map/memcpy/Unmap` | CPU 操作，录制时覆盖 ❌ |

---

## 二、尝试过的方案

### 2.1 Deferred Queue（失败）

**思路**: `updateBuffer` 不立即写入，记录到队列；`draw()` 前一次性 flush。

**失败原因**: `flush` 仍然是 CPU `Map/memcpy`，录制多个 level 时不同 draw 的 flush 在 CPU 时间线上是顺序的——后面的 flush 仍会覆盖前面的数据。

### 2.2 CopyBufferRegion + DEFAULT heap（失败）

**思路**: 用 GPU 命令 `CopyBufferRegion` 替代 CPU Map/memcpy。目标 buffer 需要 `COPY_DEST` 状态，因此改为 DEFAULT heap。

**失败原因**: 其他代码路径（`PipelineUBO::_cameraBuffer->update()`）对 DEFAULT heap 调用 `Map()` 失败。DEFAULT heap 不能 Map。

---

## 三、最终方案：snapshot + replaceD3D12Resource + forceUpdate

### 3.1 原理

每个 `updateBuffer` 调用创建一个**全新的 UPLOAD heap buffer**（snapshot），将数据写入 snapshot，然后用 `replaceD3D12Resource` 原子替换目标 buffer 的底层 D3D12 资源。旧资源挂到 `pendingUploadResources` 保活到帧末。

```
Level 0:
  updateBuffer(buf, lv0_data):
    1. CreateCommittedResource(UPLOAD) → snapshot_lv0
    2. Map/memset/memcpy/Unmap(snapshot_lv0, lv0_data)
    3. old = buf->replaceD3D12Resource(snapshot_lv0)
    4. pendingUploadResources += old     ← 旧资源不释放
    5. pendingUploadResources += snapshot_lv0
    6. forceUpdate()                     ← 重建所有 pending DS 的 CBV
  draw_lv0 → CBV → GPU VA_lv0 (snapshot_lv0) ✅

Level 1:
  updateBuffer(buf, lv1_data):
    1. CreateCommittedResource(UPLOAD) → snapshot_lv1
    2. Map/memset/memcpy/Unmap(snapshot_lv1, lv1_data)
    3. old = buf->replaceD3D12Resource(snapshot_lv1)
    4. pendingUploadResources += old (= snapshot_lv0)  ← 仍存活
    5. pendingUploadResources += snapshot_lv1
    6. forceUpdate()
  draw_lv1 → CBV → GPU VA_lv1 (snapshot_lv1) ✅
```

每个 level 的 draw 读取**完全不同的 GPU VA**，数据永不互相覆盖。

### 3.2 与 GLES3/Vulkan 的等价性

```
GLES3:  glBufferSubData(buf, lv0) → Draw → glBufferSubData(buf, lv1) → Draw
Vulkan: vkCmdUpdateBuffer(buf, lv0) → Draw → vkCmdUpdateBuffer(buf, lv1) → Draw
D3D12:  replaceResource(buf, snapshot_lv0) → Draw → replaceResource(buf, snapshot_lv1) → Draw
                                              ↑
                         不同的 GPU 资源，不同的 VA，数据隔离
```

### 3.3 缓冲区视图兼容

Buffer view（如 `_firstLightBufferView`）通过 `_impl->parent` 链自动解析父 buffer 的新资源：

```cpp
// D3D12Buffer.cpp
void *getD3D12ResourceHandle() const {
    if (_impl->parent) return _impl->parent->getD3D12ResourceHandle(); // 跟随父链
}
uint64_t getD3D12GPUVirtualAddress() const {
    if (_impl->parent) return _impl->parent->getD3D12GPUVirtualAddress() + _impl->resourceOffset;
}
```

`replaceD3D12Resource` 替换父 buffer 的资源后，view 自动获取新的 GPU VA。

---

## 四、改动代码

### 4.1 `native/cocos/renderer/gfx-d3d12/D3D12CommandBuffer.cpp`

**函数**: `CCD3D12CommandBuffer::updateBuffer`（行 1009-1096）

核心逻辑：对于 UNIFORM 且非 buffer view 的 buffer，创建 snapshot 并原子替换。

```cpp
if (!buff->isBufferView() && hasFlag(buff->getUsage(), BufferUsageBit::UNIFORM)) {
    // 1. 创建 UPLOAD heap snapshot 资源
    // 2. Map/memset/memcpy/Unmap
    // 3. replaceD3D12Resource(snapshot)
    // 4. 旧资源、新 snapshot 都加入 pendingUploadResources
    // 5. forceUpdate 所有 pending descriptor set
}
// Fallback: 对于 buffer view 和非 UNIFORM buffer，保持原有 Map/memcpy
```

### 4.2 未修改的文件

| 文件 | 说明 |
|------|------|
| `PipelineUBO.cpp/.h` | 不变——GLES3/Vulkan 共享代码 |
| `D3D12Buffer.cpp/.h` | 不变——仍用 UPLOAD heap |
| `D3D12DescriptorSet.cpp` | 不变——`forceUpdate` 正常重建 CBV |
| `D3D12PipelineState.cpp` | 不变 |

---

## 五、性能考量

| 指标 | 开销 |
|------|------|
| `CreateCommittedResource` | 每个 UNIFORM `updateBuffer` 调用 1 次 |
| `forceUpdate` | 每个 `updateBuffer` 调用 N 次（N = pending descriptor set 数量） |
| 内存 | 旧资源累积到帧末一次性释放 |
| 适用场景 | CSM 多 level（2-4 次/帧）、forward-add 多光源（每个光源 1 次） |

对典型场景（CSM 4 level × 2 UBO = 8 次/帧），每次创建 256B 的 UPLOAD 资源，开销可接受。

---

## 六、测试验证

1. 编译 D3D12 Debug 构建
2. 运行 CSM 场景（多 level shadow）
3. RenderDoc 抓帧，对比 level 0 和 level 1 的 CCShadow UBO 数据——应不同
4. 运行 forward-add 多光源场景，确认每个光源的 UBO 数据正确
5. D3D12 验证层无错误
