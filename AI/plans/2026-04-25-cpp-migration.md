# Cocos Creator v3.8.8 C++ 化实施计划（仓库现状修订版）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不破坏现有 JS_ONLY 编辑器工作流的前提下，基于当前仓库已有 C++ 迁移骨架，补全 NATIVE_FAST 发布路径，使场景构建、组件生命周期调度、脚本桥接、资产引用与二进制反序列化形成稳定闭环，并最终满足原计划的性能与兼容性目标。

**Architecture:** 当前仓库并非从零开始，`M1/M2/M3/M4/M5/M6/M7` 已存在大量骨架代码。本计划不再按"新建文件清单"推进，而是以"补全现有模块、统一命名与入口、消除计划-仓库漂移"为主线，优先打通最小运行闭环，再逐步扩展到二进制场景、资产系统、内置组件与桥接优化。

**Tech Stack:** C++17, TypeScript 4.9.5, JSB (`se::Object`), CMake + Ninja, GoogleTest, ccstd 容器库

---

## 0. 修订原则

### 0.1 当前仓库事实

以下模块/文件已经存在，原始计划中"Create"表述已失效：

- `native/cocos/core/components/*`
  - `Component.h/.cpp`
  - `ScriptComponent.h/.cpp`
  - `ThreeBucketArray.h`
  - `ComponentScheduler.h/.cpp`
  - `NodeActivator.h/.cpp`
  - `BuiltinTypeIds.h`
  - `ComponentMacros.h`
- `native/cocos/core/serialization/*`
  - `TypeRegistry.h/.cpp`
  - `BinaryDeserializer.h/.cpp`
  - `BinarySceneFormat.h`
- `native/cocos/core/scene-graph/*`
  - `Node.h/.cpp`
  - `Scene.h/.cpp`
  - `Prefab.h/.cpp`
  - `PrefabInfo.h`
  - `PrefabUtils.h/.cpp`
- `native/cocos/core/*`
  - `Director.h/.cpp`
- `native/cocos/core/scripting/*`
  - `ScriptBridge.h/.cpp`
- `native/cocos/core/assets/*`
  - `AssetRefManager.h/.cpp`
  - `NativePipeline.h/.cpp`
  - `NativeBundle.h/.cpp`
  - `NativeDependUtil.h/.cpp`
  - `AssetManager.h/.cpp`
  - 旧有 `ReleaseManager.h`
- JSB 绑定已存在：
  - `native/cocos/bindings/manual/jsb_AssetRefManager.*`
  - `native/cocos/bindings/manual/jsb_binary_deser_manual.*`
  - `native/cocos/bindings/manual/jsb_script_bridge_manual.*`
  - `native/cocos/bindings/manual/jsb_camera_component_manual.*` (Phase E1)
  - `native/cocos/bindings/manual/jsb_mesh_renderer_manual.*` (Phase E4)
  - `native/cocos/bindings/manual/jsb_light_component_manual.*` (Phase E5)
- 内置组件 C++ 类（Phase E2/E3）：
  - `native/cocos/core/components/CameraComponent.h/.cpp` (M9)
  - `native/cocos/core/components/MeshRendererComponent.h/.cpp`
  - `native/cocos/core/components/LightComponent.h/.cpp` (含 5 子类)

### 0.2 命名统一

后续一律以当前仓库命名为准：

- 使用 `native/cocos/core/components/*`，不再使用旧计划中的 `core/component/*`
- 使用现有 `register_all_*` 风格 JSB 注册接口，不再引入第二套 `register_*` 风格
- 资产加载入口以现有 `AssetManager + NativePipeline + NativeBundle + NativeDependUtil` 为主，不再平行引入第二套 `NativeAssetManager`
- 资产释放优先扩展现有 `ReleaseManager` 或在其基础上演进，不直接硬插一套独立 `NativeReleaseManager`，除非现有接口无法承载

### 0.3 执行策略

先修通最小闭环，再扩展性能路径：

1. 生命周期闭环
2. 场景/反序列化闭环
3. 脚本桥接闭环
4. 资产引用/释放闭环
5. 内置组件迁移
6. 桥接与性能优化
7. 全量验证

---

## 1. 与原计划相比的关键纠正

### 1.1 组件系统目录纠正

原计划中的以下路径作废：

- `native/cocos/core/component/*`

