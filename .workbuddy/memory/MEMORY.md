# Cocos Creator Engine 项目记忆

## 项目基本信息
- **引擎版本**: Cocos Creator v3.8.8 (MIT 许可证)
- **分支**: v3.8.8_custome
- **工作目录**: d:\cocos_custome\cocos-engine
- **构建**: VS 2022 x64, CMake, build/ 目录
- **架构文档**: AI/CocosCreator-v3.8.8-Architecture.md

## 关键架构知识
- 入口: predefine.ts → global-exports.ts → cclegacy 命名空间
- 双实现: .ts (Web) vs .jsb.ts (原生JSB), 54个双实现文件
- PAL 平台抽象: pal/ 虚拟模块映射
- 核心3类: Game(主循环) → Director(场景调度) → Root(渲染管理)
- GFX 抽象层: gfx-base 接口 → 各后端实现 (GLES3/Vulkan/Metal/D3D12)

## C++ 迁移 (M1-M10) 已完成
- TypeRegistry, Director, BinarySerialization, ScriptBridge, AssetManager, CameraComponent, MeshRenderer, Light JSB 绑定等
- Debug/Release 全量编译通过
- 详见 AI/design-cpp-master-spec.md v2.0

## JSB 绑定经验
- JSB 注册函数必须在全局命名空间（不在 namespace cc 内）
- native_ptr_to_seval<T> 需要类型 T 有完整定义（不能前向声明）
- se::Value 无 isFunction()/NullObject()，用替代方案
- Vec3 转换用 jsb_conversions_spec.h 中的标准函数

## D3D12 GFX 后端 PoC (2026-04-25 启动)
- **目录**: native/cocos/renderer/gfx-d3d12/
- **架构**: pImpl 模式隔离 D3D12/Win32 头文件
- **16 个 D3D12 GFX 类全部真实实现** (Device/Swapchain/Buffer/Texture/DescriptorSet*/PipelineLayout/Shader/RenderPass/Framebuffer/PipelineState/InputAssembler/CommandBuffer/Queue/QueryPool)
- **Shader 编译管线**: GLSL(#version 450) → glslang → SPIR-V → SPIRV-Cross → HLSL(SM 5.1) → D3DCompile → DXBC
- **端到端渲染已打通**: 三角形→真实着色器→PSO→draw call→像素输出
- **外部测试项目**: D:\Work\CocosProjects\WebGPUDemo\

### D3D12 关键修复历史
- **Swapchain 纹理修复**: 动态返回 getCurrentBackBufferHandle()
- **DEVICE_HUNG 修复**: copyBuffersToTexture upload 资源生命周期 bug（ComPtr 作用域）
- **PSO 黑屏修复**: InputLayout 与 fallback shader 不匹配 → 清除 InputLayout 重试
- **SPIRV-Cross HLSL 绑定**: hlslBinding.stage 映射、register_space=set、入口点统一 "main"
- **黑屏根因**: beginRenderPass 无条件 ClearRenderTarget 忽略 loadOp → 修复后 loadOp=LOAD 不再 clear
- **CommandBuffer 延迟描述符绑定**: bindDescriptorSet 只记录 pending, draw 前统一 flush
- **PipelineLayout visibility**: 使用 D3D12_SHADER_VISIBILITY_ALL
- **Null 描述符修复**: forceUpdate() 中 null 绑定不再跳过，改为写入 dummy 资源描述符
  - CBV: null CBV (CreateConstantBufferView(nullptr, handle))
  - SRV/UAV: dummy 1x1 RGBA8 纹理或 256B buffer（不支持 null SRV/UAV 描述符的硬件会 DEVICE_REMOVED）
  - Sampler: default point-clamp sampler
  - D3D12Device 新增 getDummyTexture()/getDummyBuffer()

### D3D12 当前问题 (2026-05-01)
- **EID 410 "No Resource"**: 已修复 null 描述符写入机制
  - 根因: forceUpdate() 中 null buffer → CBV 跳过写入 → 堆槽位零值
  - 修复: null 绑定写入 null CBV / dummy SRV-UAV / default sampler
  - 待验证: 重新抓 RenderDoc capture 确认 CBV 绑定正常
- **RenderDoc capture**: C:\temp\d3d12_capture_capture.rdc

### D3D12 代码审查 (14/17 完成)
- copyTexture/blitTexture/resolveTexture 完整实现
- getRTVHandle 按 texture 独立检查
- pipelineBarrier 增加 inRenderPass 标志

### D3D12 性能审查 (2026-05-03)
- **报告**: AI/d3d12-performance-review.md
- **Critical**: 同步submit(C1)、每帧描述符堆reset(C2)、forceUpdate无dirty追踪(C3)、文件I/O诊断泄漏(C4)
- **Important**: Buffer全用UPLOAD堆(I1)、无帧流水线(I2)、每次纹理上传新建UploadBuffer(I3)、flushDescriptorSets堆分配(I4)、DescriptorSet各自CPU堆(I5)、全局UAV屏障(I6)
- **最高优先级修复**: 帧流水线+异步submit → 预估30-50% FPS提升
- **C4 已修复 (2026-05-03)**: 移除 dsDiagLog 文件I/O、D3D12Buffer 诊断计数、Queue 诊断改为 Debug-only
- **I4+M2 已修复 (2026-05-04)**: 所有渲染热路径上的 ccstd::vector 替换为固定大小栈数组（flushDescriptorSets/bindInputAssembler/beginRenderPass/endRenderPass/blitTexture/copyTexture/resolveTexture），编译通过

### D3D12 Shadow 修复 (2026-05-23)
- **根因**: PipelineUBO::updateMultiCameraUBO resize camera buffer 后只更新 globalDSMgr，漏绑 pipeline descriptor set
  - 多相机(3个)触发 _cameraBuffer resize(2304B)，旧 _cameraBufferView 被 delete
  - pipeline descriptor set 中 UBOCamera binding 变成悬垂指针
  - forceUpdate() 用悬垂指针创建 CBV → 指向已销毁资源 → shader 读到零值 → shadowEnable=0
- **修复**: resize 分支中增加 `_pipeline->getDescriptorSet()->bindBuffer(UBOCamera::BINDING, _cameraBufferView)`
- **教训**: buffer resize 后必须同步所有 descriptor set 的 binding，不能只更新一个

### D3D12 半透明渲染修复 (2026-05-03)
- **H1 已修复**: bindPipelineState 中添加 OMSetBlendFactor，传递 BlendState.blendColor
- **M1 已修复**: SampleDesc.Count 从 RenderPass.getSampleCount() 获取（不再硬编码 1）
- **附带效果**: Alpha-to-Coverage 现在可随 MSAA 启用生效
- 分析报告: AI/d3d12-alpha-blend-analysis.md
- **已知残留限制**: CONSTANT_ALPHA 映射不精确（D3D12 API 限制，不影响内置 Effect）
