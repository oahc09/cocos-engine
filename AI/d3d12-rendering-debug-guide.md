# D3D12 渲染问题排查与修复指南

**日期**: 2026-05-02
**适用**: Cocos Creator v3.8.8 D3D12 GFX 后端

---

## 一、调试工具链

### 1.1 D3D12 Debug Layer（已集成）

引擎初始化时已启用，输出到 Visual Studio Output 窗口：

```cpp
// 已存在于 D3D12Device::initializeD3D12Context
ID3D12Debug *debugCtrl = nullptr;
D3D12GetDebugInterface(IID_PPV_ARGS(&debugCtrl));
debugCtrl->EnableDebugLayer();
```

**检测能力**：
- 无效的资源状态转换
- 描述符类型不匹配
- PSO 创建失败原因
- 命令列表录制错误

### 1.2 RenderDoc（已集成）

`renderdoc_app.h` 已在 `gfx-d3d12/` 目录中。

**使用方式**：

1. 安装 [RenderDoc](https://renderdoc.org/)
2. 通过 RenderDoc Launch 程序，或代码触发 capture：
   ```cpp
   #include "renderdoc_app.h"
   RENDERDOC_API_1_1_2 *rdocAPI = nullptr;
   // 初始化后...
   rdocAPI->TriggerCapture();
   ```
3. 逐 draw call 检查：
   - **Pipeline State** → PSO、root signature、描述符绑定
   - **Texture Viewer** → RT 输出、深度缓冲、纹理内容
   - **Mesh Viewer** → 顶点数据是否正确上传
   - **API Inspector** → 每个 API 调用的参数

### 1.3 PIX for Windows（微软官方）

**适用场景**：
- GPU timing 分析（性能瓶颈定位）
- `ExecuteIndirect` 参数验证
- Descriptor heap 内容查看
- 命令队列时序分析

下载：https://devblogs.microsoft.com/pix/

---

## 二、引擎内置诊断手段

### 2.1 资源状态追踪

D3D12Texture 已内置状态追踪：
```cpp
// 每个纹理记录当前 D3D12_RESOURCE_STATES
D3D12_RESOURCE_STATES getCurrentState() const;
void setCurrentState(D3D12_RESOURCE_STATES state);
```

在 `pipelineBarrier` 中自动使用追踪状态（当 `prevAccesses == NONE` 时）。

### 2.2 Shader 编译管线

```
GLSL (#version 450)
  → glslang → SPIR-V
  → SPIRV-Cross → HLSL (SM 5.1)
  → D3DCompile → DXBC
```

任何一环出错都会导致渲染异常。排查方法：
- 检查 `D3D12Shader::doInit` 中的编译日志
- 对比 Vulkan/GLES3 的 shader 编译结果
- 用 `fxc` / `dxc` 直接编译生成的 HLSL 验证

### 2.3 描述符绑定验证

```cpp
// 在 flushDescriptorSets() 中设断点
// 检查每个 binding 的：
//   - 类型 (CBV/SRV/UAV/Sampler)
//   - 资源指针
//   - 偏移量
```

### 2.4 Null 描述符安全机制

Device 已内置 dummy 资源：
- `getDummyTexture()` → 1x1 RGBA8 纹理（用于 null SRV/UAV）
- `getDummyBuffer()` → 256B buffer（用于 null CBV）
- `forceUpdate()` 中 null 绑定写入 null CBV / dummy SRV-UAV / default sampler

---

## 三、常见渲染问题排查路径

### 3.1 症状 → 原因 → 排查方法

| 症状 | 可能原因 | 排查方法 |
|------|---------|---------|
| **黑屏（无输出）** | PSO 创建失败 / RT 未绑定 / loadOp 错误 | RenderDoc 检查 PSO state 和 RT 绑定 |
| **DEVICE_HUNG (0x887a0006)** | 资源生命周期 / 竞争条件 / null 描述符 | D3D12 Debug Layer + GPU 崩溃转储 |
| **闪烁/撕裂** | 双缓冲同步问题 / 资源状态错误 | 检查 Swapchain present mode + barrier |
| **纹理采样错误** | descriptor heap 越界 / 格式不匹配 | RenderDoc Texture Viewer 检查 SRV |
| **几何体消失** | Vertex buffer 上传失败 / IA 配置错误 | RenderDoc Mesh Viewer 检查顶点数据 |
| **颜色错误** | Shader uniform 绑定偏移 / 格式转换 | RenderDoc 检查 CBV 内容 |
| **深度测试失败** | Depth format 错误 / depth buffer 未绑定 | 检查 DSV format + RESOURCE_STATE |
| **Compute shader 无输出** | UAV barrier 缺失 / dispatch 参数错误 | 添加 UAV barrier + 检查 thread group count |

### 3.2 系统性排查流程

```
1. 开启 D3D12 Debug Layer（已默认开启）
   ↓
2. 检查 Device Removed Reason
   HRESULT drr = device->GetDeviceRemovedReason();
   ↓
3. RenderDoc 抓帧 → 逐 draw call 检查
   ├─ PSO 是否有效？
   ├─ Root Signature 是否匹配？
   ├─ 描述符堆是否正确绑定？
   ├─ VB/IB 数据是否正确？
   └─ RT/DS 是否绑定？
   ↓
4. 对比 Vulkan/GLES3 后端的同一帧
   ↓
5. 简化场景 → 逐步复杂化
   ├─ 先画一个三角形（纯色）
   ├─ 再画一个带纹理的
   ├─ 再加上 uniform（CBV）
   └─ 最后恢复完整场景
```

---

## 四、实用调试技巧

### 4.1 快速定位 GPU 崩溃的 draw call

```cpp
// 临时在 CommandBuffer 中加（二分法定位）
device->GetDeviceRemovedReason();  // draw 前
commandList->DrawIndexedInstanced(...);
device->GetDeviceRemovedReason();  // draw 后
```

### 4.2 验证描述符内容

```cpp
// 用 RenderDoc 的 Descriptor Viewer 直接查看
// 或用 ID3D12Device::GetCopyableFootprints 验证纹理布局
```

### 4.3 对比测试法

- 同一场景在 GLES3 后端渲染正确 → 截图对比
- 逐步注释 D3D12 特有的优化代码找到问题：
  - 延迟描述符绑定 (`flushDescriptorSets`)
  - null 描述符写入 (`forceUpdate`)
  - 诊断 fallback shader

### 4.4 设备移除诊断

```cpp
// D3D12Device::waitForGpu() 中已有 fence 机制
// 如果需要更详细的诊断：
HRESULT drr = _impl->d3dDevice->GetDeviceRemovedReason();
if (FAILED(drr)) {
    CC_LOG_ERROR("Device Removed! Reason: 0x%08x", (unsigned)drr);
    // 0x887a0006 = DXGI_ERROR_DEVICE_HUNG (GPU 执行了非法操作)
    // 0x887a0005 = DXGI_ERROR_DEVICE_REMOVED (设备被移除)
    // 0x887a0020 = DXGI_ERROR_DRIVER_INTERNAL_ERROR
}
```

### 4.5 资源泄漏检查

```cpp
// 在程序退出前：
ID3D12DebugDevice *debugDevice = nullptr;
if (SUCCEEDED(_impl->d3dDevice->QueryInterface(&debugDevice))) {
    debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL);
    debugDevice->Release();
}
// 输出到 VS Output 窗口，显示所有存活的 D3D12 对象及其 refcount
```

---

## 五、D3D12 后端关键修复历史

| 问题 | 根因 | 修复方案 |
|------|------|---------|
| Swapchain 纹理失效 | 静态缓存 back buffer | 动态 `getCurrentBackBufferHandle()` |
| DEVICE_HUNG | copyBuffersToTexture upload 资源生命周期 | ComPtr 作用域修复 |
| PSO 黑屏 | InputLayout 与 fallback shader 不匹配 | 清除 InputLayout 重试 |
| SPIRV-Cross HLSL 绑定 | register_space/set 映射错误 | `register_space=set`, 入口点统一 "main" |
| loadOp=LOAD 仍清屏 | beginRenderPass 无条件 ClearRenderTarget | 按 loadOp 条件 clear |
| 描述符绑定时机 | bindDescriptorSet 立即 flush 效率低 | 延迟绑定，draw 前 flush |
| Null 描述符 DEVICE_REMOVED | forceUpdate 跳过 null 绑定写入 | 写入 null CBV / dummy SRV-UAV |

---

## 六、快速参考

| 工具 | 用途 | 获取方式 |
|------|------|---------|
| D3D12 Debug Layer | API 调用验证 | 已集成 |
| RenderDoc | 单帧抓取分析 | renderdoc.org |
| PIX | GPU 性能分析 | devblogs.microsoft.com/pix |
| Visual Studio Graphics Debugger | 集成 GPU 调试 | VS 内置 |
| `fxc` / `dxc` | HLSL 编译验证 | Windows SDK |
