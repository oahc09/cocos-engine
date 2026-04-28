/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated engine source code (the "Software"), a limited,
 worldwide, royalty-free, non-assignable, revocable and non-exclusive license
 to use Cocos Creator solely to develop games on your target platforms. You shall
 not use Cocos Creator software for developing other software or tools that's
 used for developing games. You are not granted to publish, distribute,
 sublicense, and/or sell copies of Cocos Creator.

 The software or tools in this License Agreement are licensed, not sold.
 Xiamen Yaji Software Co., Ltd. reserves all rights not expressly granted to you.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/

#include "core/scene-graph/Node.h"
#include "core/scene-graph/Prefab.h"
#include "core/serialization/BinaryDeserializer.h"
#include "math/Quaternion.h"
#include "math/Vec3.h"

#include "gtest/gtest.h"

#include <cstring>

using namespace cc;

namespace {

template <class T>
void appendBytes(ccstd::vector<uint8_t> &buffer, const T &value) {
    const auto *raw = reinterpret_cast<const uint8_t *>(&value);
    buffer.insert(buffer.end(), raw, raw + sizeof(T));
}

ccstd::vector<uint8_t> makeMinimalBinaryScene(const ccstd::string &nodeName = "PrefabRoot") {
    ccstd::vector<uint8_t> buffer;
    buffer.resize(sizeof(BinarySceneHeader), 0);

    const uint32_t stringTableOffset = static_cast<uint32_t>(buffer.size());
    const uint32_t stringCount = 1;
    appendBytes(buffer, stringCount);

    StringTableEntry nameEntry{};
    nameEntry.offset = 0;
    nameEntry.length = static_cast<uint32_t>(nodeName.size());
    appendBytes(buffer, nameEntry);
    buffer.insert(buffer.end(), nodeName.begin(), nodeName.end());

    const uint32_t rootNodeOffset = static_cast<uint32_t>(buffer.size());

    NodeEntry nodeEntry{};
    nodeEntry.instanceIndex = 0;
    nodeEntry.childCount = 0;
    nodeEntry.componentCount = 0;
    nodeEntry.nameStringIndex = 0;
    nodeEntry.layer = 9;
    nodeEntry.lsfIndex = 0;
    appendBytes(buffer, nodeEntry);

    const float transformData[10] = {
        1.0F, 2.0F, 3.0F,
        0.0F, 0.0F, 0.0F, 1.0F,
        4.0F, 5.0F, 6.0F,
    };
    const auto *transformRaw = reinterpret_cast<const uint8_t *>(transformData);
    buffer.insert(buffer.end(), transformRaw, transformRaw + sizeof(transformData));

    BinarySceneHeader header{};
    std::memcpy(header.magic, "CCSC", 4);
    header.version = BINARY_SCENE_VERSION;
    header.flags = 0;
    header.stringTableOffset = stringTableOffset;
    header.instanceTableOffset = 0;
    header.rootNodeOffset = rootNodeOffset;
    header.totalSize = static_cast<uint32_t>(buffer.size());

    std::memcpy(buffer.data(), &header, sizeof(header));
    return buffer;
}

TEST(PrefabTest, instantiateUsesBinaryTemplateWhenSceneHasSingleRootChild) {
    Prefab prefab;
    const auto data = makeMinimalBinaryScene("PrefabBinaryRoot");

    ASSERT_TRUE(prefab.initFromBinary(data.data(), static_cast<uint32_t>(data.size())));

    Node *instance = prefab.instantiate();

    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->getParent(), nullptr);
    EXPECT_EQ(instance->getName(), ccstd::string("PrefabBinaryRoot"));
    EXPECT_EQ(instance->getLayer(), 9U);
    EXPECT_EQ(instance->getPosition(), (Vec3(1.0F, 2.0F, 3.0F)));
    EXPECT_TRUE(instance->getRotation().approxEquals(Quaternion(0.0F, 0.0F, 0.0F, 1.0F)));
    EXPECT_EQ(instance->getScale(), (Vec3(4.0F, 5.0F, 6.0F)));
    instance->release();
}

TEST(PrefabTest, instantiateReturnsNullWhenBinaryTemplateCannotDeserialize) {
    Prefab prefab;
    auto data = makeMinimalBinaryScene();
    data[0] = 'B';

    ASSERT_TRUE(prefab.initFromBinary(data.data(), static_cast<uint32_t>(data.size())));

    Node *instance = prefab.instantiate();

    EXPECT_EQ(instance, nullptr);
}

TEST(PrefabTest, initFromBinaryRejectsEmptyPayload) {
    Prefab prefab;

    EXPECT_FALSE(prefab.initFromBinary(nullptr, 0));
    EXPECT_EQ(prefab.instantiate(), nullptr);
}

} // namespace
