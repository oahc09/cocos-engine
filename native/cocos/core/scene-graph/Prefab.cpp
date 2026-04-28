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

#include "core/scene-graph/Prefab.h"
#include "core/scene-graph/Node.h"
#include "core/scene-graph/Scene.h"
#include "core/platform/Debug.h"
#include "core/serialization/BinaryDeserializer.h"

namespace cc {

Prefab::~Prefab() = default;

bool Prefab::initFromBinary(const uint8_t *data, uint32_t size) {
    // The binary format is defined in AI/design-cpp-scene-system.md §4.2.
    if (data == nullptr || size == 0) {
        CC_LOG_WARNING("Prefab::initFromBinary: invalid data or size is 0");
        return false;
    }

    _binaryTemplate.data.assign(data, data + size);
    _binaryTemplate.nodeCount = 0;
    _binaryTemplate.componentCount = 0;

    CC_LOG_DEBUG("Prefab::initFromBinary: stored %u bytes for binary instantiation", size);
    return true;
}

Node *Prefab::instantiate() {
    ++_instantiatedTimes;

    // Choose instantiation strategy based on optimization policy
    switch (_optimizationPolicy) {
        case OptimizationPolicy::SINGLE_INSTANCE:
            return instantiateFromBinary();
        case OptimizationPolicy::MULTI_INSTANCE:
            return instantiateFromBinary();
        case OptimizationPolicy::AUTO:
            // AUTO: use binary path when available, otherwise fall back to data clone
            if (!_binaryTemplate.data.empty()) {
                return instantiateFromBinary();
            }
            break;
    }

    // Fallback: clone from data node (editor / JS path)
    if (_data != nullptr) {
        // TODO: Node::instantiate static method for deep clone
        // For now, return nullptr as this path requires JS bridge
        CC_LOG_WARNING("Prefab::instantiate: data node clone not yet implemented in C++, use JS path");
        return nullptr;
    }

    CC_LOG_WARNING("Prefab::instantiate: no binary template or data node available");
    return nullptr;
}

Node *Prefab::instantiateFromBinary() {
    if (_binaryTemplate.data.empty()) {
        CC_LOG_WARNING("Prefab::instantiateFromBinary: binary template is empty");
        return nullptr;
    }

    const auto result = BinaryDeserializer::deserialize(_binaryTemplate.data.data(),
                                                        static_cast<uint32_t>(_binaryTemplate.data.size()));
    if (!result.success || result.scene == nullptr) {
        CC_LOG_WARNING("Prefab::instantiateFromBinary: deserialize failed: %s", result.errorMessage.c_str());
        return nullptr;
    }

    const auto &children = result.scene->getChildren();
    if (children.size() != 1U || children.front() == nullptr) {
        CC_LOG_WARNING("Prefab::instantiateFromBinary: expected exactly one root child, got %u",
                       static_cast<uint32_t>(children.size()));
        return nullptr;
    }

    Node *instance = children.front();
    instance->addRef();
    instance->removeFromParent();
    return instance;
}

void Prefab::setData(Node *data) {
    _data = data;
}

} // namespace cc
