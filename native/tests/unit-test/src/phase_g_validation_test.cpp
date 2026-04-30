/****************************************************************************
Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

http://www.cocos2d-x.org

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

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

#include "core/Director.h"
#include "core/assets/Asset.h"
#include "core/assets/AssetManager.h"
#include "core/assets/AssetRefManager.h"
#include "core/assets/SceneAsset.h"
#include "core/scene-graph/Node.h"
#include "core/scene-graph/Prefab.h"
#include "core/scene-graph/Scene.h"
#include "core/serialization/BinaryDeserializer.h"
#include "math/Quaternion.h"
#include "math/Vec3.h"
#include "gtest/gtest.h"

#include <chrono>
#include <cstring>

using namespace cc;

namespace {

template <class T>
void appendBytes(ccstd::vector<uint8_t> &buffer, const T &value) {
    const auto *raw = reinterpret_cast<const uint8_t *>(&value);
    buffer.insert(buffer.end(), raw, raw + sizeof(T));
}

ccstd::vector<uint8_t> makeMinimalBinaryScene(const ccstd::string &nodeName = "PhaseGPrefabRoot") {
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
    nodeEntry.layer = 1;
    nodeEntry.lsfIndex = 0;
    appendBytes(buffer, nodeEntry);

    const float transformData[10] = {
        0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F,
        1.0F, 1.0F, 1.0F,
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

void resetDirectorAndAssets() {
    auto *director = Director::getInstance();
    director->getCompScheduler()->clear();
    director->setScene(nullptr);
    AssetManager::getInstance().clearCache();
}

TEST(PhaseGValidationTest, loadSceneWith1000Nodes) {
    resetDirectorAndAssets();

    auto *scene = new Scene("LargeScene");
    for (int i = 0; i < 1000; ++i) {
        auto *node = new Node("N");
        scene->addChild(node);
    }

    auto *sceneAsset = new SceneAsset();
    sceneAsset->setScene(scene);
    AssetManager::getInstance().cacheAsset("scene/large", sceneAsset);

    Scene *loaded = nullptr;
    const auto begin = std::chrono::steady_clock::now();
    Director::getInstance()->loadScene("scene/large", [&loaded](Scene *s) {
        loaded = s;
    });
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - begin)
                               .count();

    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->getChildren().size(), 1000U);
    // Keep this threshold loose for CI stability; perf target stays in benchmark stage.
    EXPECT_LT(elapsedMs, 1000);
}

TEST(PhaseGValidationTest, sceneLoadUnload100Cycles) {
    resetDirectorAndAssets();

    auto *sceneA = new Scene("CycleA");
    auto *sceneB = new Scene("CycleB");
    auto *assetA = new SceneAsset();
    auto *assetB = new SceneAsset();
    assetA->setScene(sceneA);
    assetB->setScene(sceneB);
    AssetManager::getInstance().cacheAsset("scene/cycle-a", assetA);
    AssetManager::getInstance().cacheAsset("scene/cycle-b", assetB);

    for (int i = 0; i < 100; ++i) {
        const char *sceneName = (i % 2 == 0) ? "scene/cycle-a" : "scene/cycle-b";
        Scene *loaded = nullptr;
        Director::getInstance()->loadScene(sceneName, [&loaded](Scene *s) {
            loaded = s;
        });
        ASSERT_NE(loaded, nullptr);
        ASSERT_NE(Director::getInstance()->getScene(), nullptr);
    }
}

TEST(PhaseGValidationTest, prefabInstantiateDestroy1000Cycles) {
    Prefab prefab;
    const auto data = makeMinimalBinaryScene();
    ASSERT_TRUE(prefab.initFromBinary(data.data(), static_cast<uint32_t>(data.size())));

    for (int i = 0; i < 1000; ++i) {
        Node *instance = prefab.instantiate();
        ASSERT_NE(instance, nullptr);
        instance->release();
    }
}

TEST(PhaseGValidationTest, assetRefCountBalance1000Cycles) {
    auto &refManager = AssetRefManager::getInstance();
    auto *asset = new Asset();
    asset->setUuid("phase-g-asset-ref");

    for (int i = 0; i < 1000; ++i) {
        refManager.addRef(asset);
        refManager.decRef(asset, true);
    }

    EXPECT_EQ(refManager.getRefCount(asset), 0U);
    EXPECT_EQ(asset->getAssetRefCount(), 0U);
}

} // namespace
