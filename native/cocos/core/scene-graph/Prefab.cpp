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
#include "core/platform/Debug.h"

namespace cc {

Prefab::~Prefab() = default;

bool Prefab::initFromBinary(const uint8_t *data, uint32_t size) {
    // TODO(M4): Full binary deserialization of Prefab template.
    // Currently stub — will be implemented by M4-S1 BinaryDeserializer.
    // The binary format is defined in AI/design-cpp-scene-system.md §4.2.
    if (data == nullptr || size == 0) {
        CC_LOG_WARNING("Prefab::initFromBinary: invalid data or size is 0");
        return false;
    }

    // Stub: store raw binary data for later processing
    _binaryTemplate.data.assign(data, data + size);
    _binaryTemplate.nodeCount = 0;
    _binaryTemplate.componentCount = 0;

    CC_LOG_DEBUG("Prefab::initFromBinary: stub — stored %u bytes, awaiting M4 deserializer", size);
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
    // TODO(M4): Full binary template instantiation.
    // Will use BinaryDeserializer to create node tree from _binaryTemplate.data.
    // Currently stub — returns nullptr.
    CC_LOG_DEBUG("Prefab::instantiateFromBinary: stub — awaiting M4 BinaryDeserializer");
    return nullptr;
}

void Prefab::setData(Node *data) {
    _data = data;
}

} // namespace cc
