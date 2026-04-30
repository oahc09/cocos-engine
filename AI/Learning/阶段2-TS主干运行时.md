# 阶段2-TS主干运行时（S2-T5）

> 范围：阶段2全量收敛（`cocos/core` / `cocos/game` / `scene-graph` / `asset` / `rendering` / `animation` / `ui` / `physics` / `audio`）  
> 目标：输出 2 条关键调用链 + 2 个问题定位样例，作为阶段2结项文档。

---

## 1. 阶段2收敛结论（先看）

- 关键调用链 A（场景链）：`Director.loadScene -> Bundle.loadScene -> AssetManager.loadAny -> Director.runSceneImmediate -> Scene._load/_activate`。  
- 关键调用链 B（帧链）：`Director.tick -> System.update/postUpdate -> Root.frameMove -> Pipeline.render -> device.present`。  
- 阶段2的高频排障，本质都落到这两条主链上的某一跳。

---

## 2. 关键调用链 A：场景加载与切换链

## 2.1 链路描述

1. `Director.loadScene(sceneName)`：
   - 在已注册 bundle 中查找场景；
   - 发 `BEFORE_SCENE_LOADING`；
   - 调用 `bundle.loadScene(sceneName, cb)`。

2. `Bundle.loadScene(sceneName)`：
   - 设置 `opts.preset='scene'`、`opts.bundle=this.name`；
   - 调用 `assetManager.loadAny({ scene: sceneName }, ...)`；
   - 成功后校验并回填 `sceneAsset.scene` 的 `id/name`。

3. `AssetManager.loadAny(...)`：
   - 构建 `Task` 后进入 `pipeline.async(task)`；
   - 完成下载、解析与依赖组装。

4. `Director.runSceneImmediate(scene, ...)`：
   - `scene._load()` 初始化节点树；
   - 处理常驻节点并切走旧场景；
   - `releaseManager._autoRelease(oldScene, scene, persistNodes)` 自动释放旧依赖；
   - `scene._activate()` 激活新场景并触发启动后事件。

## 2.2 这条链回答的问题

- 为什么 `loadScene` 成功回调了但新场景节点不可见？
- 为什么切场后出现资源丢失/黑图？
- 为什么常驻节点资源没有按预期释放？

---

## 3. 关键调用链 B：每帧运行与渲染提交链

## 3.1 链路描述

1. `Director.tick(dt)`：
   - `BEGIN_FRAME`；
   - 组件 `start/update/lateUpdate`；
   - 系统 `update(dt)` 与 `postUpdate(dt)`；
   - `BEFORE_DRAW` 后调用 `root.frameMove(dt)`；
   - `AFTER_DRAW` 与 `END_FRAME`。

2. 系统阶段（与功能模块耦合点）：
   - `AnimationManager` 在 `DirectorEvent.INIT` 注册，运行于系统调度；
   - `PhysicsSystem/PhysicsSystem2D` 也在 `INIT` 注册，2D 在 `postUpdate` 中步进并发 `BEFORE_PHYSICS/AFTER_PHYSICS`。

3. `Root.frameMove(dt)`：
   - `_frameMoveBegin()`：清理本帧状态；
   - `_frameMoveProcess()`：收集窗口相机、更新 scene 与 2D batcher；
   - `_frameMoveEnd()`：排序相机、`pipeline.render(cameraList)`、`device.present()`。

## 3.2 这条链回答的问题

- 为什么逻辑在跑但屏幕不更新？
- 为什么物理/动画看起来“慢一拍”或“完全不动”？
- 为什么相机存在但没有最终提交到屏幕？

---

## 4. 问题定位样例（S2-T5 要求）

## 样例1：切场后黑屏（场景链问题）

**现象**：`director.loadScene('X')` 回调已成功，但场景显示异常或资源黑图。  
**定位顺序**：

1. `cocos/game/director.ts`：确认 `loadScene -> runSceneImmediate` 是否走到。  
2. `cocos/asset/asset-manager/bundle.ts`：确认 `Bundle.loadScene` 是否产出有效 `SceneAsset.scene`。  
3. `cocos/scene-graph/scene.ts`：确认 `scene._load()`、`scene._activate()` 是否执行。  
4. `cocos/game/director.ts`：检查 `releaseManager._autoRelease(...)` 是否提前回收被复用资源。  

**结论模板**：优先区分“加载失败”还是“激活后被释放/不可见”。

## 样例2：UI 可交互但不渲染（帧链问题）

**现象**：逻辑事件触发正常，UI 节点层级存在，但画面未提交。  
**定位顺序**：

1. `cocos/game/director.ts`：确认 `tick(dt)` 是否到 `BEFORE_DRAW` 与 `root.frameMove(dt)`。  
2. `cocos/root.ts`：确认 `_frameMoveProcess` 后 `cameraList` 非空。  
3. `cocos/2d/renderer/batcher-2d.ts`：确认 `Batcher2D` 已登记 screen 且相机可见层匹配。  
4. `cocos/root.ts`：确认 `_pipeline.render(cameraList)` 与 `_device.present()` 被调用。  

**结论模板**：优先排除“没有相机/层可见性不匹配/提交流程未执行”。

---

## 5. 阶段2能力验收（对照目标）

- 已完成生命周期、场景资源、渲染主线、功能模块定位四类文档闭环。  
- 已形成两条跨目录关键链路，可支持后续 TS->JSB->C++ 的阶段4链路学习。  
- 已具备首轮故障定位能力（可在 10~15 分钟内锁定问题落点目录）。

---

## 6. 下一步（进入阶段3）

- 建议先做 `S3-T1`：`pal/` 抽象接口分层与 web/native/minigame 分支梳理。  
- 保持同样输出模板：**入口 -> 核心对象 -> 运行时挂点 -> 边界 -> 排障路径**。

状态：已完成（S2-T5）
