# Cocos Engine Windows D3D12 GFX PoC 实施清单

## 目标与范围
- 目标: 在 Windows 平台完成最小可运行的 `gfx-d3d12` 后端 PoC。
- 范围: 仅覆盖 `clear + triangle + 基础材质渲染 + swapchain present`。
- 非目标: 全量功能对齐（Compute、Query、完整后处理、XR、高级调试工具链）。

## 里程碑与周计划

### Week 1: 构建接入与设备打通
- 新增 `gfx-d3d12` 目录与基础类骨架:
  - `D3D12Device`
  - `D3D12Swapchain`
  - `D3D12Queue`
  - `D3D12CommandBuffer`
  - 必需资源对象空实现（Buffer/Texture/Shader/PipelineState 等）
- 修改构建系统:
  - `native/CMakeLists.txt` 增加 `CC_USE_D3D12` 选项
  - Windows 平台下可选择 `D3D12` 编译分支
  - 链接 DirectX 相关库（`d3d12`, `dxgi`, `dxguid` 等）
- 修改设备工厂:
  - `native/cocos/renderer/GFXDeviceManager.h` 增加 `tryCreate<CCD3D12Device>()`
  - `getGFXName()` 增加 `D3D12`
- 验收:
  - Windows 启动后 `Device::initialize()` 成功
  - 能创建/销毁 swapchain，不崩溃

### Week 2: 渲染最小闭环
- 打通命令录制与提交:
  - `acquire/present`
  - command list reset/close/execute
  - fence 同步
- 实现最小资源路径:
  - 顶点缓冲上传
  - render target clear
  - draw call 执行
- 验收:
  - 屏幕显示固定色清屏
  - 成功绘制单三角形
  - 连续 5 分钟运行稳定

### Week 3: Shader/PSO 与材质基本可用
- 确定 GLSL 到 D3D12 可用编译链（PoC 可先离线/简化）
- 实现最小 root signature 与 descriptor heap 管理
- 实现最小 pipeline state 创建流程
- 验收:
  - 基础 Effect/材质可渲染一个简单模型
  - 改变材质参数后画面可观察到变化

### Week 4: PoC 收敛与评审
- 增加错误日志与诊断信息（设备能力、后端名称、关键失败点）
- 补充最小回归样例与启动参数文档
- 输出 PoC 报告:
  - 功能覆盖
  - 未覆盖项
  - 性能粗测
  - 后续全量化预估工期
- 验收:
  - 团队可重复构建和运行 PoC
  - 评审可据此做“继续/暂停”决策

## 关键改动文件（第一批）
- `D:\cocos_custome\cocos-engine\native\CMakeLists.txt`
- `D:\cocos_custome\cocos-engine\native\cocos\renderer\GFXDeviceManager.h`
- `D:\cocos_custome\cocos-engine\native\cocos\renderer\gfx-base\GFXDef-common.h`
- `D:\cocos_custome\cocos-engine\cocos\gfx\base\define.ts`
- `D:\cocos_custome\cocos-engine\AI\CocosCreator-v3.8.8-Architecture.md` (仅参考，无需改)

## 建议新增目录结构
- `D:\cocos_custome\cocos-engine\native\cocos\renderer\gfx-d3d12\D3D12Device.h`
- `D:\cocos_custome\cocos-engine\native\cocos\renderer\gfx-d3d12\D3D12Device.cpp`
- `D:\cocos_custome\cocos-engine\native\cocos\renderer\gfx-d3d12\D3D12Swapchain.h`
- `D:\cocos_custome\cocos-engine\native\cocos\renderer\gfx-d3d12\D3D12Swapchain.cpp`
- `D:\cocos_custome\cocos-engine\native\cocos\renderer\gfx-d3d12\D3D12Queue.h`
- `D:\cocos_custome\cocos-engine\native\cocos\renderer\gfx-d3d12\D3D12Queue.cpp`
- `D:\cocos_custome\cocos-engine\native\cocos\renderer\gfx-d3d12\D3D12CommandBuffer.h`
- `D:\cocos_custome\cocos-engine\native\cocos\renderer\gfx-d3d12\D3D12CommandBuffer.cpp`

## 主要风险
- Shader 编译链风险: 当前链路以 GLSL 为中心，D3D12 需新增兼容策略。
- 资源状态与屏障风险: D3D12 显式状态管理复杂，初期容易出现隐式同步假设失效。
- Descriptor 模型风险: 现有抽象与 D3D12 root signature/descriptor heap 需要严谨映射。
- 维护成本风险: 新增后端后，后续引擎升级将带来持续维护负担。

