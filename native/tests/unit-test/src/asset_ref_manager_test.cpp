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

#include "core/assets/Asset.h"
#include "core/assets/AssetRefManager.h"
#include "core/assets/ReleaseManager.h"
#include "gtest/gtest.h"

using namespace cc;

namespace {

TEST(AssetRefManagerTest, addRefAndDecRefStayInSyncWithReleaseManager) {
    auto &refManager = AssetRefManager::getInstance();
    auto &releaseManager = ReleaseManager::getInstance();
    releaseManager.init();

    auto *asset = new Asset();
    asset->setUuid("asset-ref-sync");

    refManager.addRef(asset);
    EXPECT_EQ(asset->getAssetRefCount(), 1U);
    EXPECT_EQ(refManager.getRefCount(asset), 1U);
    EXPECT_EQ(releaseManager.getRefCount("asset-ref-sync"), 1U);

    refManager.decRef(asset, false);
    EXPECT_EQ(asset->getAssetRefCount(), 0U);
    EXPECT_EQ(refManager.getRefCount(asset), 0U);
    EXPECT_EQ(releaseManager.getRefCount("asset-ref-sync"), 0U);
    EXPECT_EQ(releaseManager.getPendingReleaseCount(), 1U);
}

TEST(AssetRefManagerTest, autoReleaseClearsPendingReleaseQueue) {
    auto &refManager = AssetRefManager::getInstance();
    auto &releaseManager = ReleaseManager::getInstance();
    releaseManager.init();

    auto *asset = new Asset();
    asset->setUuid("asset-auto-release");

    refManager.addRef(asset);
    refManager.decRef(asset, true);

    EXPECT_EQ(asset->getAssetRefCount(), 0U);
    EXPECT_EQ(releaseManager.getPendingReleaseCount(), 0U);
    EXPECT_EQ(releaseManager.getRefCount("asset-auto-release"), 0U);
}

} // namespace
