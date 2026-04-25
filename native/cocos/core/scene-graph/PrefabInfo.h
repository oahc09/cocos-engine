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

#include "base/Ptr.h"
#include "base/Macros.h"

namespace cc {

class Prefab;
class Node;

/**
 * @en Information about a prefab instance within a node tree.
 * @zh 节点树中预制件实例的信息。
 */
struct PrefabInfo {
    /**
     * @en The prefab asset reference.
     * @zh 预制件资源引用。
     */
    IntrusivePtr<Prefab> asset;

    /**
     * @en The file ID of this prefab instance.
     * @zh 此预制件实例的文件 ID。
     */
    ccstd::string fileId;

    /**
     * @en The info ID of this prefab instance.
     * @zh 此预制件实例的信息 ID。
     */
    uint32_t infoId{0};

    /**
     * @en The root node of the prefab instance.
     * @zh 预制件实例的根节点。
     */
    Node *root{nullptr};

    /**
     * @en Whether this prefab instance has been deleted.
     * @zh 此预制件实例是否已被删除。
     */
    bool isDeleted{false};
};

} // namespace cc
