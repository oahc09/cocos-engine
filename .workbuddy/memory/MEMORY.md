# Cocos Creator Engine 项目记忆

## 项目基本信息
- **引擎版本**: Cocos Creator v3.8.8 (MIT 许可证)
- **分支**: v3.8.8_custome
- **工作目录**: d:\cocos_custome\cocos-engine
- **架构文档**: AI/CocosCreator-v3.8.8-Architecture.md

## 关键架构知识
- 入口: predefine.ts → cocos/core/global-exports.ts → cclegacy 命名空间
- 构建核心配置: cc.config.json (852行, features/modules/constants/moduleOverrides)
- 双实现机制: .ts (Web) vs .jsb.ts (原生JSB)
- PAL 平台抽象: pal/ 目录, 通过虚拟模块映射实现跨平台
- 渲染双管线: custom-pipeline (WebPipeline) vs legacy-pipeline
- 物理策略模式: Cannon/PhysX/Ammo/Builtin 通过 framework 层切换
- 初始化四阶段: Base → Infrastructure → Subsystem → Project
- 核心3类: Game(主循环), Director(场景调度), Root(渲染管理)
- Node 是引擎最核心类 (3014行), 继承自 CCObject

## 技术栈
- TypeScript 4.9.5, ES6 + CommonJS
- @cocos/ccbuild 构建工具
- @cocos/box2d (2D物理), @cocos/cannon (3D物理)

## JSB/Native 分析结论 (2026-04-24)
- 共 54 个 .jsb.ts 双实现文件，覆盖 7 大模块组
- JSB 5 种补丁模式: 直接替换类/Prototype猴子补丁/_ctor构造钩子/属性桥接/Decorator补丁
- `_tempFloatArray` 是核心跨层数据传递通道 (Float32Array共享内存)
- 全局桥接对象: jsb(核心类), nr(渲染管线), n2d(2D渲染), gfx(GFX), render(自定义管线)
- native-binding/decorators.ts (1611行) 是自动生成的元数据补丁文件
- **核心结论**: Windows/Android 无法完全脱离 TS，因组件系统/用户脚本/序列化依赖 JS 引擎
- 可优化方向: 减少 JS↔C++ 桥接次数、消除 _tempFloatArray、热路径 Native 化
- 详细对比文档: AI/TS-vs-Native-Comparison.md

## C++ 化设计结论 (2026-04-25, v2.0 统一修订)
- **核心策略**: 双轨制 — 编辑器JS_ONLY + 运行时NATIVE_FAST
- **用户脚本不可C++化**: 只优化桥接/调度，不改变JS执行本质
- **引用计数统一**: C++ AssetRefManager 为权威源，JS通过桥接同步
- **双层调度**: 内置组件 ThreeBucketArray + 用户脚本 ScriptBridge 批量调用
- **二进制序列化**: 构建时 .scene JSON → .scene.bin，C++零反射解析
- **Prefab预编译**: BinaryTemplate + instantiateFromBinary 替代JS JIT编译
- **内置组件C++化优先级**: Camera/MeshRenderer/Light > Animation/Sprite
- **Native已有基础**: Asset/Scene/Node/SceneGlobals/Texture2D/Mesh/Material已有C++实现
- **Native关键缺失**: AssetManager/Pipeline/ReleaseManager/Prefab/序列化 (Director 已有最小实现)
- **统一 TypeRegistry**: 合并组件注册表与序列化注册表为单一注册表 (I-1)
- **统一 ScriptComponent**: 合并生命周期代理与反序列化占位 (I-3)
- **常驻节点归 Director**: Scene 不持有 _persistRootNodes (I-4)
- **统一时间线**: 26周6阶段 (Phase 0-5) (I-5)
- **线程模型**: 单线程主循环，C++对象无需锁 (G-5)
- **GC协议**: ScriptComponent._jsObject 弱引用，safeCallJS 检查 isDead() (G-8)
- **回滚策略**: 每个C++化功能都有JS降级路径 (G-14)
- **统一注册宏**: CC_REGISTER_BUILTIN 替代各文档独立宏
- **生命周期检测**: hasUpdateMethod/hasStartMethod 虚函数替代函数指针
- **权威文档**: AI/design-cpp-master-spec.md v2.0 (948行)
- **子设计文档**: AI/design-cpp-{component,scene,asset,script}-system.md v2.0
- **分析文档**: AI/analysis-*.md (4份深度分析)
- 总体时间线: ~6个月 (Phase 0-5, 26周)

