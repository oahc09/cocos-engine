# Cocos Creator v3.8.8 — C++ 化设计总览

> 日期: 2026-04-25
> 分支: v3.8.8_custome
> 目标: 基于 TS 源码深度分析，设计 C++ 化方案，提升运行时性能

---

## 1. 项目产出文件

### 1.1 分析文档（输入）

| 文件 | 内容 | 行数 |
|------|------|------|
| `AI/analysis-component-framework.md` | CCObject/Component/Node组件系统/类注册反射/调度器 | ~400 |
| `AI/analysis-scene-system.md` | Scene类/序列化反序列化/Prefab/场景加载管线/Director | ~260 |
| `AI/analysis-asset-system.md` | Asset基类/AssetManager加载管线/引用计数GC/Native桥接 | ~420 |
| `AI/analysis-script-system.md` | 装饰器/Property系统/脚本编译注册/生命周期/JSB桥接 | ~455 |

### 1.2 设计文档（输出）

| 文件 | 内容 | 核心设计 |
|------|------|---------|
| `AI/design-cpp-component-framework.md` | C++ 组件框架 | Component基类/类注册宏/NodeActivator/ComponentScheduler |
| `AI/design-cpp-scene-system.md` | C++ 场景系统 | 二进制序列化/Prefab预编译/Director C++/双模式运行 |
| `AI/design-cpp-asset-system.md` | C++ 资产系统 | 引用计数统一/NativeAssetManager/Pipeline/ReleaseManager |
| `AI/design-cpp-script-system.md` | C++ 脚本系统 | ScriptBridge/双层调度/内置组件C++化/桥接优化 |

### 1.3 已有文档

| 文件 | 内容 |
|------|------|
| `AI/CocosCreator-v3.8.8-Architecture.md` | 引擎整体架构文档 |
| `AI/TS-vs-Native-Comparison.md` | TS vs Native 双实现对比 |
| `AI/D3D12-GFX-PoC-Checklist.md` | D3D12 GFX PoC 检查清单 |

---

## 2. 设计核心策略

### 2.1 双轨制

```
┌─────────────────────────────────────────────────────┐
│              编辑器模式 (JS_ONLY)                    │
│  JSON反序列化 + JS反射 + JIT编译 + 热重载            │
│  → 保持现有架构不变，零风险                          │
├─────────────────────────────────────────────────────┤
│              运行时模式 (NATIVE_FAST)                │
│  二进制反序列化 + C++反射 + 预编译实例化 + 最小JS    │
│  → 关键路径 C++ 化，显著提升性能                     │
└─────────────────────────────────────────────────────┘
```

### 2.2 四大子系统 C++ 化定位

| 子系统 | 可 C++ 化度 | 核心策略 |
|--------|------------|---------|
| **Component 框架** | ★★★★☆ | 内置组件纯 C++，用户脚本保留 JS，双层调度 |
| **Scene 系统** | ★★★★★ | Scene 已 C++ 化，补全 Director/序列化/Prefab |
| **Asset 系统** | ★★★★☆ | 内置资源 C++ 全权管理，逻辑资源保留 JS |
| **脚本系统** | ★★☆☆☆ | 用户脚本不可 C++ 化，优化调度/桥接开销 |

### 2.3 关键优化指标

| 优化项 | 当前开销 | 优化后 | 加速比 |
|--------|---------|--------|--------|
| 场景加载(1000节点) | 58ms | 15ms | ~4× |
| Prefab 实例化(100节点) | 12ms | 4ms | ~3× |
| 资源加载(100 Texture2D) | 140ms | 78ms | ~1.8× |
| 场景切换释放(500节点) | 25ms | 4ms | ~6× |
| 每帧调度桥接 | ~250次 | ~5次 | ~50× |

---

## 3. 依赖关系图

