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
- Node 最核心类 (~3014行), 继承 CCObject
- GFX 抽象层: gfx-base 接口 → 各后端实现 (GLES3/Vulkan/Metal/D3D12)
- 技术栈: TypeScript 4.9.5, @cocos/ccbuild, C++17

## C++ 迁移 (M1-M10) 状态汇总
- **权威设计文档**: AI/design-cpp-master-spec.md v2.0
- **核心策略**: 双轨制 — 编辑器JS_ONLY + 运行时NATIVE_FAST
- **线程模型**: 单线程主循环，C++对象无需锁
- **引用计数**: IntrusivePtr + ReleaseManager, C++ 为权威源
- **统一注册**: TypeRegistry 合并组件注册+序列化注册
- **已完成模块**:
  - M1: TypeRegistry, PipelineStateManager, 基准测试框架
  - M2: 无 (合并到其他阶段)
  - M3: Director 最小实现, NodeActivator, Prefab C++ 类
  - M4: 二进制序列化 (BinarySceneFormat + BinaryDeserializer)
  - M5: Director 完整实现 (tick/loadScene/runScene/Scheduler/ComponentScheduler)
  - M6: ScriptBridge 核心实现
  - M7: NativePipeline 接口定义
  - M8: AssetManager + ReleaseManager
  - M9: CameraComponent C++ (编译通过, JSB待启用)
  - M10: JSB 注册整合 (jsb_module_register.cpp)
- **已知问题**: Engine::tick 与 Director::tick 重复调用 update
- **全量编译**: Debug/Release 均通过
- **Phase A4 ✅**: Director::tick 职责明确文档化（ComponentScheduler + deferredDestroy）
- **Phase C1 ✅**: ScriptComponent-ScripBridge 双向 ID 同步（_compId + auto-unregister）
- **Phase C2 ✅**: ScriptBridge 批量调用已有 callJSBatchMethod + fallback
- **Phase C3 ✅**: collectAssetRefs + collectAssetRefsBatch 实现（JS 回调 + getPrivateData）
- **Phase D1 ✅**: ReleaseManager 为统一引用计数入口
- **Phase D2 ✅**: destroyOldScene walk收集资产引用 → decRef → autoRelease
- **Phase D3 ✅**: cacheAsset=registerAsset+addRef, removeCachedAsset=decRef, clearCache=批量decRef

## Phase E: Builtin Component JSB 绑定
- **Phase E1 ✅**: CameraComponent JSB 手动绑定 (projection/near/far/fov + setViewport + getRenderCamera)
- **Phase E2 ✅**: MeshRendererComponent C++ 类 (onLoad/onDestroy/update + mesh/material/shadow)
- **Phase E3 ✅**: LightComponent C++ 类 (5子类: Directional/Sphere/Spot/Point/RangedDirectional)
- **Phase E4 ✅**: MeshRendererComponent JSB 手动绑定 (shadowCastingMode/receiveShadow + setMesh/getMaterial + getRenderModel)
- **Phase E5 ✅**: LightComponent JSB 手动绑定 (5个灯光类各自的属性: color/illuminance/range/size/luminance/spotAngle/shadow*)
- **Phase E6 ✅**: 所有 JSB 注册整合到 jsb_module_register.cpp
- **Phase E7 ✅**: Debug + Release 全量编译通过，零错误
- **新增文件**: jsb_camera_component_manual.h/.cpp, jsb_mesh_renderer_manual.h/.cpp, jsb_light_component_manual.h/.cpp
- **BuiltinTypeIds**: 新增 BUILTIN_DIRECTIONAL_LIGHT(19) ~ BUILTIN_RANGED_DIRECTIONAL_LIGHT(23), COUNT=24
- **待后续**: MeshRendererComponent.syncToRenderModel() 完整 submodel 设置