统一替换为：

- `native/cocos/core/components/*`

### 1.2 资产管理方案纠正

原计划中的以下项不再作为首选实现路径：

- `NativeAssetManager`
- `NativeReleaseManager`

改为：

- 扩展 `native/cocos/core/assets/AssetManager.h/.cpp`
- 复用 `NativePipeline`, `NativeBundle`, `NativeDependUtil`
- 在需要时将 `ReleaseManager` 改造成 native 权威释放入口，或引入兼容性包装层，但不制造双入口

### 1.3 JSB 命名纠正

原计划中的以下绑定命名作废：

- `jsb_asset_ref_manual.*`
- `register_asset_ref_manager(...)`

统一替换为当前仓库约定：

- `jsb_AssetRefManager.*`
- `register_all_AssetRefManager(...)`

同理，`BinaryDeserializer` 与 `ScriptBridge` 均沿用现有 `register_all_*` 风格。

### 1.4 调度模型纠正

原计划中的"双层调度器"目标保留，但必须在现有 `ComponentScheduler` 上演进，不可新增第二个并行调度器实现。当前 `ComponentScheduler` 只有三桶 `Component*` 调度，后续扩展时必须：

- 保持现有 `invokeStart/invokeUpdate/invokeLateUpdate` 接口可用
- 在内部补充 builtin/script 双层调度结构，或在不破坏外部接口的情况下折叠实现
- 不引入第三套与 TS `ComponentScheduler` 语义不兼容的生命周期顺序

### 1.5 主循环入口纠正

原计划中 `Director.tick()` 直接接管全部调度流程的目标保留，但当前仓库中 `Scheduler::update(dt)` 仍由 `Engine::tick()` 先调用。推进时必须先统一权威入口，避免双重调度。

---

## 2. 当前完成度评估

| 模块 | 状态 | 结论 |
|------|------|------|
| M1 TypeRegistry | ✅ 已完成 | 可继续补强，非从零开发 |
| M2 Component | ✅ 已完成 | 合并到其他阶段，生命周期闭环已通 |
| M3 SceneGraph | ✅ 已完成 | `Node/Scene/Prefab` 已存在，组件管理能力已补全 |
| M4 Serialization | ✅ 已完成 | `BinaryDeserializer` 可解析骨架，引用解析已完成 |
| M5 Director | ✅ 已完成 | `tick/runSceneImmediate/persist root/loadScene` 完整实现 |
| M6 ScriptSystem | ✅ 已完成 | `ScriptBridge` 核心+批量调用+资产收集均已闭环 |
| M7 Asset | ✅ 已完成 | `AssetManager` + `ReleaseManager` 统一引用计数，场景集成完成 |
| M8 BuiltinComp (Camera) | ✅ 已完成 | CameraComponent C++ 类 + JSB 手动绑定 |
| M9 BuiltinComp (Mesh/Light) | ✅ 已完成 | MeshRendererComponent + LightComponent(5子类) C++ + JSB |
| M10 JSB整合 | ✅ 已完成 | 所有模块 JSB 注册整合到 jsb_module_register.cpp |

---

## 3. 实施阶段（修订后）

## Phase A: 生命周期最小闭环

**目标:** 让 `NodeActivator -> ComponentScheduler -> Director.tick` 形成真实闭环，使节点激活、组件启停、`start/update/lateUpdate` 的注册与注销行为稳定可测。

**Files:**
- Modify: `native/cocos/core/components/Component.cpp`
- Modify: `native/cocos/core/components/ComponentScheduler.h`
- Modify: `native/cocos/core/components/ComponentScheduler.cpp`
- Modify: `native/cocos/core/components/NodeActivator.cpp`
- Modify: `native/cocos/core/scene-graph/Node.cpp`
- Modify: `native/cocos/core/Director.cpp`
- Test: `native/tests/unit-test/src/component_test.cpp`
- Test: `native/tests/unit-test/src/node_test.cpp`

### A1. Component 启停接线
- [x] `Component::setEnabled()` 在节点已激活时触发 `onEnable/onDisable` 与调度器注册/注销
- [x] `Component::destroy()` 从所属 `Node` 卸载自身
- [x] `Component::schedule/unschedule()` 接到 `Director::getScheduler()`，当前实现基于传入 `std::function` 对象地址生成 scheduler key，仅保证同一 callback 实例的重用/取消语义

