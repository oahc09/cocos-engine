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

#include "core/components/NodeActivator.h"
#include "core/Director.h"
#include "core/components/Component.h"
#include "core/data/Object.h"
#include "core/scene-graph/Node.h"

namespace cc {

void NodeActivator::activateNode(Node *node, bool active) {
    if (active) {
        activateNodeRecursively(node);
    } else {
        deactivateNodeRecursively(node);
    }
}

void NodeActivator::activateNodeRecursively(Node *node) {
    // 1. Set _activeInHierarchy = true
    node->setActiveInHierarchy(true);
    node->emit<Node::ActiveInHierarchyChanged>();

    // 2. Iterate components and activate each
    const auto &components = node->getComponents();
    for (auto *comp : components) {
        activateComp(comp, true);
    }

    // 3. Recurse into active children
    const auto &children = node->getChildren();
    for (const auto &child : children) {
        if (child && child->isActive()) {
            activateNodeRecursively(child.get());
        }
    }
}

void NodeActivator::deactivateNodeRecursively(Node *node) {
    // 1. Set Deactivating flag (prevents child reparenting during deactivation)
    node->_objFlags |= CCObject::Flags::DEACTIVATING;

    // 2. Set _activeInHierarchy = false
    node->setActiveInHierarchy(false);
    node->emit<Node::ActiveInHierarchyChanged>();

    // 3. Recurse into children first (depth-first deactivation)
    const auto &children = node->getChildren();
    for (const auto &child : children) {
        if (child && child->isActiveInHierarchy()) {
            deactivateNodeRecursively(child.get());
        }
    }

    // 4. Deactivate components in reverse order (bottom-up)
    const auto &components = node->getComponents();
    for (auto it = components.rbegin(); it != components.rend(); ++it) {
        activateComp(*it, false);
    }

    // 5. Clear Deactivating flag
    node->_objFlags &= ~CCObject::Flags::DEACTIVATING;
}

void NodeActivator::activateComp(Component *comp, bool active) {
    if (active) {
        if (comp->_enabled && !comp->_enabledInHierarchy) {
            comp->_enabledInHierarchy = true;
            if (!comp->_preloaded) {
                comp->__preload();
                comp->_preloaded = true;
            }
            if (!comp->_loaded) {
                comp->onLoad();
                comp->_loaded = true;
            }
            comp->onEnable();
            Director::getInstance()->getCompScheduler()->registerComponent(comp);
        }
    } else {
        if (comp->_enabledInHierarchy) {
            comp->_enabledInHierarchy = false;
            Director::getInstance()->getCompScheduler()->unregisterComponent(comp);
            comp->onDisable();
        }
    }
}

void NodeActivator::activateComponent(Component *comp, bool active) {
    activateComp(comp, active);
}

} // namespace cc
