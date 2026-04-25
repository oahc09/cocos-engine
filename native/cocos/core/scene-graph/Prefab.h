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
#include "base/Ptr.h"
#include "core/assets/Asset.h"

namespace cc {

class Node;

/**
 * @en Prefab is a template of node tree, which can be instantiated to create nodes.
 * @zh Prefab 是节点树的模板，可以通过实例化来创建节点。
 */
class Prefab : public Asset {
public:
    using Super = Asset;

    enum class OptimizationPolicy : uint8_t {
        AUTO = 0,
        SINGLE_INSTANCE = 1,
        MULTI_INSTANCE = 2,
    };

    Prefab() = default;
    ~Prefab() override;

    /**
     * @en Initialize from binary template data.
     * @zh 从二进制模板数据初始化。
     * @param data Pointer to binary data.
     * @param size Size of binary data in bytes.
     * @return true if initialization succeeded.
     */
    bool initFromBinary(const uint8_t *data, uint32_t size);

    /**
     * @en Create a new node tree from this prefab.
     * @zh 通过此预制件创建新的节点树。
     * @return The instantiated root node, or nullptr on failure.
     */
    Node *instantiate();

    /**
     * @en Get the optimization policy for instantiation.
     * @zh 获取实例化的优化策略。
     */
    inline OptimizationPolicy getOptimizationPolicy() const { return _optimizationPolicy; }

    /**
     * @en Set the optimization policy for instantiation.
     * @zh 设置实例化的优化策略。
     */
    inline void setOptimizationPolicy(OptimizationPolicy policy) { _optimizationPolicy = policy; }

    /**
     * @en Get the number of times this prefab has been instantiated.
     * @zh 获取此预制件已实例化的次数。
     */
    inline uint32_t getInstantiatedTimes() const { return _instantiatedTimes; }

    /**
     * @en Get the data root node (used in editor mode for JS instantiation).
     * @zh 获取数据根节点（编辑器模式下用于 JS 实例化）。
     */
    inline Node *getData() const { return _data.get(); }

    /**
     * @en Set the data root node.
     * @zh 设置数据根节点。
     */
    void setData(Node *data);

private:
    IntrusivePtr<Node> _data;
    OptimizationPolicy _optimizationPolicy{OptimizationPolicy::AUTO};
    uint32_t _instantiatedTimes{0};

    /**
     * @en Binary template for runtime fast instantiation.
     * @zh 运行时快速实例化的二进制模板。
     */
    struct BinaryTemplate {
        ccstd::vector<uint8_t> data;
        uint32_t nodeCount{0};
        uint32_t componentCount{0};
    };
    BinaryTemplate _binaryTemplate;

    /**
     * @en Instantiate from binary template (C++ fast path).
     * @zh 从二进制模板实例化（C++ 快速路径）。
     */
    Node *instantiateFromBinary();

    CC_DISALLOW_COPY_MOVE_ASSIGN(Prefab);
};

} // namespace cc