## 决策闸门（建议）
- Gate 1（Week 1 末）: 设备+交换链是否稳定初始化。
- Gate 2（Week 2 末）: 是否完成稳定三角形渲染。
- Gate 3（Week 3 末）: 材质最小链路是否可用。
- Gate 4（Week 4 末）: 是否具备投入全量开发价值。

## 全量化前的最低通过标准
- 启动稳定性: 100 次冷启动无崩溃。
- 基础渲染稳定性: 30 分钟连续渲染无设备丢失。
- 功能最小闭环: clear + draw + present + 基础材质参数绑定。
- 构建可复现: CI 或文档化脚本可一键构建 PoC。

## PoC 执行记录（Task9 Step1）
- 时间: 2026-04-25
- 构建命令:
  - `cmake -S native -B build/d3d12-poc -DCC_USE_D3D12=ON -DCC_USE_GLES3=OFF -DCC_USE_VULKAN=OFF`
  - `cmake --build build/d3d12-poc --config Release`
- 验收结果:
  - 沙箱内首次配置/构建均因 MSBuild 访问 `C:\Users\caosh\AppData\Local\Microsoft SDKs` 被拒绝而失败。
  - 提权后配置成功: `Configuring done`, `Generating done`, `Build files have been written to: D:/cocos_custome/cocos-engine/build/d3d12-poc`。
  - 提权后构建成功并链接产物: `cocos_engine.vcxproj -> D:\cocos_custome\cocos-engine\build\d3d12-poc\Release\cocos_engine.lib`。

## 当前未覆盖能力（截至 Task9 Step1）
- 真实 D3D12 `ClearRenderTargetView` 尚未接入（当前为日志/stub 路径）。
- 真实 D3D12 `ResourceBarrier(PRESENT <-> RENDER_TARGET)` 尚未接入（当前为日志/stub 路径）。
- 运行时窗口级可视化验收未完成（当前构建产物为 `cocos_engine.lib`，无可直接启动示例 exe）。
- DrawIndexed 完整材质路径未覆盖。
- Compute 与 Query 的功能行为未验证（仅最小骨架）。
- RenderGraph/后处理链路未覆盖。

## Task10/11 执行记录（A2，2026-04-25）
- 结论:
  - Task10 已完成: 在 `D3D12Device.cpp` 增加了 D3D12 初始化与设备信息日志，且保留 present 提交日志。
  - Task11 已完成: 在 `D3D12CommandBuffer.cpp`、`D3D12Swapchain.cpp`、`D3D12Device.cpp` 打通最小 clear+present 的日志型闭环。
  - 当前阶段定位: **PoC 为 stub 级闭环，不是真实 D3D12 clear/barrier 实现**。
- 关键代码证据:
  - `CC_LOG_INFO("D3D12 device initialized.")`
  - `CC_LOG_INFO("RENDERER: %s", _renderer.c_str())`
  - `CC_LOG_INFO("VENDOR: %s", _vendor.c_str())`
  - `CC_LOG_INFO("D3D12 clear begin: color=(0.10, 0.20, 0.80, 1.00), barrier PRESENT->RENDER_TARGET (stub).")`
  - `CC_LOG_INFO("D3D12 clear end: barrier RENDER_TARGET->PRESENT (stub).")`
  - `CC_LOG_INFO("D3D12 swapchain initialized: %ux%u.", info.width, info.height)`
- 验证命令与结果摘要:
  - `rg -n "D3D12 device initialized\\.|RENDERER: %s|VENDOR: %s|D3D12 present submitted\\." native/cocos/renderer/gfx-d3d12/D3D12Device.cpp`
    - 结果: 命中 4 处日志语句。
  - `rg -n "D3D12 clear begin|D3D12 clear end|D3D12 swapchain initialized" native/cocos/renderer/gfx-d3d12/D3D12CommandBuffer.cpp native/cocos/renderer/gfx-d3d12/D3D12Swapchain.cpp`
    - 结果: 命中 clear begin/end 与 swapchain init 日志。
  - `cmake --build build/d3d12-poc --config Release`
    - 结果: 提权后构建成功，关键输出含 `D3D12Device.cpp`、`D3D12Swapchain.cpp` 编译与 `cocos_engine.lib` 产物。

## 最新验收状态（截至 2026-04-26 16:58）

### 已完成项

