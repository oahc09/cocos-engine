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

#include "core/serialization/BinaryDeserializer.h"
#include "core/assets/AssetManager.h"
#include "core/assets/Asset.h"
#include "core/scene-graph/Scene.h"
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

ccstd::vector<uint8_t> makeMinimalBinaryScene(const ccstd::string &nodeName = "RootNode",
                                              const ccstd::string &sceneName = "BinaryScene",
                                              bool autoReleaseAssets = false) {
    ccstd::vector<uint8_t> buffer;
    buffer.resize(sizeof(BinarySceneHeader), 0);

    SceneEntry sceneEntry{};
    sceneEntry.nameStringIndex = 0;
    sceneEntry.autoReleaseAssets = autoReleaseAssets ? 1 : 0;
    appendBytes(buffer, sceneEntry);

    const uint32_t stringTableOffset = static_cast<uint32_t>(buffer.size());
    const uint32_t stringCount = 2;
    appendBytes(buffer, stringCount);

    StringTableEntry sceneNameEntry{};
    sceneNameEntry.offset = 0;
    sceneNameEntry.length = static_cast<uint32_t>(sceneName.size());
    appendBytes(buffer, sceneNameEntry);

    StringTableEntry nodeNameEntry{};
    nodeNameEntry.offset = sceneNameEntry.length;
    nodeNameEntry.length = static_cast<uint32_t>(nodeName.size());
    appendBytes(buffer, nodeNameEntry);

    buffer.insert(buffer.end(), sceneName.begin(), sceneName.end());
    buffer.insert(buffer.end(), nodeName.begin(), nodeName.end());

    const uint32_t rootNodeOffset = static_cast<uint32_t>(buffer.size());

    NodeEntry nodeEntry{};
    nodeEntry.instanceIndex = 0;
    nodeEntry.childCount = 0;
    nodeEntry.componentCount = 0;
    nodeEntry.nameStringIndex = 1;
    nodeEntry.layer = 7;
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

ccstd::vector<uint8_t> makeBinarySceneWithFooterAndAssetRef(const ccstd::string &assetUuid) {
    auto buffer = makeMinimalBinaryScene("RootWithAssetRef", "SceneWithAssetRef");

    const uint32_t stringCount = 2;
    BinarySceneHeader *header = reinterpret_cast<BinarySceneHeader *>(buffer.data());
    const uint32_t stringTableOffset = header->stringTableOffset;

    ccstd::vector<uint8_t> rebuilt;
    rebuilt.resize(sizeof(BinarySceneHeader), 0);

    SceneEntry sceneEntry{};
    sceneEntry.nameStringIndex = 0;
    sceneEntry.autoReleaseAssets = 0;
    appendBytes(rebuilt, sceneEntry);

    appendBytes(rebuilt, stringCount);

    StringTableEntry rootEntry{};
    rootEntry.offset = 0;
    rootEntry.length = static_cast<uint32_t>(ccstd::string("RootWithAssetRef").size());
    appendBytes(rebuilt, rootEntry);

    StringTableEntry assetEntry{};
    assetEntry.offset = rootEntry.length;
    assetEntry.length = static_cast<uint32_t>(assetUuid.size());
    appendBytes(rebuilt, assetEntry);

    const ccstd::string rootName = "RootWithAssetRef";
    rebuilt.insert(rebuilt.end(), rootName.begin(), rootName.end());
    rebuilt.insert(rebuilt.end(), assetUuid.begin(), assetUuid.end());

    const uint32_t rootNodeOffset = static_cast<uint32_t>(rebuilt.size());
    NodeEntry nodeEntry{};
    nodeEntry.instanceIndex = 0;
    nodeEntry.childCount = 0;
    nodeEntry.componentCount = 0;
    nodeEntry.nameStringIndex = 0;
    nodeEntry.layer = 1;
    nodeEntry.lsfIndex = 0;
    appendBytes(rebuilt, nodeEntry);

    const float transformData[10] = {
        0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F,
        1.0F, 1.0F, 1.0F,
    };
    const auto *transformRaw = reinterpret_cast<const uint8_t *>(transformData);
    rebuilt.insert(rebuilt.end(), transformRaw, transformRaw + sizeof(transformData));

    const uint32_t assetRefOffset = static_cast<uint32_t>(rebuilt.size());
    AssetRefEntry ref{};
    ref.ownerInstanceIndex = 0;
    ref.assetPathStringIndex = 1;
    ref.assetType = 0;
    ref.propertyIndex = 0;
    appendBytes(rebuilt, ref);

    BinarySceneFooter footer{};
    footer.assetRefTableOffset = assetRefOffset;
    footer.assetRefCount = 1;
    footer.checksum = 0;
    appendBytes(rebuilt, footer);

    BinarySceneHeader finalHeader{};
    std::memcpy(finalHeader.magic, "CCSC", 4);
    finalHeader.version = BINARY_SCENE_VERSION;
    finalHeader.flags = 0;
    finalHeader.stringTableOffset = stringTableOffset;
    finalHeader.instanceTableOffset = 0;
    finalHeader.rootNodeOffset = rootNodeOffset;
    finalHeader.totalSize = static_cast<uint32_t>(rebuilt.size());
    std::memcpy(rebuilt.data(), &finalHeader, sizeof(finalHeader));
    return rebuilt;
}

TEST(BinaryDeserializerTest, rejectsBadMagicWithExplicitError) {
    auto data = makeMinimalBinaryScene();
    data[0] = 'B';

    const auto result = BinaryDeserializer::deserialize(data.data(), static_cast<uint32_t>(data.size()));

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.scene, nullptr);
    EXPECT_NE(result.errorMessage.find("bad magic"), ccstd::string::npos);
}