```
Phase 1 (基础设施)          Phase 2 (核心系统)          Phase 3 (优化)
─────────────────         ─────────────────         ─────────────────

┌──────────────┐
│AssetRefMgr   │──────┐
│引用计数统一   │      │
└──────────────┘      │     ┌──────────────┐
                      ├────→│NativeAssetMgr│──────┐
┌──────────────┐      │     │资产管理C++   │      │
│TypeRegistry  │──────┤     └──────────────┘      │     ┌──────────────┐
│类型注册表    │      │                            ├────→│内置组件      │
└──────────────┘      │     ┌──────────────┐      │     │Camera/Sprite │
                      ├────→│Director C++  │      │     │MeshRenderer  │
┌──────────────┐      │     │场景管理      │      │     └──────────────┘
│ScriptBridge  │──────┤     └──────────────┘      │
│脚本桥接      │      │                            │     ┌──────────────┐
└──────────────┘      │     ┌──────────────┐      ├────→│桥接优化      │
                      ├────→│ComponentSched│      │     │SharedArray   │
┌──────────────┐      │     │双层调度      │      │     │Lazy Binding  │
│BinaryDeser.  │──────┘     └──────────────┘      │     └──────────────┘
│二进制反序列化 │                                    │
└──────────────┘      │     ┌──────────────┐      │     ┌──────────────┐
                      └────→│Prefab C++    │──────┘     │集成测试      │
                            │预编译实例化   │            │性能基准      │
                            └──────────────┘            └──────────────┘
```

---

## 4. 总体实施时间线

```
Week 1-2: Phase 1A — 引用计数统一 + TypeRegistry
Week 3-4: Phase 1B — ScriptBridge + Director C++
Week 5-7: Phase 2A — NativeAssetManager + 二进制序列化
Week 8-9: Phase 2B — ComponentScheduler 双层调度
Week 10-11: Phase 2C — Prefab C++ 化
Week 12-15: Phase 3A — 内置组件 C++ 化 (Camera/MeshRenderer/Light)
Week 16-19: Phase 3B — 内置组件 C++ 化 (Animation/Sprite)
Week 20-21: Phase 3C — 桥接优化
Week 22-23: Phase 4 — 集成测试与性能验证

总计: ~6 个月
```

---

## 5. 模块间接口摘要

### 5.1 Component ↔ Scene

```
Component.start()          ← NodeActivator.activateNode() 调用
Component.update(dt)       ← ComponentScheduler.updatePhase() 调用
Component.node             ← Node 持有 Component 引用
Node.addComponent<T>()     ← TypeRegistry::create(classId)
```

### 5.2 Scene ↔ Asset

```
SceneAsset.getScene()      ← Scene 持有 RenderScene 引用
Scene.autoReleaseAssets    ← ReleaseManager 场景切换时读取
BinaryDeserializer         ← TypeRegistry 创建组件实例
```

### 5.3 Asset ↔ Script

```
ScriptComponentPlaceholder ← 用户脚本延迟绑定
Asset.addRef/decRef        ← ScriptBridge 通知引用变化
getAssetProperties()       ← ReleaseManager 遍历组件资源引用
```

### 5.4 Script ↔ Component

```
ScriptBridge.registerScriptComponent()  ← JS 组件注册到 C++
ScriptBridge.invokeUpdateBatch()        ← C++ 批量调用 JS update
Component.isBuiltin()                   ← 调度器区分内置/脚本
```

---

## 6. 风险矩阵

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| 二进制格式与 JSON 不兼容 | 中 | 高 | 编辑器始终走 JS 路径 |
| 引用计数同步延迟 | 中 | 中 | 原子操作 + 批量同步 |
| 内置组件 C++ 化遗漏属性 | 高 | 高 | 自动化属性覆盖测试 |
| 用户脚本调度时序不一致 | 中 | 高 | 保持 start→update→lateUpdate 顺序 |
| ScriptBridge 批量失败 | 低 | 高 | 分批 + 异常隔离 |
| 双模式运行时切换不稳定 | 低 | 中 | 启动时确定模式，运行中不切换 |
| 大规模重构影响现有功能 | 高 | 高 | 渐进式替换 + 完整回归测试 |

---

## 7. 下一步行动

1. **Review 四份设计文档** — 确认架构方案和优先级
2. **Phase 1A 实施** — AssetRefManager + TypeRegistry（最底层，无依赖）
3. **PoC 验证** — 选择 Camera 组件做端到端 C++ 化 PoC
4. **性能基准建立** — 记录当前 JS 路径的各阶段耗时
5. **D3D12 GFX 并行推进** — 与渲染层 C++ 化协同设计