### A2. NodeActivator 接管调度注册
- [x] `activateComp(comp, true)` 在 `_enabled == true` 时执行：`__preload -> onLoad -> onEnable -> registerComponent`
- [x] `activateComp(comp, false)` 在 `_enabledInHierarchy == true` 时执行：`unregisterComponent -> onDisable`
- [x] 避免重复注册与重复注销

### A3. Node 组件管理补全最小功能
- [x] `Node::addComponent(Component*)` 防止空指针/重复添加
- [x] 若节点已激活，新增组件立即激活并调度注册
- [x] `Node::removeComponent(Component*)` 在移除前若组件仍在 hierarchy 中，先停用并注销，再清除 `_node`

### A4. Director 主循环保持单一职责
- [x] 保持当前 `Scheduler::update(dt)` 仍由 `Engine::tick()` 驱动
- [x] `Director::tick()` 只负责组件生命周期阶段与延迟销毁
- [x] 文档中显式标注此处与原计划差异，后续若要收口到 `Director`，需单独阶段处理

### A5. 单元测试
- [x] 新增/扩展测试覆盖以下行为：
  - 节点激活后，组件触发 `onLoad/onEnable`
  - 组件禁用后，不再收到 `update/lateUpdate`
  - 组件重新启用后，重新进入调度
  - `start()` 仅触发一次
  - 组件销毁后从节点移除

**门控:**
- `component_test` 通过
- `node_test` 通过
- 生命周期顺序与 TS 语义一致：`__preload -> onLoad -> onEnable -> start -> update -> lateUpdate`

---

## Phase B: 场景与反序列化闭环

**目标:** 让 `Scene.load/activate`, `Prefab`, `BinaryDeserializer` 形成可运行但未完全优化的 native 场景构建链路。

**Files:**
- Modify: `native/cocos/core/serialization/BinaryDeserializer.h`
- Modify: `native/cocos/core/serialization/BinaryDeserializer.cpp`
- Modify: `native/cocos/core/scene-graph/Prefab.cpp`
- Modify: `native/cocos/core/scene-graph/Scene.cpp`
- Modify: `native/cocos/core/Director.cpp`
- Test: `native/tests/unit-test/src/node_test.cpp`
- Create or Modify: `native/tests/unit-test/src/binary_deserializer_test.cpp`

### B1. BinaryDeserializer 补完最小正确性
- [x] `createScene()` 读取最小 scene 基本属性（当前支持 `scene name` 与 `autoReleaseAssets`）
- [x] `resolveReferences()` 支持最小资产引用解析（当前支持 footer/reference-table 校验与缓存资产收集到结果集，不做通用属性回填）
- [x] Header/Version 校验失败时返回明确错误

### B2. Prefab 二进制实例化接线
- [x] `Prefab::instantiateFromBinary()` 通过 `BinaryDeserializer` 或其共享内部能力创建节点树
- [x] 当前不做高级优化策略，只保证正确性优先

### B3. Director.loadScene 打通
- [x] 基于现有 `AssetManager` / `SceneAsset` 管线把 `loadScene()` 从 stub 提升为真实加载入口
- [x] 失败时返回清晰错误，不 silent fallback

**门控:**
- 二进制场景可构造基础节点树
- `runSceneImmediate()` 能接入反序列化结果
- Prefab 二进制实例化不再返回 stub/null

---

## Phase C: 脚本桥接闭环

**目标:** 让 `ScriptComponent + ScriptBridge + JSB` 具备稳定的脚本实例注册、批量生命周期调用与基本类型语义。

**Files:**
- Modify: `native/cocos/core/components/ScriptComponent.h`
- Modify: `native/cocos/core/components/ScriptComponent.cpp`
- Modify: `native/cocos/core/scripting/ScriptBridge.h`
- Modify: `native/cocos/core/scripting/ScriptBridge.cpp`
- Modify: `native/cocos/bindings/manual/jsb_script_bridge_manual.cpp`
- Create: `cocos/core/scripting/batch-executor.ts`
- Modify: `cocos/native-binding/impl.ts`
- Modify: `cocos/scene-graph/node.jsb.ts`
- Modify: `@types/jsb.d.ts`
- Test: `tests/core/scripting/batch-executor.test.ts`