TEST(BinaryDeserializerTest, rejectsUnsupportedVersionWithExplicitError) {
    auto data = makeMinimalBinaryScene();
    auto *header = reinterpret_cast<BinarySceneHeader *>(data.data());
    header->version = static_cast<uint16_t>(BINARY_SCENE_VERSION + 1);

    const auto result = BinaryDeserializer::deserialize(data.data(), static_cast<uint32_t>(data.size()));

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.scene, nullptr);
    EXPECT_NE(result.errorMessage.find("unsupported version"), ccstd::string::npos);
}

TEST(BinaryDeserializerTest, rejectsSceneWithInvalidRootOffset) {
    auto data = makeMinimalBinaryScene();
    auto *header = reinterpret_cast<BinarySceneHeader *>(data.data());
    header->rootNodeOffset = header->totalSize;

    const auto result = BinaryDeserializer::deserialize(data.data(), static_cast<uint32_t>(data.size()));

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.scene, nullptr);
    EXPECT_NE(result.errorMessage.find("root node"), ccstd::string::npos);
}

TEST(BinaryDeserializerTest, createsMinimalSceneWithRootNode) {
    const auto data = makeMinimalBinaryScene("BinaryRoot", "BinaryScene", true);

    const auto result = BinaryDeserializer::deserialize(data.data(), static_cast<uint32_t>(data.size()));

    ASSERT_TRUE(result.success);
    ASSERT_NE(result.scene, nullptr);
    EXPECT_TRUE(result.errorMessage.empty());
    EXPECT_TRUE(result.assets.empty());
    EXPECT_EQ(result.scene->getName(), ccstd::string("BinaryScene"));
    EXPECT_TRUE(result.scene->isAutoReleaseAssets());
    ASSERT_EQ(result.scene->getChildren().size(), 1U);

    const auto &root = result.scene->getChildren().front();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->getName(), ccstd::string("BinaryRoot"));
    EXPECT_EQ(root->getLayer(), 7U);
    EXPECT_EQ(root->getPosition(), (Vec3(1.0F, 2.0F, 3.0F)));
    EXPECT_TRUE(root->getRotation().approxEquals(Quaternion(0.0F, 0.0F, 0.0F, 1.0F)));
    EXPECT_EQ(root->getScale(), (Vec3(4.0F, 5.0F, 6.0F)));
}

TEST(BinaryDeserializerTest, defaultsScenePropertiesWhenSceneEntryMissing) {
    auto data = makeMinimalBinaryScene("LegacyRoot", "LegacyScene", true);
    BinarySceneHeader *header = reinterpret_cast<BinarySceneHeader *>(data.data());

    // Remove the SceneEntry gap to simulate older payloads that start strings right after the header.
    const uint32_t sceneEntryBytes = sizeof(SceneEntry);
    const uint32_t oldStringTableOffset = header->stringTableOffset;
    data.erase(data.begin() + sizeof(BinarySceneHeader),
               data.begin() + sizeof(BinarySceneHeader) + sceneEntryBytes);

    header = reinterpret_cast<BinarySceneHeader *>(data.data());
    header->stringTableOffset = oldStringTableOffset - sceneEntryBytes;
    header->rootNodeOffset -= sceneEntryBytes;
    header->totalSize = static_cast<uint32_t>(data.size());

    const auto result = BinaryDeserializer::deserialize(data.data(), static_cast<uint32_t>(data.size()));

    ASSERT_TRUE(result.success);
    ASSERT_NE(result.scene, nullptr);
    EXPECT_EQ(result.scene->getName(), ccstd::string("New Node"));
    EXPECT_FALSE(result.scene->isAutoReleaseAssets());
}

TEST(BinaryDeserializerTest, collectsResolvedAssetsFromReferenceFooter) {
    AssetManager::getInstance().clearCache();

    auto *asset = new Asset();
    asset->setUuid("asset://cached-texture");
    AssetManager::getInstance().cacheAsset("asset://cached-texture", asset);

    const auto data = makeBinarySceneWithFooterAndAssetRef("asset://cached-texture");
    const auto result = BinaryDeserializer::deserialize(data.data(), static_cast<uint32_t>(data.size()));

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.assets.size(), 1U);
    EXPECT_EQ(result.assets.front().get(), asset);

    AssetManager::getInstance().clearCache();
}

} // namespace
