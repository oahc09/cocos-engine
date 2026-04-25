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

#include "core/serialization/BinarySceneFormat.h"
#include "base/Ptr.h"
#include "core/assets/Asset.h"
#include "base/std/container/string.h"
#include "base/std/container/vector.h"

namespace cc {

class Scene;
class Node;
class CCObject;

struct BinaryDeserializeResult {
    IntrusivePtr<Scene> scene;
    ccstd::vector<IntrusivePtr<Asset>> assets;
    bool success{false};
    ccstd::string errorMessage;
};

class BinaryDeserializer final {
public:
    /**
     * Deserialize a binary scene from raw byte data.
     * @param data Pointer to the binary scene data.
     * @param size Size of the data in bytes.
     * @return Deserialization result containing the scene and assets on success,
     *         or an error message on failure.
     */
    static BinaryDeserializeResult deserialize(const uint8_t *data, uint32_t size);

private:
    BinaryDeserializer() = delete;

    static bool validateHeader(const BinarySceneHeader &header);

    static ccstd::vector<ccstd::string> parseStringTable(const uint8_t *data, uint32_t offset, uint32_t count);

    static Scene *createScene(const uint8_t *data, uint32_t size, const BinarySceneHeader &header,
                              const ccstd::vector<ccstd::string> &strings);

    static Node *createNodeTree(const uint8_t *data, uint32_t offset,
                                const BinarySceneHeader &header,
                                const ccstd::vector<ccstd::string> &strings,
                                ccstd::vector<CCObject *> &instances);

    static void resolveReferences(const ccstd::vector<CCObject *> &instances,
                                  const uint8_t *data, uint32_t assetRefOffset, uint32_t assetRefCount,
                                  const ccstd::vector<ccstd::string> &strings);
};

} // namespace cc