### C1. ScriptComponent 与 ScriptBridge ID 同步
- [x] `registerScriptInstance()` 返回的实例 ID 与 `ScriptComponent` 生命周期回调保持一致
- [x] `ScriptComponent` 销毁时自动走 `unregisterScriptInstance()`
- [x] JSB 侧支持显式 `compId` 注册形态：`registerScriptInstance(jsComp, compId, className, scriptComp?)`
- [x] `node.jsb.ts` 中用户脚本组件创建后立即注册到 native `ScriptBridge`
- [x] `node.jsb.ts` 在组件预销毁阶段同步执行 native/TS 两侧反注册

### C2. 批量调用落地
- [x] `invokeStartBatch/invokeUpdateBatch/invokeLateUpdateBatch` 真正走 JS 端批量执行器
- [x] 单个脚本异常不影响整个批次（C++ fallback 已覆盖）
- [x] `impl.ts` 在 native bootstrap 阶段安装 `__scriptBridgeBatchCall`
- [x] TS helper 维护 `compId -> jsComp` 注册表，batch 调用不依赖 scene-graph 反查

### C3. 资产引用收集
- [x] `collectAssetRefs()` 从 JS 侧拿到用户脚本序列化属性中的资产依赖
- [x] 批量 `collectAssetRefsBatch()` 接口已实现
- [x] TS helper 基于 `__values__ + class attrs` 遍历序列化属性，支持数组/嵌套对象/循环引用保护
- [x] `@types/jsb.d.ts` 已补齐 `ScriptBridge` 重载与 `Asset` 类型声明，`tsc --noEmit` 可通过

**门控:**
- 脚本组件实例可注册/反注册
- batch executor 生效
- JSB 调用次数显著低于逐个调用基线

**本轮验证:**
- [x] `npx jest tests/core/scripting/batch-executor.test.ts --runInBand`
- [x] `npx jest tests/scene-graph/scene.test.ts tests/scene-graph/layers.test.ts tests/core/scripting/batch-executor.test.ts --runInBand`
- [x] `npx tsc --noEmit --pretty false`

---

## Phase D: 资产引用与释放闭环

**目标:** 以现有 `AssetManager` 体系为中心建立 native 权威的引用计数与释放策略。

**Files:**
- Modify: `native/cocos/core/assets/AssetRefManager.cpp`
- Modify: `native/cocos/core/assets/AssetManager.h`
- Modify: `native/cocos/core/assets/AssetManager.cpp`
- Modify: `native/cocos/core/assets/ReleaseManager.h`
- Create or Modify: `native/cocos/core/assets/ReleaseManager.cpp`
- Test: `native/tests/unit-test/src/asset_ref_manager_test.cpp`

### D1. 明确权威源
- [x] `AssetRefManager` 仍可直接复用 `Asset::_assetRefCount`，但必须成为统一入口
- [x] 禁止未来新增绕过 `AssetRefManager` 的引用计数路径

### D2. 释放策略收口
- [x] 场景切换时按当前场景依赖与 persist root 依赖做释放判定
- [x] 先在现有 `ReleaseManager` 上演进，避免双套释放器

### D3. AssetManager 闭环
- [x] `load/loadSync/preload` 与引用计数语义一致
- [x] async 路径至少保证语义正确，再优化线程池
- [x] `cacheAsset()` 视为 `ReleaseManager` 的一个逻辑引用源
- [x] `removeCachedAsset()/clearCache()` 在 `decRef()` 后立即 `autoRelease()`，避免缓存移除后残留记录

**门控:**
- addRef/decRef 行为稳定
- 场景切换时不会错误释放仍被引用资产
- 无明显引用计数漂移

**本轮验证:**
- [x] `cmake --build build/unit-test-phase-a --config Debug --target CocosTest`
- [x] `CocosTest.exe --gtest_filter=AssetManagerTest.*:AssetRefManagerTest.*:BinaryDeserializerTest.*:PrefabTest.*:DirectorTest.*:ComponentTest.*:NodeTest.removeComponentDeactivatesAndDetachesComponent`

---

## Phase E: 内置组件迁移

**目标:** 只迁移高价值且已有 native 依附能力的内置组件，避免盲目铺开。

**优先级:**
- P0: Camera, MeshRenderer, Light ✅ 全部完成
- P1: SkeletonAnimation, RigidBody, Sprite (待后续)