## JSB 绑定生成机制 (2026-04-25 实施)
- **SWIG 自动生成**: CMake configure 阶段调用自定义 SWIG，从 `.i` 文件生成 `jsb_*_auto.cpp/h`
- **核心配置**: `native/tools/swig-config/scene.i`，通过 `%ignore` 排除方法，`%attribute` 定义属性
- **排除方法**: 在 `%include` 之前添加 `%ignore cc::Node::xxx;` 即可阻止 SWIG 绑定
- **se::Value API**: 无 `isFunction()`，需用 `fn.isObject() && fn.toObject()->isFunction()` 组合判断
- **se::Object::call()**: 签名 `bool call(const ValueArray &args, Object *thisObject, Value *rval = nullptr)`
- **CMake 生成函数**: `cc_gen_swig_files()` 在 `native/cmake/predefine.cmake` 第269行
- **Node 组件方法**: 需在 scene.i 中 %ignore，因为 Component 类型未注册到 JSB 绑定系统

## 手动 JSB 绑定模式 (2026-04-25 M1-S2)
- **PipelineStateManager 模式**: 在 `jsb` 命名空间下创建 `se::Class::create()` 对象，绑定静态方法+实例方法
- **C++ 回调→JS**: lambda 捕获 `se::Value jsFunc`，`jsThis.toObject()->attachObject(jsFunc.toObject())` 防 GC
- **Asset* 参数**: 使用 `sevalue_to_native(args[i], &asset, s.thisObject())` 从 JS Asset 对象提取 C++ 指针
- **注册流程**: `jsb_module_register.cpp` 中 `se->addRegisterCallback(register_all_XXX)` 添加到注册链
- **TS 声明**: 在 `@types/jsb.d.ts` 的 `declare namespace jsb { }` 内添加 class 声明

## 基准测试框架 (2026-04-25 M1-S3)
- **测试目录**: `native/tests/benchmarks/`
- **bench_type_registry**: 链接 cocos_engine，测试 registerType/getTypeInfo/hasClass/getClassIdByName/registerScriptType/create
- **bench_ref_manager**: 自包含 mock，隔离测试管理器层开销（不链接引擎）
- **门控**: getTypeInfo 查询 < 1μs/op (实测 ~115ns/op)
- **结果归档**: `native/tests/benchmarks/bench-results/native-fast.json`
- **CI 回归阈值**: 10% (G-15)

## Director 最小实现 (2026-04-25 M3-S1b)
- **Director.h/cpp**: 单例，持有 NodeActivator，提供 `getNodeActivator()` 访问
- **NodeActivator 增强**: 递归激活/停用子节点 + 设置 `_activeInHierarchy` + emit `ActiveInHierarchyChanged`
- **递归激活**: activateNodeRecursively — 设置 activeInHierarchy → 激活组件 → 递归子节点
- **递归停用**: deactivateNodeRecursively — 设置 Deactivating 标记 → 清 activeInHierarchy → 先递归子节点 → 逆序停用组件 → 清标记
- **Scene::activate()**: 接入 `Director::getInstance()->getNodeActivator()->activateNode(this, active)`
- **Node::setActive/onHierarchyChangedBase**: 替换 `emit<ActiveNode>` 为 `Director::getInstance()->getNodeActivator()->activateNode()`
- **Node::addComponent**: 对 active 节点立即调用 `activateComponent(comp, true)`
- **NodeActivator::activateComponent**: 公共接口，委托给私有 activateComp
- **C++ emit 事件类型**: 外部调用 Node emit 时需完整限定名 `node->emit<Node::ActiveInHierarchyChanged>()`

## 第二批并行任务 (2026-04-25 M3-S2/M4/M5-S1/M6-S1/M7-S0)

### M3-S2: Prefab C++ 类
- **新建**: Prefab.h/cpp, PrefabInfo.h, PrefabUtils.h/cpp
- **Prefab**: 继承 Asset，OptimizationPolicy(AUTO/SINGLE/MULTI)，BinaryTemplate 结构，initFromBinary/instantiate
- **PrefabUtils**: expandNestedPrefabInstanceNode/applyTargetOverrides 当前 stub (等 M4 完整实现)
- **Scene.cpp**: 替换注释为 `PrefabUtils::expandNestedPrefabInstanceNode(this)` / `PrefabUtils::applyTargetOverrides(this)`

