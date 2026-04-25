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

namespace cc {

class Node;
class Component;

/**
 * @en NodeActivator handles activation/deactivation of nodes and their components.
 * Manages recursive activation of child nodes, sets _activeInHierarchy,
 * and triggers component lifecycle methods (__preload, onLoad, onEnable, onDisable).
 * @zh 节点激活器，处理节点及其组件的激活/停用。
 * 递归激活子节点，设置 _activeInHierarchy，
 * 触发组件生命周期方法（__preload、onLoad、onEnable、onDisable）。
 */
class NodeActivator final {
public:
    NodeActivator() = default;
    ~NodeActivator() = default;

    /**
     * @brief Activate or deactivate a node and all its active children recursively.
     * For activation: sets _activeInHierarchy=true, activates components, recurses into active children.
     * For deactivation: sets Deactivating flag, sets _activeInHierarchy=false,
     *                    recurses children first, then deactivates components bottom-up.
     * @param node The target node to activate/deactivate.
     * @param active True to activate, false to deactivate.
     */
    void activateNode(Node *node, bool active);

    /**
     * @brief Activate or deactivate a single component independently.
     * Used when a component is added to an already-active node at runtime.
     * @param comp The component to activate/deactivate.
     * @param active True to activate, false to deactivate.
     */
    void activateComponent(Component *comp, bool active);

private:
    /**
     * @brief Recursively activate a node and its active children.
     * Sets _activeInHierarchy, calls component lifecycle, recurses into active children.
     */
    void activateNodeRecursively(Node *node);

    /**
     * @brief Recursively deactivate a node and its children.
     * Sets Deactivating flag and _activeInHierarchy=false, recurses children first,
     * then deactivates components bottom-up.
     */
    void deactivateNodeRecursively(Node *node);

    /**
     * @brief Activate or deactivate a single component.
     * For activation: calls __preload, onLoad, onEnable (if component is enabled).
     * For deactivation: calls onDisable (if component was enabledInHierarchy).
     */
    void activateComp(Component *comp, bool active);
};

} // namespace cc