**原则:**
- 每个组件都必须有 JS fallback
- 每个组件都必须有属性覆盖测试
- 与现有 render/physics/2d native 能力复用，不另造并行管线

### E1. CameraComponent JSB 手动绑定
- [x] 绑定 projection/near/far/fov 属性 (SE_BIND_PROP_GET/SET)
- [x] 绑定 setViewport() 方法
- [x] 绑定 getRenderCamera() 方法
- [x] `register_all_camera_component()` 声明在全局命名空间

**Files:** `jsb_camera_component_manual.h/.cpp`

### E2. MeshRendererComponent C++ 类
- [x] `onLoad()` / `onDestroy()` / `update()` 生命周期
- [x] mesh/material 属性管理
- [x] shadowCastingMode / receiveShadow 属性
- [x] `syncToRenderModel()` 基础框架（完整 submodel 设置待后续）

**Files:** `native/cocos/core/components/MeshRendererComponent.h/.cpp`

### E3. LightComponent C++ 类 (5 子类)
- [x] `LightComponent` 基类 (color/illuminance)
- [x] `DirectionalLightComponent` (size/luminance/shadow)
- [x] `SphereLightComponent` (range)
- [x] `SpotLightComponent` (spotAngle/shadow)
- [x] `PointLightComponent`
- [x] `RangedDirectionalLightComponent`
- [x] BuiltinTypeIds 新增 19~23, COUNT=24
- [x] 首个 DirectionalLight 自动调用 `setMainLight()`

**Files:** `native/cocos/core/components/LightComponent.h/.cpp`

### E4. MeshRendererComponent JSB 手动绑定
- [x] 绑定 shadowCastingMode/receiveShadow 属性
- [x] 绑定 setMesh()/getMaterial() 方法
- [x] 绑定 getRenderModel() 方法
- [x] 使用 `static_cast<T*>(getPrivateData())` 提取 Mesh/Material 指针
- [x] 包含完整头文件 `3d/assets/Mesh.h` 和 `core/assets/Material.h`

**Files:** `jsb_mesh_renderer_manual.h/.cpp`

### E5. LightComponent JSB 手动绑定
- [x] 绑定 5 个灯光子类各自的属性
- [x] 使用 `jsb_conversions_spec.h` 进行 Vec3 转换
- [x] 单一注册函数 `register_all_light_components()`

**Files:** `jsb_light_component_manual.h/.cpp`

### E6. JSB 注册整合
- [x] 所有新增 JSB 注册整合到 `jsb_module_register.cpp`
- [x] 包含 3 个头文件 + 3 个 `addRegisterCallback` 调用

### E7. 编译验证
- [x] Debug 全量编译通过，零错误
- [x] Release 全量编译通过，零错误

**门控:**
- [x] P0 组件 (Camera/MeshRenderer/Light) 全部 C++ 实现 + JSB 绑定
- [x] Debug + Release 编译通过
- [x] 端到端功能验证（Phase G 已完成当前门控）

---

## Phase F: 桥接优化与性能

**目标:** 在功能闭环后做桥接次数与热点路径优化。

**Files:**
- Modify: `cocos/scene-graph/node.jsb.ts`
- Modify: `cocos/scene-graph/utils.jsb.ts`
- Modify: `native/cocos/profiler/Profiler.h`
- Modify: `native/cocos/profiler/Profiler.cpp`
- Modify: `native/cocos/core/scripting/ScriptBridge.cpp`
- Test: `native/tests/unit-test/src/profiler_test.cpp`