### M4-S0+S1: 二进制序列化
- **新建**: BinarySceneFormat.h (格式规范: Header/StringTable/InstanceTable/NodeEntry/ComponentEntry/AssetRefEntry/Footer)
- **新建**: BinaryDeserializer.h/cpp
- **static_assert**: BinarySceneHeader 32B, BinarySceneFooter 32B
- **BinaryDeserializer::deserialize**: validateHeader → parseStringTable → createScene → createNodeTree → resolveReferences
- **IntrusivePtr<Asset> 析构问题**: 头文件中使用 IntrusivePtr<Asset> 时必须 include Asset.h，前向声明不够

### M5-S1: Director 完整实现
- **扩展 Director.h/cpp**: tick/loadScene/runScene/runSceneImmediate/addPersistRootNode/removePersistRootNode/isPersistRootNode
- **tick**: Scheduler::update → ComponentScheduler::invokeStart/Update/LateUpdate
- **runSceneImmediate**: onBeforeLoadScene → scene->load → handlePersistRootNodes → destroyOldScene → setScene → scene->activate → onLaunched
- **常驻节点**: _persistRootNodes (uuid → Node*)，addPersistRootNode 设置 DONT_DESTROY 标志
- **Scheduler**: 懒初始化 getScheduler()，类在 base/Scheduler.h

### M6-S1: ScriptBridge 核心实现
- **新建**: scripting/ScriptBridge.h/cpp
- **接口**: invokeStartBatch/UpdateBatch/LateUpdateBatch (批量), invokeOnDestroy/OnEnable/OnDisable/OnLoad (单个)
- **注册**: registerScriptClass (调用 TypeRegistry), registerScriptInstance/unregisterScriptInstance (compId → ScriptInstanceInfo)
- **ScriptInstanceInfo**: se::Object* jsObject, ScriptComponent* scriptComp, typeId, className
- **collectAssetRefs/isInstanceOf**: 与 JS 侧交互

### M7-S0: NativePipeline 接口定义
- **新建**: assets/NativePipeline.h (纯头文件，无 cpp)
- **PipelineTask**: taskId/path/uuid/data/output/isComplete/hasError
- **PipelineHandler**: 虚函数 handle(PipelineTask&)
- **NativePipeline**: insert/removeHandler/executeSync/executeAsync/Mode(JS_ONLY/NATIVE_FAST)
- **门控**: 编译通过

## Prefab C++ 实现 (2026-04-25 M3-S2)
- **Prefab.h/.cpp**: 继承 Asset，含 OptimizationPolicy 枚举(AUTO/SINGLE/MULTI)、BinaryTemplate 结构体
- **Prefab::initFromBinary()**: stub，存储原始二进制数据，TODO(M4) 等待 BinaryDeserializer
- **Prefab::instantiate()**: 根据 OptimizationPolicy 选择路径，有二进制模板走 instantiateFromBinary()
- **Prefab::instantiateFromBinary()**: stub，返回 nullptr，TODO(M4)
- **PrefabInfo.h**: 结构体 (asset/fileId/infoId/root/isDeleted)
- **PrefabUtils.h/.cpp**: 静态工具类，expandNestedPrefabInstanceNode/applyTargetOverrides stub 实现
- **Scene::load() 修改**: 注释替换为 `PrefabUtils::expandNestedPrefabInstanceNode(this)` / `PrefabUtils::applyTargetOverrides(this)`
- **CMakeLists.txt**: 添加 Prefab.cpp/h, PrefabInfo.h, PrefabUtils.cpp/h 到 cocos_source_files

## 第四批并行任务 (2026-04-25 M8-S1/S2)

### M8-S1: AssetManager C++ 核心实现
- **新建**: `native/cocos/core/assets/AssetManager.h/cpp`
- **功能**: 单例模式，支持同步/异步加载、Bundle 管理、缓存管理、预加载、双模式检测
- **loadSync 查找顺序**: 缓存 → Bundle → nullptr (远程加载未实现)
- **异步加载**: 当前同步执行后回调，TODO 未来接入线程池
- **Bundle 集成**: 通过 NativeBundle::get() / getUuidByPath() 查找资源
- **CMakeLists.txt**: 已添加 AssetManager.cpp/h

### M8-S2: ReleaseManager C++ 核心实现
- **新建**: `native/cocos/core/assets/ReleaseManager.h/cpp`
- **功能**: 资源引用计数管理、自动释放、依赖管理
- **核心机制**: registerAsset → addRef/decRef → autoRelease → releaseAsset (递归 decRef dependencies)
- **Asset 生命周期**: 由 IntrusivePtr 管理，ReleaseManager 只维护引用计数，不直接 delete
- **风险**: 依赖图存在环时可能导致同一资源被多次 decRef，需后续增加环检测
- **CMakeLists.txt**: 已添加 ReleaseManager.cpp/h
