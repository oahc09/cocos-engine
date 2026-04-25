/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
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

namespace cc {

class Scene;

/**
 * @en Utility functions for Prefab operations in the scene graph.
 * @zh 场景图中预制件操作的实用工具函数。
 */
class PrefabUtils final {
public:
    /**
     * @en Expand nested prefab instance nodes within the scene.
     * Replaces prefab instance placeholders with actual prefab content.
     * @zh 展开场景中嵌套的预制件实例节点。
     * 将预制件实例占位符替换为实际的预制件内容。
     * @param scene The scene to process.
     */
    static void expandNestedPrefabInstanceNode(Scene *scene);

    /**
     * @en Apply target overrides to prefab instances in the scene.
     * Propagates property modifications from the prefab instance to its children.
     * @zh 对场景中的预制件实例应用目标覆盖。
     * 将预制件实例的属性修改传播到其子节点。
     * @param scene The scene to process.
     */
    static void applyTargetOverrides(Scene *scene);

private:
    PrefabUtils() = delete;
    ~PrefabUtils() = delete;
    CC_DISALLOW_COPY_MOVE_ASSIGN(PrefabUtils);
};

} // namespace cc