### F1. `_tempFloatArray` 消除
- [x] 已梳理 `node.jsb.ts` 的主要写入/读取热点，首批目标锁定为 `setPosition`、`setScale`、`setRotationFromEuler`
- [~] 用直接绑定替换高频路径
  - [x] `setPosition()` 已改为直调 `setPositionInternal(x, y, z, true)`，不再经 `_tempFloatArray + _setPosition()`
  - [x] `setScale()` 已改为直调 `setScaleInternal(x, y, z, true)`，不再经 `_tempFloatArray + _setScale()`
  - [x] `setRotation()` 已改为直调 `setRotationInternal(x, y, z, w, true)`，不再经 `_tempFloatArray + _setRotation()`
  - [x] `setRotationFromEuler()` 已改为回调原生 `setRotationFromEuler(x, y, z)`，不再经 `_tempFloatArray + _setRotationFromEuler()`
  - [x] `setRTS()` 已新增 `setRTSForJS(rot?, pos?, scale?)` 手动绑定，优先直调 `setRTSInternal(..., true)`，旧 `_setRTS()` 保留为 fallback
  - [x] `rotate()` 已新增 `rotateForJS2(rot, ns?)` 手动绑定，优先直调 native 参数通道，旧 `_rotateForJS()` 保留为 fallback
  - [x] 新增 `resolveNodeVec3Args()` 统一 `Vec3 / (x, y) / (x, y, z)` 参数归一化
  - [x] 新增 `resolveNodeRTSArgs()` 统一 `Quat | Euler Vec3` 到 `Quaternion + optional pos + optional scale` 的归一化
  - [x] 新增 `resolveNodeQuatArgs()` 统一 `Quat / (x, y, z, w?)` 参数归一化
- [x] 按热点顺序替换并验证（保留低频 fallback，后续按需继续收敛）

**本轮验证:**
- [x] `npx jest tests/scene-graph/utils.jsb.test.ts --runInBand`
- [x] `npx jest tests/core/node.test.ts --runInBand`
- [x] `npx jest tests/scene-graph/scene.test.ts tests/scene-graph/layers.test.ts tests/scene-graph/utils.jsb.test.ts tests/core/scripting/batch-executor.test.ts --runInBand`
- [x] `npx tsc --noEmit --pretty false`
- [x] `cmake --build build/unit-test-phase-a --config Debug --target CocosTest`
- [x] `CocosTest.exe --gtest_filter=ProfilerTest.*:AssetManagerTest.*:AssetRefManagerTest.*:BinaryDeserializerTest.*:PrefabTest.*:DirectorTest.*:ComponentTest.*:NodeTest.removeComponentDeactivatesAndDetachesComponent`

### F2. Profiler
- [x] 先复用现有 `native/cocos/profiler/Profiler.*`，不再新建第二套 profiler 模块
- [x] 已新增 `ScriptBridge` batch 调用统计接口：记录总 batch 次数、覆盖组件数、累计耗时、按 `start/update/lateUpdate` 分方法计数
- [x] 统计入口已接到 `ScriptBridge::callJSBatchMethod()`，覆盖真实 batch 路径与 fallback 路径
- [x] 输出每帧调度、脚本桥接、渲染前阶段耗时
- [x] 输出 JSB 调用计数到现有 profiler 文本面板或等价调试输出

**本轮验证:**
- [x] `cmake -S tests/unit-test -B build/unit-test-phase-a -G "Visual Studio 17 2022"`
- [x] `cmake --build build/unit-test-phase-a --config Debug --target CocosTest`
- [x] `CocosTest.exe --gtest_filter=ProfilerTest.recordsScriptBridgeBatchCounters`
- [x] `CocosTest.exe --gtest_filter=ProfilerTest.*:AssetManagerTest.*:AssetRefManagerTest.*:BinaryDeserializerTest.*:PrefabTest.*:DirectorTest.*:ComponentTest.*:NodeTest.removeComponentDeactivatesAndDetachesComponent`

---

## Phase G: 全量验证

### G1. 功能回归
- [x] 空场景加载
- [x] 1000 节点场景加载
- [x] Prefab 实例化
- [x] 资产加载/释放循环
- [x] 组件 add/remove/enable/disable/destroy
- [x] persist root 场景切换
- [x] Script start/update/lateUpdate
- [x] GC 安全

**本轮验证（2026-04-29）:**
- [x] `npx jest tests/assets/load-scene.test.ts tests/asset-manager/finalizer.test.ts tests/core/node.test.ts --runInBand`
- [x] `CocosTest.exe --gtest_filter=ComponentTest.*:DirectorTest.*:PrefabTest.*:BinaryDeserializerTest.*:AssetManagerTest.*:AssetRefManagerTest.*`
- [x] `CocosTest.exe --gtest_filter=PhaseGValidationTest.*`
- [x] `CocosTest.exe --gtest_filter=ScriptBridgeTest.*`

### G2. 性能基准

