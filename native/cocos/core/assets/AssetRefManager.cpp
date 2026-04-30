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

#include "core/assets/AssetRefManager.h"
#include "core/assets/Asset.h"
#include "core/assets/ReleaseManager.h"

namespace cc {

AssetRefManager &AssetRefManager::getInstance() {
    static AssetRefManager instance;
    return instance;
}

void AssetRefManager::addRef(Asset *asset) {
    if (!asset) return;
    ReleaseManager::getInstance().registerAsset(asset);
    ReleaseManager::getInstance().addRef(asset->getUuid());
    uint32_t oldCount = asset->getAssetRefCount();
    asset->addAssetRefInternal();
    uint32_t newCount = asset->getAssetRefCount();
    if (_callback) _callback(asset, oldCount, newCount);
}

void AssetRefManager::decRef(Asset *asset, bool autoRelease) {
    if (!asset) return;
    uint32_t oldCount = asset->getAssetRefCount();
    if (oldCount == 0) return;
    ReleaseManager::getInstance().registerAsset(asset);
    asset->decAssetRefInternal();
    ReleaseManager::getInstance().decRef(asset->getUuid());
    if (autoRelease) {
        ReleaseManager::getInstance().autoRelease();
    }
    uint32_t newCount = asset->getAssetRefCount();
    if (_callback) _callback(asset, oldCount, newCount);
}

uint32_t AssetRefManager::getRefCount(Asset *asset) const {
    if (!asset) return 0;
    return asset->getAssetRefCount();
}

void AssetRefManager::addRefBatch(const ccstd::vector<Asset *> &assets) {
    for (auto *asset : assets) addRef(asset);
}

void AssetRefManager::decRefBatch(const ccstd::vector<Asset *> &assets, bool autoRelease) {
    for (auto *asset : assets) decRef(asset, autoRelease);
}

void AssetRefManager::setRefCountChangedCallback(const RefCountChangedCallback &cb) {
    _callback = cb;
}

} // namespace cc