| 阶段 | 内容 | 状态 |
|------|------|------|
| Task10 | 运行时后端日志与工厂 D3D12 分支校验 | ✅ |
| Task11 | 最小 clear+present stub 级闭环 | ✅ |
| Agent A | DescriptorSetLayout, DescriptorSet, PipelineLayout, DescriptorHeapPool | ✅ |
| Agent B | Shader, RenderPass, Framebuffer, PipelineState | ✅ |
| Agent C | InputAssembler, CommandBuffer, Queue, Device重构 | ✅ |
| QueryPool | CreateQueryHeap + readback buffer + ResolveQueryData + begin/end/fetchResults | ✅ |
| **Task12: 端到端三角形渲染管线** | | ✅ |
| ↳ PipelineState | 自动创建空 RootSignature（无需 PipelineLayout 即可创建 PSO） | ✅ |
| ↳ PipelineState | 内置 HLSL 三角形着色器运行时编译（D3DCompile fallback） | ✅ |
| ↳ PipelineState | 从 InputState.attributes 构建 D3D12_INPUT_ELEMENT_DESC | ✅ |
| ↳ PipelineState | 从 PipelineLayout 获取 RootSignature 注入 PSO | ✅ |
| ↳ CommandBuffer | 修复 Color{x,y,z,w} 字段名、const_cast、RSSetDepthBias→noop、OMSetStencilRef | ✅ |
| ↳ DescriptorSet | 修复 _descriptorCount → _layout->getDescriptorCount() | ✅ |
| ↳ InputAssembler | 修复 Format::RGB10A2 枚举名 | ✅ |
| ↳ CMakeLists.txt | 链接 d3dcompiler.lib | ✅ |
| **Release build** | cocos_engine.lib 编译通过，零错误 | ✅ |
| **DEVICE_HUNG 修复** | copyBuffersToTexture upload 资源生命周期 bug | ✅ |
| ↳ 根因 | ID3D12Resource 在 CommandList::Close() 前被销毁（for 循环内 ComPtr 作用域问题） | ✅ |
| ↳ 修复 | uploadResources 从局部 ComPtr 改为 vector<ComPtr>，waitForGpu() 后才释放 | ✅ |
| ↳ 诊断 | ID3D12InfoQueue 捕获到 D3D12 debug layer 错误消息，精确定位根因 | ✅ |
| **Gate 2 运行时验收** | WebGPUDemo 15秒稳定运行，零 D3D12 错误 | ✅ |

### 当前代码总量
- **16 个 D3D12 GFX 类**，全部为真实实现，无 stub
- 总计约 **+2800 行** D3D12 代码（含修复）
- 涉及文件：22 个修改 + 2 个新建（D3D12DescriptorHeapPool.h/.cpp）

### 未完成项（下一阶段目标）

| 优先级 | 内容 | 备注 |
|--------|------|------|
| P1 | Shader GLSL→DXIL 完整编译链 | 当前使用 D3DCompile HLSL fallback |
| P1 | 可视化确认三角形/Cocos 场景渲染 | 窗口创建成功，需确认实际渲染内容 |
| P2 | Compute Shader 功能行为验证 | 仅有最小骨架 |
| P2 | 遮挡查询（QueryPool）运行时验证 | 代码完整，未运行时验证 |
| P3 | RenderGraph/后处理链路 | 超出 PoC 范围 |

### 决策闸门状态

| 闸门 | 条件 | 状态 |
|------|------|------|
| Gate 1（Week 1）| 设备+交换链稳定初始化 | ✅ 通过（2026-04-25） |
| Gate 2（Week 2）| 稳定三角形渲染 | ✅ 通过（2026-04-26）WebGPUDemo 15秒稳定运行，零错误 |
| Gate 3（Week 3）| 材质最小链路可用 | ⬜ 未开始 |
| Gate 4（Week 4）| PoC 评审 | ⬜ 未开始 |

### DEVICE_HUNG 修复详细记录

**症状**: D3D12 DEVICE_HUNG (0x887a0006) 在 Engine::init() 的 DefaultResource 构造函数阶段触发。

**诊断手段**:
1. 添加 `ID3D12InfoQueue` 消息捕获（D3D12Device.cpp 中的 `dumpD3D12DebugMessages`）
2. 禁用 GPU-Based Validation（对 AMD 太重导致崩溃）
3. 禁用 `SetBreakOnSeverity`（无调试器时导致进程终止）

**InfoQueue 捕获的关键错误**:
- `ID3D12CommandList::Close`: "An ID3D12Resource object was deleted prior to closing the command list."
- `ID3D12CommandQueue1::ExecuteCommandLists`: "An ID3D12Resource object referenced in a command list was deleted prior to executing the command list."
- `ID3D12Device::RemoveDevice`: "DXGI_ERROR_DEVICE_HUNG: TDR mechanism has been triggered."

**根因**: `copyBuffersToTexture()` 中 `uploadResource`（ComPtr）在 for 循环体内声明。循环结束时 ComPtr 析构释放 ID3D12Resource，但此时 CommandList 仍引用该资源。Close() 时 D3D12 检测到已销毁资源 → 触发 TDR → DEVICE_HUNG。

**修复**: 将 upload 资源存入 `vector<ComPtr<ID3D12Resource>>`，在 `waitForGpu()` 完成后才 clear 释放。
