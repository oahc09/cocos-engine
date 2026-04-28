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
#include "base/std/container/unordered_set.h"
#include "core/assets/AssetManager.h"
#include "core/assets/ReleaseManager.h"
#include "core/assets/SceneAsset.h"
#include "core/scene-graph/Scene.h"
#include "core/scene-graph/Node.h"
#include "core/components/ScriptComponent.h"
#include "core/scripting/ScriptBridge.h"
#include "base/Log.h"

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
    // ── Phase A4: Single Responsibility Clarification ──
    // Director::tick() is responsible for:
    //   1. Component lifecycle phases (start → update → lateUpdate)
    //   2. Deferred destruction of CCObjects
    //
    // Scheduler::update(dt) is called SEPARATELY by Engine::tick() BEFORE
    // Director::tick(). This split is intentional:
    //   - Engine::tick() drives: Scheduler (timers, tweens) → Director::tick()
    //   - Director::tick() drives: ComponentScheduler + deferred destroy
    //
    // Future: If Director should become the single entry point, Scheduler::update
    // would move here. For now, the split avoids duplicate scheduling with the
    // existing Engine::tick() path. See design-cpp-master-spec.md §1.5.

    // 1. ComponentScheduler phases - invoke start, update, lateUpdate in order
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

    if (sceneName.empty()) {
        if (onLaunched) {
            onLaunched(nullptr);
        }
        return;
    }

    auto &assetManager = AssetManager::getInstance();
    auto sceneAsset = assetManager.loadSceneAsset(sceneName);
    Scene *scene = sceneAsset ? sceneAsset->getScene() : nullptr;
    if (scene == nullptr) {
        CC_LOG_WARNING("Director::loadScene - failed to resolve scene '%s'", sceneName.c_str());
        if (onLaunched) {
            onLaunched(nullptr);
        }
        return;
    }

    runSceneImmediate(scene, nullptr, [onLaunched, scene]() {
        if (onLaunched) {
            onLaunched(scene);
        }
    });
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
    if (!_scene) return;

    // ── Phase D2: Scene-switch release strategy ──
    // Persist root nodes have already been migrated to the new scene by
    // handlePersistRootNodes(), so they won't appear in the walk below.
    //
    // Walk all remaining nodes in the old scene, collect asset references
    // from their components, and decRef each unique asset. After scene
    // destruction, autoRelease() will free assets with ref count == 0.

    // 1. Collect all unique asset UUIDs referenced by old scene components
    ccstd::unordered_set<ccstd::string> assetUuids;

    _scene->walk([&](Node *node) {
        for (Component *comp : node->getComponents()) {
            // Builtin components: getAssetProperties() returns C++ side refs
            auto refs = comp->getAssetProperties();
            for (Asset *asset : refs) {
                if (asset && !asset->getUuid().empty()) {
                    assetUuids.insert(asset->getUuid());
                }
            }

            // Script components: collect asset refs through ScriptBridge (JS side)
            auto *scriptComp = dynamic_cast<ScriptComponent *>(comp);
            if (scriptComp != nullptr && scriptComp->getScriptBridgeCompId() != 0) {
                auto scriptRefs = ScriptBridge::getInstance().collectAssetRefs(
                    scriptComp->getScriptBridgeCompId());
                for (Asset *asset : scriptRefs) {
                    if (asset && !asset->getUuid().empty()) {
                        assetUuids.insert(asset->getUuid());
                    }
                }
            }
        }
    });

    // 2. decRef all collected assets
    auto &releaseMgr = ReleaseManager::getInstance();
    for (const auto &uuid : assetUuids) {
        releaseMgr.decRef(uuid);
    }

    // 3. Destroy old scene (persist nodes already migrated out)
    _scene->destroy();
    _scene.reset();

    // 4. Auto-release assets with zero ref count
    releaseMgr.autoRelease();
}

} // namespace cc
