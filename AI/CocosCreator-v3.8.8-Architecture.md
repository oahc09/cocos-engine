# Cocos Creator Engine v3.8.8 架构分析文档

> **引擎版本**: 3.8.8 | **许可证**: MIT | **源码路径**: `d:\cocos_custome\cocos-engine`

---

## 目录

1. [顶层项目结构](#1-顶层项目结构)
2. [构建体系与模块化配置](#2-构建体系与模块化配置)
3. [核心模块详解](#3-核心模块详解)
4. [模块依赖关系图](#4-模块依赖关系图)
5. [入口调用逻辑与初始化流程](#5-入口调用逻辑与初始化流程)
6. [平台抽象层 (PAL)](#6-平台抽象层-pal)
7. [原生绑定 (JSB)](#7-原生绑定-jsb)
8. [渲染管线架构](#8-渲染管线架构)
9. [编译时常量与条件编译](#9-编译时常量与条件编译)
10. [模块覆盖机制](#10-模块覆盖机制)

---

## 1. 顶层项目结构

```
cocos-engine/
├── @types/              # 全局类型定义 (16 .ts)
│   ├── globals.d.ts     # 全局变量声明
│   ├── jsb.d.ts         # JSB 原生绑定类型
│   ├── consts.d.ts      # 编译常量类型
│   ├── webGL*.d.ts      # WebGL 扩展类型
│   ├── webGPU.d.ts      # WebGPU 类型
│   └── pal/             # PAL 虚拟模块类型定义
│       ├── system-info, screen-adapter, minigame
│       ├── audio, input, env, pacer, wasm
├── cocos/               # ★ 引擎核心源码 (34 子模块, 1244 .ts)
├── exports/             # ★ 公共导出层 (47 .ts) — 用户可见 API 入口
├── pal/                 # ★ 平台抽象层 (80 .ts)
├── native/              # C++ 原生层 (16691 文件, .h/.hpp/.cpp/.js)
├── editor/              # 编辑器资源 (effects, chunks, prefabs)
├── scripts/             # 构建与工具脚本 (569 .js)
├── platforms/           # 平台适配器 (272 .js)
├── templates/           # 项目模板 (Android/iOS/MiniGame)
├── tests/               # 单元测试 (230 .ts)
├── external/            # 第三方库 (zlib 等)
├── vendor/              # 供应商扩展 (Google 等)
├── extensions/          # 引擎扩展
├── bin/                 # 构建产物输出
├── docs/                # 文档与图示
├── licenses/            # 第三方许可证
├── predefine.ts         # 引擎入口预定义 (导出 legacyCC)
├── cc.config.json       # ★ 模块化构建配置 (核心)
├── cc.config.schema.json # 配置 Schema
├── package.json         # NPM 包配置
├── tsconfig.json        # TypeScript 编译配置
└── babel.config.js      # Babel 转译配置
```

### 关键配置文件

| 文件 | 作用 |
|------|------|
| `package.json` | 引擎包名 `cocos-creator` v3.8.8, 入口 `index.js` 导出 `legacyCC` |
| `tsconfig.json` | ES6 + CommonJS, 路径映射 `cc.decorator` → `cocos/core/data/decorators` |
| `cc.config.json` | **852 行核心配置**: features/modules/constants/moduleOverrides/treeShake |
| `predefine.ts` | `import { legacyCC } from './cocos/core/global-exports'; export default legacyCC;` |

---

## 2. 构建体系与模块化配置

### 2.1 构建脚本链

```
npm run build:min
  └─ build:debug-infos       → scripts/build-debug-infos.js
  └─ build-h5-minified       → @cocos/ccbuild.buildEngine({mode:'BUILD', platform:'HTML5', compress:true})

npm run build:dev
  └─ build:debug-infos
  └─ build-h5-source         → @cocos/ccbuild.buildEngine({mode:'BUILD', platform:'HTML5', compress:false})
```

**核心构建工具**: `@cocos/ccbuild` — 负责模块解析、条件编译、Tree Shaking、产物打包。

### 2.2 cc.config.json 特性模块体系

定义了 **35+ 个特性模块 (features)**，每个映射到内部模块和资源：

| 特性 | 内部模块 | 说明 |
|------|---------|------|
| `base` | `base` | 基础模块（始终包含） |
| `gfx-webgl` / `gfx-webgl2` / `gfx-webgpu` / `gfx-empty` | 同名 | 图形后端选择 |
| `3d` | `3d` | 3D 渲染，启用 `USE_3D` 常量 |
| `2d` | `2d`, `sorting` | 2D 渲染 + 排序 |
| `animation` / `skeletal-animation` | `animation`, `skeletal-animation` | 动画系统 |
| `physics-cannon` / `physics-physx` / `physics-ammo` / `physics-builtin` | `physics-*` + `physics-framework` | 3D 物理后端（策略模式） |
| `physics-2d-box2d` / `physics-2d-box2d-jsb` / `physics-2d-box2d-wasm` / `physics-2d-builtin` | `physics-2d-*` + `physics-2d-framework` | 2D 物理后端 |
| `spine-3.8` / `spine-4.2` | `spine` | Spine 动画版本切换 |
| `dragon-bones` | `dragon-bones` | 龙骨动画 |
| `marionette` | (intrinsicFlags) | 木偶动画系统 |
| `custom-pipeline` / `custom-pipeline-post-process` | 对应模块 | 自定义渲染管线 |
| `xr` | `xr` | XR/VR 支持 |

---

## 3. 核心模块详解

### 3.1 cocos/ 目录完整模块清单

```
cocos/
├── core/               ★ 核心基础层 (131 .ts)
│   ├── math/           数学库: Vec2/3/4, Mat3/4, Quat, Color, Size, Rect
│   ├── geometry/       几何体: AABB, OBB, Sphere, Frustum, Ray, Plane, Capsule
│   ├── data/           数据系统: CCClass, CCObject, 装饰器, 属性系统
│   │   ├── class.ts        CCClass 类系统引擎
│   │   ├── object.ts       CCObject 基类 (CCObjectFlags 枚举)
│   │   └── decorators/     @ccclass, @property, @serializable, @type, @override
│   ├── event/          事件系统: EventTarget, Eventify, AsyncDelegate
│   ├── memop/          内存池: Pool, RecyclePool, CachedArray
│   ├── value-types/    值类型: ValueType, Enum, BitMask
│   ├── curves/         曲线: EasingMethod, Bezier
│   ├── platform/       平台: sys, macro, debug, screen, visible-rect
│   ├── algorithm/      算法: binary-search
│   ├── scheduler.ts    调度器 (45KB, 引擎核心调度)
│   ├── settings.ts     设置系统
│   ├── system.ts       系统基类
│   ├── global-exports.ts  全局导出: cclegacy/legacyCC 命名空间
│   └── legacy.ts       旧版兼容: legacyCC.log/warn/error/path
│
├── gfx/                ★ 图形抽象层 (109 .ts)
│   ├── base/           抽象接口定义
│   │   ├── device.ts       Device 抽象类 (GFX 设备)
│   │   ├── buffer.ts, command-buffer.ts, shader.ts
│   │   ├── texture.ts, render-pass.ts, framebuffer.ts
│   │   ├── pipeline-state.ts, pipeline-layout.ts
│   │   ├── descriptor-set.ts, descriptor-set-layout.ts
│   │   ├── input-assembler.ts, queue.ts, swapchain.ts
│   │   └── define.ts       枚举/接口定义 (API, Feature, Format 等)
│   ├── device-manager.ts   设备管理器 (创建/选择 GFX 设备)
│   ├── webgl/         WebGL 1.0 实现
│   ├── webgl2/        WebGL 2.0 实现
│   └── webgpu/        WebGPU 实现
│
├── rendering/          ★ 渲染管线 (110 .ts)
│   ├── define.ts          渲染定义 (UBO 布局, 管线常量, 51KB)
│   ├── render-pipeline.ts 渲染管线基类
│   ├── render-stage.ts    渲染阶段
│   ├── render-flow.ts     渲染流程
│   ├── render-queue.ts    渲染队列
│   ├── pipeline-ubo.ts    UBO 管理 (29KB)
│   ├── instanced-buffer.ts 实例化缓冲
│   ├── debug-view.ts      调试视图
│   └── custom/            自定义渲染管线
│       ├── index.ts           创建/管理自定义管线
│       ├── pipeline.ts        BasicPipeline, PipelineBuilder
│       ├── web-pipeline.ts    WebPipeline 实现
│       └── layout-graph.ts    布局图
│
├── render-scene/       ★ 渲染场景 (45 .ts)
│   ├── core/
│   │   ├── render-scene.ts  RenderScene
│   │   ├── render-window.ts RenderWindow
│   │   ├── pass.ts          Pass (着色器 Pass)
│   │   ├── program-lib.ts   着色器程序库
│   │   ├── material-instance.ts
│   │   └── native-pools.ts  原生对象池
│   └── scene/
│       ├── camera.ts        Camera
│       ├── model.ts         Model/SubModel
│       ├── light.ts         Light (方向/点/聚光/球面光)
│       ├── skybox.ts, shadows.ts, fog.ts, ambient.ts
│       └── reflection-probe.ts
│
├── scene-graph/        ★ 场景图 (28 .ts)
│   ├── node.ts            Node (3014行, 引擎最核心类)
│   ├── scene.ts           Scene
│   ├── component.ts       Component 基类
│   ├── layers.ts          Layers 层级系统
│   ├── scene-globals.ts   SceneGlobals
│   ├── node-event-processor.ts 节点事件处理
│   ├── component-scheduler.ts   组件调度
│   ├── node-activator.ts  节点激活器
│   └── prefab.ts          Prefab 系统
│
├── game/               ★ 游戏主循环 (5 .ts)
│   ├── game.ts            Game 类 (1200行, 初始化+主循环)
│   ├── director.ts        Director 单例 (956行, 场景管理+帧调度)
│   ├── splash-screen.ts   启动画面
│   └── index.ts           导出 Game, Director
│
├── 2d/                 ★ 2D 渲染系统 (86 .ts)
│   ├── framework/         UI 框架: Canvas, UITransform, UIRenderer, Sprite
│   ├── components/        UI 组件: Label, Sprite, LabelOutline, UIOpacity
│   ├── renderer/          渲染器: Batcher2D, RenderData, MeshBuffer, StencilManager
│   ├── assembler/         组装器: LabelAssembler, SpriteAssembler
│   ├── assets/            2D 资源: SpriteFrame, BitmapFont, TTFFont
│   └── utils/             2D 工具
│
├── 3d/                 ★ 3D 渲染系统 (55 .ts)
│   ├── assets/            3D 资源: Mesh, Skeleton, Morph
│   ├── framework/         3D 框架: MeshRenderer
│   ├── models/            3D 模型: MorphModel, SkinningModel, BakedSkinningModel
│   ├── lights/            3D 灯光组件
│   ├── skeletal-animation/ 骨骼动画: DataPoolManager
│   ├── misc/              3D 工具: createMesh, batch-utils
│   └── reflection-probe/  反射探针
│
├── animation/          动画系统 (150 .ts)
│   ├── animation.ts       Animation 管理
│   ├── animation-clip.ts  AnimationClip
│   ├── animation-state.ts AnimationState
│   ├── animation-component.ts Animation 组件
│   └── marionette/        木偶动画系统 (状态机)
│       ├── runtime-exports.ts
│       └── pose-graph/    姿态图
│
├── physics/            3D 物理系统 (128 .ts)
│   ├── framework/         物理框架: PhysicsSystem, RigidBody, Collider*
│   ├── cannon/            Cannon.js 后端
│   ├── physx/             PhysX 后端
│   ├── bullet/            Bullet 后端
│   └── builtin/           内置简易物理
│
├── physics-2d/         2D 物理系统 (99 .ts)
│   ├── framework/         2D 物理框架
│   ├── box2d/             Box2D 后端
│   └── builtin/           内置简易 2D 物理
│
├── asset/              资产管理 (66 .ts)
│   ├── assets/            Asset 基类及派生
│   │   ├── asset.ts           Asset
│   │   ├── texture-2d.ts      Texture2D
│   │   ├── texture-cube.ts    TextureCube
│   │   ├── material.ts        Material
│   │   ├── effect-asset.ts    EffectAsset
│   │   ├── scene-asset.ts     SceneAsset
│   │   └── render-texture.ts  RenderTexture
│   └── asset-manager/     资源管理器
│       ├── asset-manager.ts   AssetManager 单例
│       ├── bundle.ts          Bundle 资源包
│       ├── builtin-res-mgr.ts 内置资源管理
│       └── release-manager.ts 资源释放管理
│
├── input/              输入系统 (21 .ts)
│   ├── input.ts           Input 单例
│   └── system-event.ts    SystemEvent
│
├── ui/                 UI 组件 (26 .ts)
│   ├── Button, Layout, ScrollView, Slider, Toggle
│   ├── ProgressBar, PageView, EditBox, Widget
│   ├── SafeArea, ScrollBar, ToggleContainer
│   └── widget-manager.ts  Widget 管理器
│
├── audio/              音频系统
├── video/              视频播放
├── web-view/           WebView 组件
├── tween/              缓动动画 (12 .ts)
├── particle/           3D 粒子系统 (31 .ts)
├── particle-2d/        2D 粒子系统 (10 .ts)
├── spine/              Spine 动画 (22 .ts, 支持多版本)
├── dragon-bones/       龙骨动画 (14 .ts)
├── terrain/            地形系统
├── tiledmap/           TiledMap 地图
├── primitive/          基础几何体 (14 .ts)
├── serialization/      序列化系统 (11 .ts): deserialize, instantiate
├── sorting/            排序系统
├── gi/                 全局光照 (9 .ts): LightProbe
├── profiler/           性能分析
├── xr/                 XR/VR 系统
├── webgpu/             WebGPU 特有实现
├── native-binding/     原生绑定
│   ├── index.ts           (66KB) 原生 API 类型声明
│   ├── impl.ts            原生桥接实现 (jsb.reflection, jsb.bridge)
│   └── decorators.ts      绑定装饰器 (127KB)
├── misc/               杂项: Camera, ModelRenderer, MissingScript
├── deprecated.ts       废弃接口
├── root.ts             ★ Root 类 (819行, 渲染根管理器)
└── root.jsb.ts         Root 原生平台实现 (271行)
```

### 3.2 exports/ — 公共 API 导出层

`exports/` 是用户可用的 API 边界。每个文件对应一个特性模块：

```
exports/
├── base.ts             ★ 核心: core + rendering + scene-graph + game + Root + serialization + asset + input + native-binding
├── 2d.ts / 3d.ts       直接转发: export * from '../cocos/2d' / '../cocos/3d'
├── animation.ts        动画
├── audio.ts / video.ts / webview.ts  媒体
├── physics-framework.ts  物理框架 + 碰撞体/刚体/约束
├── physics-cannon/physx/ammo/builtin.ts  物理后端
├── physics-2d-*.ts     2D 物理后端
├── custom-pipeline.ts   自定义渲染管线 (legacyCC.rendering)
├── gfx-webgl/webgl2/webgpu/empty.ts  GFX 后端
├── ui.ts               UI 组件
├── tween.ts            缓动
├── spine.ts            Spine
├── dragon-bones.ts     龙骨
├── terrain.ts          地形
├── tiled-map.ts        TiledMap
├── particle.ts / particle-2d.ts  粒子
├── primitive.ts        基础几何
├── profiler.ts         性能分析
└── xr.ts               XR
```

**关键导出链**: `exports/base.ts` → `cocos/core` → `global-exports.ts` → `cclegacy` (全局命名空间)

---

## 4. 模块依赖关系图

```
┌─────────────────────────────────────────────────────┐
│                     用户代码 (cc module)              │
└────────────────────┬────────────────────────────────┘
                     │ import from 'cc'
                     ▼
┌─────────────────────────────────────────────────────┐
│              exports/ (公共 API 层)                    │
│  base → core, rendering, scene-graph, game,          │
│         asset, input, gfx, serialization, native     │
└────────┬───────────────────────────┬─────────────────┘
         │                           │
    ┌────▼────┐                ┌─────▼──────┐
    │  cocos/  │                │   pal/     │
    │ 核心模块  │◄──────────────│ 平台抽象层  │
    └────┬────┘                └────────────┘
         │
    ┌────▼────────────────────────────────────┐
    │           模块层级依赖                     │
    │                                          │
    │  Game ──► Director ──► Root              │
    │    │          │          │               │
    │    │          │          ├── RenderScene  │
    │    │          │          ├── RenderWindow │
    │    │          │          ├── Pipeline     │
    │    │          │          └── Batcher2D    │
    │    │          │                          │
    │    │          ├── Scene ──► Node          │
    │    │          │              │            │
    │    │          │              ├── Component│
    │    │          │              └── Children │
    │    │          │                          │
    │    │          └── Scheduler              │
    │    │                                     │
    │    ├── GFX Device ◄── WebGL/WebGL2/WebGPU│
    │    ├── AssetManager ◄── Bundle/Resources │
    │    ├── PhysicsSystem ◄── Cannon/PhysX    │
    │    ├── AnimationManager                  │
    │    └── Input System                      │
    └─────────────────────────────────────────┘
```

### 核心类继承关系

```
CCObject (cocos/core/data/object.ts)
  ├── Asset (cocos/asset/assets/asset.ts)
  │   ├── Texture2D, TextureCube, RenderTexture
  │   ├── Material, EffectAsset
  │   ├── SceneAsset, Mesh, Skeleton, Morph
  │   ├── SpriteFrame, BitmapFont, TTFFont
  │   ├── PhysicsMaterial, AudioAsset
  │   └── ...
  ├── Node (cocos/scene-graph/node.ts) [3014行]
  └── Component (cocos/scene-graph/component.ts)
      ├── MeshRenderer, SkinnedMeshRenderer
      ├── Camera, Light, AudioSource
      ├── UI: UITransform, Sprite, Label, Button, Layout...
      ├── Physics: RigidBody, Collider, Constraint
      ├── Animation
      └── ...

EventTarget (cocos/core/event/event-target.ts)
  ├── Game (cocos/game/game.ts)
  ├── Director (cocos/game/director.ts)
  └── Node (via Eventify mixin)
```

---

## 5. 入口调用逻辑与初始化流程

### 5.1 引擎入口

```
predefine.ts → import { legacyCC } from './cocos/core/global-exports'
            → export default legacyCC

global-exports.ts → cclegacy = { _global: window/global }
                  → legacyCC.ENGINE_VERSION = '3.8.8'
                  → window.cc = legacyCC
                  → cclegacy.internal = {}
```

### 5.2 Game.init() 完整初始化流程

```
Game.init(config)
│
├── Phase 1: Base 基础模块初始化
│   ├── EVENT_PRE_BASE_INIT
│   ├── _resetDebugSetting(debugMode)
│   ├── sys.init()                    ← PAL 系统信息初始化
│   ├── _initEvents()                 ← 注册 pause/resume/low-memory 事件
│   ├── settings.init()               ← 加载 settings.json + overrideSettings
│   └── EVENT_POST_BASE_INIT
│
├── Phase 2: Infrastructure 基础设施初始化
│   ├── EVENT_PRE_INFRASTRUCTURE_INIT
│   ├── macro.init()                  ← 全局宏配置
│   ├── _initXR()                     ← XR 初始化
│   ├── findCanvas()                  ← 获取 Canvas 适配器
│   ├── screen.init()                 ← 屏幕适配
│   ├── garbageCollectionManager.init()
│   ├── deviceManager.init()          ← ★ 创建 GFX Device (WebGL/WebGL2/WebGPU)
│   ├── [custom pipeline 检查]        ← cclegacy.rendering 配置
│   ├── assetManager.init()           ← 资源管理器
│   ├── builtinResMgr.init()          ← 内置资源
│   ├── Layers.init()                 ← 层级系统
│   ├── initPacer()                   ← 帧率控制器 (requestAnimationFrame/setTimeout)
│   └── EVENT_POST_INFRASTRUCTURE_INIT
│
├── Phase 3: Subsystem 子系统初始化
│   ├── EVENT_PRE_SUBSYSTEM_INIT
│   ├── effectSettings.init()         ← Effect 配置加载
│   ├── cclegacy.rendering.init()     ← 自定义渲染管线初始化
│   ├── [scriptPackages 加载]         ← 动态脚本包
│   ├── director.init()               ← ★ Director 单例初始化
│   ├── builtinResMgr.loadBuiltinAssets()  ← 加载内置资源
│   └── EVENT_POST_SUBSYSTEM_INIT
│       → EVENT_ENGINE_INITED         ← ★ 引擎初始化完成
│
└── Phase 4: Project 项目数据初始化
    ├── EVENT_PRE_PROJECT_INIT
    ├── [jsList 加载]                 ← 插件脚本
    ├── _loadProjectBundles()          ← 项目 Bundle 加载
    ├── _loadCCEScripts()              ← CCE 脚本加载
    ├── _setupRenderPipeline()         ← ★ 渲染管线搭建
    │   ├── director.buildRenderPipeline()
    │   │   └── root.setRenderPipeline()
    │   │       ├── [custom] rendering.createCustomPipeline() → WebPipeline
    │   │       └── [legacy]  legacy_rendering.createDefaultPipeline()
    │   └── root.initialize()          ← 创建主窗口, RenderPass
    ├── _loadPreloadAssets()           ← 预加载资源
    ├── builtinResMgr.compileBuiltinMaterial()  ← 编译内置材质
    ├── SplashScreen.init()            ← 启动画面
    └── EVENT_POST_PROJECT_INIT
        → EVENT_GAME_INITED            ← ★ 游戏初始化完成
```

### 5.3 主循环 (Game Loop)

```
Game.run()
  └── Pacer (requestAnimationFrame / setTimeout)
        └── Game._loop()
              └── Director.tick(deltaTime)
                    │
                    ├── Director.BEGIN_FRAME
                    ├── Director.BEFORE_UPDATE
                    │   └── ComponentScheduler.startPhase()  ← 组件 start()
                    ├── Director.UPDATE
                    │   └── ComponentScheduler.updatePhase() ← 组件 update(dt)
                    │   └── AnimationManager.update()         ← 动画更新
                    │   └── PhysicsSystem.update()            ← 物理更新
                    │   └── TweenSystem.update()              ← 缓动更新
                    ├── Director.AFTER_UPDATE
                    │   └── Director.BEFORE_RENDER
                    │       └── Root.frameMove(deltaTime)     ← ★ 渲染帧
                    │           ├── _frameMoveBegin()
                    │           │   └── scenes[i].removeBatches()
                    │           ├── _frameMoveProcess()
                    │           │   ├── windows[i].extractRenderCameras()
                    │           │   ├── device.acquire([swapchain])
                    │           │   ├── batcher2D.update() + uploadBuffers()
                    │           │   └── scenes[i].update(stamp)
                    │           └── _frameMoveEnd()
                    │               ├── Director.BEFORE_COMMIT
                    │               ├── cameras.sort(priority)
                    │               ├── Director.BEFORE_RENDER
                    │               ├── pipeline.render(cameraList) ← ★ 管线渲染
                    │               ├── Director.AFTER_RENDER
                    │               └── device.present()            ← 提交帧
                    ├── Director.AFTER_DRAW
                    └── Director.END_FRAME
```

---

## 6. 平台抽象层 (PAL)

PAL (Platform Abstraction Layer) 是引擎跨平台的核心机制，位于 `pal/` 目录：

```
pal/
├── system-info/     系统信息: web/ | native/ | minigame/
├── screen-adapter/  屏幕适配: web/ | native/ | minigame/
├── audio/           音频播放: web/player.ts | native/player.ts | minigame/player.ts
├── input/           输入处理: web/index.ts | native/index.ts | minigame/index.ts
├── env/             运行环境: web/env.ts | native/env.ts | minigame/env.ts | runtime/env.ts
├── pacer/           帧率控制: pacer-web.ts | pacer-native.ts | pacer-minigame.ts
├── minigame/        小游戏适配: wechat/ | xiaomi/ | alipay/ | bytedance/ | oppo/ | vivo/ ...
└── wasm/            WASM 加载: wasm-web.ts | wasm-native.ts | wasm-minigame.ts
```

**路由机制**: 通过 `cc.config.json` 的 `moduleOverrides` 配置，在构建时将虚拟模块 `pal/audio` 等映射到具体平台实现：

| 构建目标 | pal/audio → | pal/system-info → | pal/env → |
|---------|------------|-------------------|----------|
| HTML5 | `pal/audio/web/player.ts` | `pal/system-info/web/system-info.ts` | `pal/env/web/env.ts` |
| NATIVE | `pal/audio/native/player.ts` | `pal/system-info/native/system-info.ts` | `pal/env/native/env.ts` |
| MINIGAME | `pal/audio/minigame/player.ts` | `pal/system-info/minigame/system-info.ts` | `pal/env/minigame/env.ts` |
| RUNTIME | `pal/audio/minigame/player.ts` | `pal/system-info/minigame/system-info.ts` | `pal/env/runtime/env.ts` |

---

## 7. 原生绑定 (JSB)

### 7.1 双实现机制

每个跨平台核心类都有两个实现文件：

| Web 实现 (.ts) | 原生实现 (.jsb.ts) | 说明 |
|---------------|-------------------|------|
| `cocos/root.ts` | `cocos/root.jsb.ts` | Root 原生使用 `jsb.Root` |
| `cocos/scene-graph/node.ts` | `cocos/scene-graph/node.jsb.ts` | Node 原生扩展 |
| `cocos/gfx/index.ts` | `cocos/gfx/index.jsb.ts` | GFX 原生设备 |
| `cocos/rendering/index.ts` | `cocos/rendering/index.jsb.ts` | 渲染管线 |
| `cocos/asset/assets/*.ts` | `cocos/asset/assets/*.jsb.ts` | 资产原生优化 |
| `cocos/2d/renderer/native-2d.ts` | `cocos/2d/renderer/native-2d.jsb.ts` | 2D 渲染 |

### 7.2 原生桥接层

```typescript
// cocos/native-binding/impl.ts
native = {
    Downloader,      // 下载器
    fileUtils,       // 文件工具
    reflection,      // Java/ObjC/ArkTS 反射调用
    bridge,          // ScriptNativeBridge 双向通信
    jsbBridgeWrapper,// 事件封装
    garbageCollect,  // GC 触发
    saveImageData,   // 图像保存
    AssetsManager,   // 热更新
};
```

- **Android**: `JavascriptJavaBridge` → Java 反射
- **iOS/macOS**: `JavaScriptObjCBridge` → ObjC 反射
- **OpenHarmony**: `JavaScriptArkTsBridge` → ArkTS 反射
- **通用**: `ScriptNativeBridge` → 事件总线 `jsbBridgeWrapper`

---

## 8. 渲染管线架构

### 8.1 双管线模式

```
Root.setRenderPipeline(useCustomPipeline)
│
├── [Custom Pipeline] (默认)
│   ├── cclegacy.rendering.createCustomPipeline()
│   │   └── WebPipeline (cocos/rendering/custom/web-pipeline.ts)
│   │       └── PipelineBuilder 策略模式
│   │           ├── "Forward" (前向渲染)
│   │           ├── "Deferred" (延迟渲染)
│   │           └── 自定义管线名
│   ├── LayoutGraph → RenderPass 配置
│   ├── ProgramLibrary → Shader 编译
│   └── 支持 Effect Import
│
└── [Legacy Pipeline] (旧版)
    ├── cclegacy.legacy_rendering.createDefaultPipeline()
    └── ForwardPipeline / DeferredPipeline
```

### 8.2 渲染帧流程

```
Root.frameMove(dt)
  └── _frameMoveBegin()     ← 清除批次
  └── _frameMoveProcess()   ← 提取相机 + 更新场景
  │     ├── device.acquire()
  │     ├── batcher2D.update() → uploadBuffers()
  │     └── scenes[i].update(stamp)
  └── _frameMoveEnd()       ← 排序 + 渲染 + 提交
        ├── cameras.sort(priority)
        ├── pipeline.render(cameraList)
        │   └── RenderFlow.execute()
        │       └── RenderStage.execute()
        │           ├── RenderQueue.sort()
        │           ├── InstancedBuffer.uploadBuffers()
        │           └── CommandBuffer.drawArrays/drawIndexed
        └── device.present()
```

### 8.3 GFX 抽象层

```
Device (抽象类)
├── WebGLDevice     → WebGL 1.0 后端
├── WebGL2Device    → WebGL 2.0 后端 (默认)
├── WebGPUDevice    → WebGPU 后端
└── EmptyDevice     → 空设备 (测试/服务端)

Device 核心接口:
├── createBuffer()          缓冲区
├── createTexture()         纹理
├── createShader()          着色器
├── createPipelineState()   管线状态
├── createRenderPass()      渲染通道
├── createFramebuffer()     帧缓冲
├── createInputAssembler()  输入组装
├── createDescriptorSet()   描述符集
├── acquire()               获取帧资源
├── present()               提交帧
└── executeCommandBuffers() 执行命令
```

---

## 9. 编译时常量与条件编译

`cc.config.json` 定义了 **30+ 个编译时常量**，通过 `internal:constants` 模块在编译时注入：

### 平台常量

| 常量 | 说明 | 动态 |
|------|------|------|
| `HTML5` | Web 平台 | ✓ |
| `NATIVE` | 原生平台 = `$ANDROID \|\| $IOS \|\| $MAC \|\| $WINDOWS \|\| $LINUX \|\| $OHOS \|\| $OPEN_HARMONY` | ✓ |
| `ANDROID` / `IOS` / `MAC` / `WINDOWS` / `LINUX` | 具体 OS | ✗ |
| `OHOS` / `OPEN_HARMONY` | 鸿蒙系统 | ✗ |

### 小游戏常量

| 常量 | 说明 |
|------|------|
| `WECHAT` / `XIAOMI` / `ALIPAY` / `BYTEDANCE` | 小游戏平台 |
| `OPPO` / `VIVO` / `HUAWEI` / `MIGU` / `HONOR` | 快游戏平台 |
| `MINIGAME` | 小游戏合集 (动态或运算) |
| `RUNTIME_BASED` | Runtime 平台合集 (动态或运算) |

### 环境常量

| 常量 | 说明 | 动态 |
|------|------|------|
| `EDITOR` | 编辑器模式 | ✓ |
| `PREVIEW` | 预览模式 | ✓ |
| `BUILD` | 构建发布 | ✗ |
| `TEST` | 单元测试 | ✓ |
| `DEBUG` | 调试模式 | ✗ |
| `DEV` | 开发模式 = `$EDITOR \|\| $PREVIEW \|\| $TEST` | ✓ |
| `JSB` | JSB 绑定 = `$NATIVE` | ✓ |

### 引擎内部常量

| 常量 | 说明 |
|------|------|
| `USE_3D` | 启用 3D 模块 |
| `USE_XR` | 启用 XR 模块 |
| `USE_UI_SKEW` | 启用 UI 倾斜 |
| `USE_SORTING_2D` | 启用 2D 排序 |
| `WEBGPU` | WebGPU 渲染后端 |
| `MARIONETTE` | 木偶动画系统 |
| `PROCEDURAL_ANIMATION` | 程序化动画 |
| `SPINE_3_8` / `SPINE_4_2` | Spine 版本选择 |

---

## 10. 模块覆盖机制

`cc.config.json` 的 `moduleOverrides` 是实现跨平台适配的**核心机制**，在构建时根据条件替换源文件：

### 覆盖规则列表

| 条件 | 覆盖规则 | 类型 |
|------|---------|------|
| `NATIVE` | 40+ 个 `.ts` → `.jsb.ts` (Root, Node, Scene, Camera, GFX, Asset 等) | 文件替换 |
| `!NATIVE` | `native-2d.ts` → `native-2d-empty.ts` | 文件替换 |
| `HTML5` | PAL 虚拟模块 → `pal/*/web/*` | 虚拟模块映射 |
| `NATIVE` | PAL 虚拟模块 → `pal/*/native/*` | 虚拟模块映射 |
| `MINIGAME` | PAL 虚拟模块 → `pal/*/minigame/*` + 动态平台名 | 虚拟模块映射 |
| `RUNTIME_BASED` | PAL 虚拟模块 → `pal/*/runtime/*` 或 `minigame/*` | 虚拟模块映射 |
| `SPINE_3_8` | `spine-version.ts` → `spine-version-3.8.ts` | 版本切换 |
| `SPINE_4_2` | `spine-version.ts` → `spine-version-4.2.ts` | 版本切换 |
| `!MARIONETTE` | `runtime-exports.ts` → `index-empty.ts` | 功能裁剪 |
| `!PROCEDURAL_ANIMATION` | `pose-graph/runtime-exports.ts` → 空导出 | 功能裁剪 |
| `NOT_PACK_PHYSX_LIBS` | `physx.asmjs.ts` → `physx.null.ts` | 物理裁剪 |
| `USE_VENDOR_GOOGLE` | `vendor/google/index.ts` → `vendor/google/impl.ts` | 供应商扩展 |

### 虚拟模块 (isVirtualModule)

PAL 模块使用虚拟模块映射，TypeScript 源码中 `import { systemInfo } from 'pal/system-info'` 并不直接指向文件，而是在构建时根据 `cc.config.json` 解析到实际平台实现。类型定义在 `@types/pal/` 中声明。

---

## 附录 A: 关键文件路径速查

| 用途 | 路径 |
|------|------|
| 引擎入口 | `predefine.ts` → `cocos/core/global-exports.ts` |
| 全局命名空间 | `cocos/core/global-exports.ts` (cclegacy/legacyCC) |
| 类系统 | `cocos/core/data/class.ts` (CCClass) |
| 对象基类 | `cocos/core/data/object.ts` (CCObject) |
| 装饰器 | `cocos/core/data/decorators/` (@ccclass, @property 等) |
| 节点 | `cocos/scene-graph/node.ts` (Node, 3014行) |
| 组件 | `cocos/scene-graph/component.ts` (Component) |
| 场景 | `cocos/scene-graph/scene.ts` (Scene) |
| 游戏主类 | `cocos/game/game.ts` (Game, 1200行) |
| 导演器 | `cocos/game/director.ts` (Director, 956行) |
| 渲染根 | `cocos/root.ts` (Root, 819行) |
| GFX 设备 | `cocos/gfx/base/device.ts` (Device 抽象) |
| 设备管理 | `cocos/gfx/device-manager.ts` (deviceManager) |
| 渲染管线 | `cocos/rendering/render-pipeline.ts` |
| 自定义管线 | `cocos/rendering/custom/index.ts` |
| 2D 渲染 | `cocos/2d/renderer/batcher-2d.ts` (Batcher2D) |
| 资源管理 | `cocos/asset/asset-manager/asset-manager.ts` |
| 物理框架 | `cocos/physics/framework/index.ts` |
| 原生桥接 | `cocos/native-binding/impl.ts` |
| 构建配置 | `cc.config.json` (852行) |
| PAL 类型 | `@types/pal/` |

## 附录 B: 核心依赖包

| 包名 | 版本 | 说明 |
|------|------|------|
| `@cocos/box2d` | 1.0.2 | 2D 物理引擎 (Box2D) |
| `@cocos/cannon` | 1.2.8 | 3D 物理引擎 (Cannon.js) |
| `@cocos/ccbuild` | ^2.3.16 | 引擎构建工具 |
| `@cocos/dragonbones-js` | ^1.0.1 | 龙骨动画 |
| `typescript` | ^4.9.5 | TypeScript 编译器 |
| `tslib` | ^2.8.1 | TypeScript 运行时库 |

---

*文档生成时间: 2026-04-24 | 基于 Cocos Creator Engine v3.8.8 源码分析*
