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

#include "core/Director.h"
#include "base/Scheduler.h"
#include "base/memory/Memory.h"
#include "core/scene-graph/Scene.h"
#include "core/scene-graph/Node.h"

namespace cc {

Director *Director::s_instance = nullptr;

Director::Director() = default;

Director::~Director() {
    delete _scheduler;
    _scheduler = nullptr;
}

Director *Director::getInstance() {
    if (!s_instance) {
        s_instance = ccnew Director();
    }
    return s_instance;
}

// ===========================================================================
// Main Loop
// ===========================================================================

void Director::tick(float dt) {
    // 1. ComponentScheduler phases - invoke start, update, lateUpdate in order
    //    Note: Scheduler::update(dt) is called by Engine::tick() before this,
    //    to avoid duplicate scheduling.
    _compScheduler.invokeStart();
    _compScheduler.invokeUpdate(dt);
    _compScheduler.invokeLateUpdate(dt);

    // 2. Deferred destruction
    CCObject::deferredDestroy();
}

// ===========================================================================
// Scene Management
// ===========================================================================

void Director::loadScene(const ccstd::string &sceneName,
                          const std::function<void(Scene *)> &onLaunched) {
    // Store loading scene name for reference
    _loadingScene = sceneName;

    // TODO: Implement actual scene loading from AssetManager in M5-S2.
    // For now, this is a placeholder that stores the scene name.
    // The full implementation will:
    // 1. Look up scene asset by name via AssetManager
    // 2. Deserialize the scene
    // 3. Call runScene() with the loaded scene
}

void Director::runScene(Scene *scene,
                         const std::function<void()> &onBeforeLoadScene,
                         const std::function<void()> &onLaunched) {
    // Run scene immediately (no deferred transition in this implementation)
    runSceneImmediate(scene, onBeforeLoadScene, onLaunched);
}

void Director::runSceneImmediate(Scene *scene,
                                  const std::function<void()> &onBeforeLoadScene,
                                  const std::function<void()> &onLaunched) {
    // 1. Before callback
    if (onBeforeLoadScene) {
        onBeforeLoadScene();
    }

    // 2. Scene load - initialize scene hierarchy and walk nodes
    scene->load();

    // 3. Handle persist root nodes - migrate from old scene to new scene
    handlePersistRootNodes(scene);

    // 4. Destroy old scene (after persist nodes have been migrated out)
    destroyOldScene();

    // 5. Set new scene as current
    _scene = scene;

    // 6. Activate the new scene - triggers node/component lifecycle
    scene->activate(true);

    // 7. After callback
    if (onLaunched) {
        onLaunched();
    }
}

// ===========================================================================
// Persist Root Nodes (I-4)
// ===========================================================================

void Director::addPersistRootNode(Node *node) {
    if (!node) return;

    const auto &uuid = node->getUuid();
    if (uuid.empty()) return;

    // Mark node as persist via DONT_DESTROY flag
    node->setPersistNode(true);

    // Store reference in persist map
    _persistRootNodes[uuid] = node;

    // Emit SceneChangedForPersist event so listeners know this node
    // has changed its scene association
    node->emit<Node::SceneChangedForPersist>();
}

void Director::removePersistRootNode(Node *node) {
    if (!node) return;

    const auto &uuid = node->getUuid();
    auto it = _persistRootNodes.find(uuid);
    if (it != _persistRootNodes.end()) {
        // Clear DONT_DESTROY flag
        node->setPersistNode(false);

        // Remove from persist map
        _persistRootNodes.erase(it);

        // Emit RemovePersistRootNode event
        node->emit<Node::RemovePersistRootNode>();
    }
}

bool Director::isPersistRootNode(Node *node) const {
    if (!node) return false;
    return _persistRootNodes.find(node->getUuid()) != _persistRootNodes.end();
}

Node *Director::getPersistRootNode(const ccstd::string &uuid) const {
    auto it = _persistRootNodes.find(uuid);
    if (it != _persistRootNodes.end()) {
        return it->second;
    }
    return nullptr;
}

// ===========================================================================
// Current Scene
// ===========================================================================

Scene *Director::getScene() const {
    return _scene;
}

void Director::setScene(Scene *scene) {
    _scene = scene;
}

// ===========================================================================
// Subsystem Access
// ===========================================================================

Scheduler *Director::getScheduler() const {
    if (!_scheduler) {
        _scheduler = ccnew Scheduler();
    }
    return _scheduler;
}

// ===========================================================================
// Internal Helpers
// ===========================================================================

void Director::handlePersistRootNodes(Scene *newScene) {
    // Migrate persist root nodes from old scene to new scene.
    // Persist nodes should NOT be destroyed with the old scene.
    for (auto &pair : _persistRootNodes) {
        Node *node = pair.second;
        if (!node) continue;

        // If node has a parent in the old scene, remove it
        if (node->getParent()) {
            node->removeFromParent();
        }

        // Add node as child of the new scene
        newScene->addChild(node);

        // Update scene reference for the node and its children
        node->emit<Node::SceneChangedForPersist>();
    }
}

void Director::destroyOldScene() {
    // If there is a current scene, destroy it.
    // Persist root nodes should already be migrated out by handlePersistRootNodes().
    if (_scene) {
        _scene->destroy();
        _scene.reset();
    }
}

} // namespace cc