| 基准项 | JS_ONLY 目标 | NATIVE_FAST 目标 |
|--------|-------------|-----------------|
| 1000 节点场景加载 | 58ms | ≤15ms |
| Prefab 实例化 (100节点) | 12ms | ≤4ms |
| 每帧 JSB 桥接次数 | 250 | ≤5 |
| 引用计数同步延迟 | — | ≤1帧 |
| 内存峰值 (场景加载) | — | ≤80% JS_ONLY |

### G3. 压力测试
- [x] 场景加载/卸载 100 次循环
- [x] Prefab 实例化/销毁 1000 次
- [x] 资产引用计数平衡
- [x] ScriptComponent 绑定对象释放

**新增压力验证（2026-04-29）:**
- [x] 新增 `native/tests/unit-test/src/phase_g_validation_test.cpp`
- [x] `PhaseGValidationTest.sceneLoadUnload100Cycles`
- [x] `PhaseGValidationTest.prefabInstantiateDestroy1000Cycles`
- [x] `PhaseGValidationTest.assetRefCountBalance1000Cycles`
- [x] 新增 `native/tests/unit-test/src/script_bridge_test.cpp`
- [x] `ScriptBridgeTest.registerUnregisterScriptInstances1000Cycles`
- [x] `ScriptBridgeTest.gcSafetyAfterBurstRegistration`

---

## 4. 当前执行顺序

### Layer 1
- Phase A 生命周期最小闭环

### Layer 2
- Phase B 场景与反序列化闭环
- Phase C 脚本桥接闭环

### Layer 3
- Phase D 资产引用与释放闭环

### Layer 4
- Phase E 内置组件迁移

### Layer 5
- Phase F 桥接优化与性能

### Layer 6
- Phase G 全量验证

---

## 5. 当前已知风险

| 风险 | 说明 | 缓解 |
|------|------|------|
| 计划与仓库继续漂移 | 文档若继续按旧路径更新，会制造第二套实现 | 后续一律基于当前仓库实际文件更新本计划 |
| 调度入口双权威 | `Engine::tick()` 与 `Director::tick()` 分工不清会造成重复调度 | Phase A 维持现状并加测试，后续单独收口 |
| 资产释放双套设计冲突 | `ReleaseManager` 与假想 `NativeReleaseManager` 并存会制造资源语义分裂 | 只扩展现有 `ReleaseManager` 体系 |
| ScriptBridge 批量执行不稳定 | JS 异常可能污染整个批次 | 按实例隔离异常，批次内部逐项收集错误 |
| Prefab/BinaryDeserializer 实现互相卡住 | 两边都依赖对方会造成 stub 长期留存 | 以 `BinaryDeserializer` 作为底层，Prefab 只负责策略选择 |
| 内置组件迁移工作量失控 | 一次铺太多组件会导致回归激增 | 严格按 P0/P1 分批推进 |

---

## 6. 整体进度总览

### 已完成阶段
- ✅ Phase A: 生命周期最小闭环 (A1-A5)
- ✅ Phase B: 场景与反序列化闭环 (B1-B3)
- ✅ Phase C: 脚本桥接闭环 (C1-C3)
- ✅ Phase D: 资产引用与释放闭环 (D1-D3)
- ✅ Phase E: 内置组件迁移 P0 (E1-E7) — Camera/MeshRenderer/Light 全部完成

### 待推进阶段
- [~] Phase F: 桥接优化与性能
- [x] Phase G: 全量验证

### 关键里程碑
| 里程碑 | 状态 | 日期 |
|--------|------|------|
| M1-M8 骨架/闭环 | ✅ 完成 | 2026-04-25 ~ 04-27 |
| Phase E P0 组件 C++ + JSB | ✅ 完成 | 2026-04-28 |
| Debug/Release 全量编译 | ✅ 零错误 | 2026-04-28 |
| Phase F 桥接优化 | ⏳ 进行中 | 2026-04-28 |
| Phase G 全量验证 | ✅ 完成 | 2026-04-29 |

### 遗留项
- `Engine::tick()` 与 `Director::tick()` 重复调用 update（已知问题，需单独收口）
- `MeshRendererComponent.syncToRenderModel()` 完整 submodel 设置（待后续）
- P1 组件迁移 (Animation/Sprite/RigidBody) 待后续批次

---

_本文件已根据当前仓库代码结构修订。后续模块推进必须继续维护该文档，而不是回退到旧版"从零建文件"的说明。_

_最后更新: 2026-04-29_
