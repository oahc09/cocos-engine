/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#pragma once

#include "base/Macros.h"
#include "base/memory/Memory.h"
#include "base/Ptr.h"
#include "base/std/container/string.h"
#include "base/std/container/unordered_map.h"
#include "core/components/NodeActivator.h"
#include "core/components/ComponentScheduler.h"
#include <functional>

namespace cc {

class Scene;
class Node;
class Scheduler;

/**
 * @en Director manages scene lifecycle, node activation, and the main loop.
 * @zh Director 管理场景生命周期、节点激活和主循环。
 *
 * Full implementation per design-cpp-scene-system.md §6.
 * M3-S1b: minimal Director with NodeActivator.
 * M5-S1: complete Director with tick, scene management, persist root nodes, subsystem access.
 */
class Director final {
public:
    static Director *getInstance();

    // === Main Loop ===

    /**
     * @en Main loop tick. Called every frame with delta time.
     * Updates scheduler, invokes component lifecycle phases (start/update/lateUpdate),
     * and processes deferred destruction.
     * @zh 主循环 tick。每帧调用，传入帧间隔时间。
     * 更新调度器，调用组件生命周期阶段（start/update/lateUpdate），处理延迟销毁。
     * @param dt Delta time in seconds since last frame.
     */
    void tick(float dt);

    // === Scene Management ===

    /**
     * @en Load a scene by name.
     * @zh 通过名称加载场景。
     * @param sceneName Name of the scene to load.
     * @param onLaunched Callback when scene has launched.
     */
    void loadScene(const ccstd::string &sceneName,
                   const std::function<void(Scene *)> &onLaunched = nullptr);

    /**
     * @en Run a scene. This will transition from the current scene to the new one.
     * @zh 运行场景。从当前场景切换到新场景。
     * @param scene The scene to run.
     * @param onBeforeLoadScene Callback before scene loading.
     * @param onLaunched Callback when scene has launched.
     */
    void runScene(Scene *scene,
                  const std::function<void()> &onBeforeLoadScene = nullptr,
                  const std::function<void()> &onLaunched = nullptr);

    /**
     * @en Run a scene immediately (no deferred transition).
     * @zh 立即运行场景（无延迟切换）。
     * @param scene The scene to run.
     * @param onBeforeLoadScene Callback before scene loading.
     * @param onLaunched Callback when scene has launched.
     */
    void runSceneImmediate(Scene *scene,
                           const std::function<void()> &onBeforeLoadScene = nullptr,
                           const std::function<void()> &onLaunched = nullptr);

    // === Persist Root Nodes (I-4) ===

    /**
     * @en Mark a node as persist root node. It will survive scene transitions.
     * @zh 将节点标记为常驻根节点，场景切换时不销毁。
     * @param node The node to make persistent.
     */
    void addPersistRootNode(Node *node);

    /**
     * @en Remove a node from persist root nodes. It becomes a normal node again.
     * @zh 取消节点的常驻根节点标记，恢复为普通节点。
     * @param node The node to remove from persist root nodes.
     */
    void removePersistRootNode(Node *node);

    /**
     * @en Check if a node is a persist root node.
     * @zh 检查节点是否是常驻根节点。
     * @param node The node to check.
     * @return True if the node is a persist root node.
     */
    bool isPersistRootNode(Node *node) const;

    /**
     * @en Get a persist root node by its uuid.
     * @zh 通过 uuid 获取常驻根节点。
     * @param uuid The uuid of the persist root node.
     * @return The persist root node, or nullptr if not found.
     */
    Node *getPersistRootNode(const ccstd::string &uuid) const;

    // === Current Scene ===

    /**
     * @en Get the current running scene.
     * @zh 获取当前运行的场景。
     */
    Scene *getScene() const;

    /**
     * @en Set the current running scene.
     * @zh 设置当前运行的场景。
     */
    void setScene(Scene *scene);

    // === Subsystem Access ===

    /**
     * @en Get the node activator for managing node/component lifecycle.
     * @zh 获取节点激活器，用于管理节点/组件生命周期。
     */
    NodeActivator *getNodeActivator() { return &_nodeActivator; }

    /**
     * @en Get the component scheduler for managing component execution order.
     * @zh 获取组件调度器，用于管理组件执行顺序。
     */
    ComponentScheduler *getCompScheduler() { return &_compScheduler; }

    /**
     * @en Get the scheduler for timer and callback management. Lazy initialized.
     * @zh 获取调度器，用于定时器和回调管理。延迟初始化。
     */
    Scheduler *getScheduler() const;

    // === Events ===

    static constexpr const char *BEFORE_SCENE_LAUNCH_EVENT = "before-scene-launch";
    static constexpr const char *AFTER_SCENE_LAUNCH_EVENT  = "after-scene-launch";

private:
    Director();
    ~Director();

    /**
     * @brief Move persist root nodes from old scene to new scene during scene transition.
     * @param newScene The scene being transitioned to.
     */
    void handlePersistRootNodes(Scene *newScene);

    /**
     * @brief Destroy the previous scene during scene transition.
     */
    void destroyOldScene();

    // Subsystems
    NodeActivator _nodeActivator;
    ComponentScheduler _compScheduler;
    mutable Scheduler *_scheduler{nullptr}; // lazy init

    // Scene state
    IntrusivePtr<Scene> _scene;
    ccstd::string _loadingScene;

    // Persist root nodes (I-4): uuid -> Node
    ccstd::unordered_map<ccstd::string, IntrusivePtr<Node>> _persistRootNodes;

    static Director *s_instance;

    CC_DISALLOW_COPY_MOVE_ASSIGN(Director);
};

} // namespace cc