## JSB 绑定经验
- SWIG 自动生成: scene.i 配置, `%ignore` 排除方法
- 手动绑定: `jsb` 命名空间, `se::Class::create()`, attachObject 防 GC
- se::Value 无 `isFunction()`, 用 `fn.isObject() && fn.toObject()->isFunction()`
- se::Value 无 `NullObject()`, 用 `se::Value::Null` 静态成员
- HandleObject 无 `isValid()`, 用 `!isEmpty()`
- sevalue_to_native 提取 C++ 指针
- **关键教训**: JSB 注册函数必须声明在全局命名空间（不能放在 namespace cc），因为 jsb_module_register.cpp 不在任何命名空间内
- **关键教训**: native_ptr_to_seval<T> 需要类型 T 有完整定义（不能是前向声明），因为它内部调用 typeid
- **关键教训**: 提取 JS 对象中的原生指针时，若 T 未注册 is_jsb_object，直接用 `static_cast<T*>(args[0].toObject()->getPrivateData())` 比 sevalue_to_native 更可靠
- **Vec3 转换**: 使用 `jsb_conversions_spec.h` 中的 `Vec3_to_seval` 和 `sevalue_to_native(Vec3*,...)`, 不要自定义

## D3D12 GFX 后端 PoC (2026-04-25 启动)
- **目录**: native/cocos/renderer/gfx-d3d12/
- **架构**: pImpl 模式隔离 D3D12/Win32 头文件
- **已完成 (REAL)**: Device (609行), Swapchain (296行), Buffer (200行), Texture (242行)
- **Agent A 已完成 (2026-04-26)**: DescriptorSetLayout, DescriptorSet (CPU staging heap), PipelineLayout (RootSignature), DescriptorHeapPool (工具类)
- **Agent B 已完成 (2026-04-26)**: Shader (字节码存储), RenderPass (格式映射), Framebuffer (RTV/DSV), PipelineState (PSO)
- **Agent C 已完成 (2026-04-26)**: InputAssembler (顶点布局), CommandBuffer (核心命令记录), Queue (命令提交), Device重构 (DescriptorHeapPool集成)
- **Device 重构**: 2个 GPU-visible DescriptorHeapPool (CBV_SRV_UAV 4096 + SAMPLER 2048), 帧循环自动 reset
- **QueryPool 已实现 (2026-04-26)**: CreateQueryHeap + readback buffer + ResolveQueryData + begin/end/fetchResults
- **全部真实实现，无 stub**: 16 个 D3D12 GFX 类全部真实代码
- **端到端渲染管线打通**: PSO自动创建空RootSignature + 运行时D3DCompile内置三角形着色器
- **总产出**: +2800 行 D3D12 代码, Debug/Release 编译通过
- **Gate 2 ✅ 通过 (2026-04-26)**: WebGPUDemo 15秒稳定运行，零 D3D12 错误
- **Swapchain 纹理修复 (2026-04-26)**:
  - D3D12Texture: swapchain颜色纹理动态返回getCurrentBackBufferHandle(), 深度纹理创建真实资源
  - D3D12Framebuffer: 检测swapchain纹理, getRTVHandle()动态返回swapchain RTV
  - D3D12CommandBuffer: beginRenderPass/endRenderPass添加PRESENT↔RENDER_TARGET资源屏障
  - 修复后进程稳定运行85秒+，之前5秒即退出(exit code 2173)
- **DEVICE_HUNG 修复**: copyBuffersToTexture upload 资源生命周期 bug（ComPtr 作用域问题），用 vector<ComPtr> 保持到 waitForGpu 后释放
- **Debug Layer**: Release 构建禁用(#if !defined(NDEBUG))，避免 AMD 驱动不稳定
- **PSO 黑屏修复 (2026-04-29)**:
  - 首次 PSO 创建因 InputLayout(引擎顶点属性) 与 fallback shader(SV_VertexID) 不匹配 → E_INVALIDARG
  - 第二次 fallback 被 `if (!compiledVS || !compiledPS)` 阻止（首次 fallback 已编译过）
  - 修复: 始终在首次失败时清除 InputLayout 并重试，PSO 创建成功，draw call 正常
- **纯 C++ 路径安全降级 (2026-04-29)**:
  - PipelineSceneData::initDebugRenderer: effect 未加载时 passes 为空，添加空检查
  - DebugRenderer::activate: 内置字体返回 nullptr，getFontPath 返回空串跳过
  - Node::onBatchCreated: 补全 C++ 实现 (invalidateChildren + siblingIndex + 递归)
- **外部测试项目**: D:\Work\CocosProjects\WebGPUDemo\ (COCOS_X_PATH 指向引擎源码)
- **进度文档**: AI/D3D12-Subagent-Progress-Log.md, AI/D3D12-GFX-PoC-Checklist.md
