/****************************************************************************
Copyright (c) 2021 Xiamen Yaji Software Co., Ltd.

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

#include "core/assets/AssetManager.h"
#include "core/assets/NativeBundle.h"
#include "core/assets/ReleaseManager.h"
#include "core/assets/SceneAsset.h"
#include "core/scene-graph/Scene.h"
#include "gtest/gtest.h"

using namespace cc;

namespace {

TEST(AssetManagerTest, loadSyncResolvesBundleSceneNameToCachedSceneAsset) {
    auto &assetManager = AssetManager::getInstance();
    assetManager.clearCache();

    auto *bundle = new NativeBundle("main", "db://assets");
    const ccstd::string configJson = R"({
        "scenes": {
            "scene/main": { "url": "db://assets/scene/main.scene", "uuid": "scene-uuid-main" }
        }
    })";
    ASSERT_TRUE(bundle->init(configJson));
    assetManager.registerBundle(bundle);

    auto *sceneAsset = new SceneAsset();
    sceneAsset->setScene(new Scene("SceneFromBundle"));
    assetManager.cacheAsset("scene-uuid-main", sceneAsset);

    AssetLoadRequest request;
    request.uuid = "scene/main";
    request.bundleName = "main";

    auto loaded = assetManager.loadSync(request);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded.get(), sceneAsset);
    EXPECT_EQ(assetManager.getCachedAsset("scene/main"), sceneAsset);

    assetManager.unregisterBundle("main");
    assetManager.clearCache();
}

TEST(AssetManagerTest, loadSceneAssetResolvesSceneNameAcrossRegisteredBundles) {
    auto &assetManager = AssetManager::getInstance();
    assetManager.clearCache();

    auto *bundle = new NativeBundle("main", "db://assets");
    const ccstd::string configJson = R"({
        "scenes": {
            "bundle-scene": { "url": "db://assets/scene/bundle-scene.scene", "uuid": "scene-uuid-bundle" }
        }
    })";
    ASSERT_TRUE(bundle->init(configJson));
    assetManager.registerBundle(bundle);

    auto *sceneAsset = new SceneAsset();
    sceneAsset->setScene(new Scene("BundleScene"));
    assetManager.cacheAsset("scene-uuid-bundle", sceneAsset);

    auto resolved = assetManager.loadSceneAsset("bundle-scene");
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved.get(), sceneAsset);
    EXPECT_EQ(assetManager.getCachedAsset("bundle-scene"), sceneAsset);

    assetManager.unregisterBundle("main");
    assetManager.clearCache();
}

TEST(AssetManagerTest, nativeBundleGetResolvesCachedAssetByPathAndUuid) {
    auto &assetManager = AssetManager::getInstance();
    assetManager.clearCache();

    auto *bundle = new NativeBundle("main", "db://assets");
    const ccstd::string configJson = R"({
        "paths": {
            "texture-uuid-main": { "path": "textures/test", "ctype": 1 }
        }
    })";
    ASSERT_TRUE(bundle->init(configJson));

    auto *asset = new Asset();
    asset->setUuid("texture-uuid-main");
    assetManager.cacheAsset("texture-uuid-main", asset);

    EXPECT_EQ(bundle->get("textures/test"), asset);
    EXPECT_EQ(bundle->get("texture-uuid-main"), asset);
    EXPECT_EQ(bundle->get("textures/missing"), nullptr);

    assetManager.clearCache();
}

TEST(AssetManagerTest, cacheAndClearRegisterAssetsWithReleaseManager) {
    auto &assetManager = AssetManager::getInstance();
    auto &releaseManager = ReleaseManager::getInstance();
    assetManager.clearCache();
    releaseManager.init();

    auto *asset = new Asset();
    asset->setUuid("cached-asset-for-release-manager");

    assetManager.cacheAsset(asset->getUuid(), asset);
    EXPECT_EQ(releaseManager.getRefCount(asset->getUuid()), 1U);
    EXPECT_EQ(releaseManager.getAllRefInfos().size(), 1U);

    assetManager.removeCachedAsset(asset->getUuid());
    EXPECT_EQ(releaseManager.getAllRefInfos().size(), 0U);
}

} // namespace
