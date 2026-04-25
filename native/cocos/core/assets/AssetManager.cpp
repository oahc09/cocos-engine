/****************************************************************************
 Copyright (c) 2021-2023 Xiamen Yaji Software Co., Ltd.

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

#include "core/assets/AssetManager.h"
#include "core/assets/Asset.h"
#include "core/assets/NativeBundle.h"
#include "core/assets/NativePipeline.h"
#include "base/Log.h"

namespace cc {

AssetManager &AssetManager::getInstance() {
    static AssetManager instance;
    return instance;
}

void AssetManager::init(NativePipeline *pipeline) {
    _pipeline = pipeline;
}

IntrusivePtr<Asset> AssetManager::loadSync(const AssetLoadRequest &request) {
    // 1. Check in-memory cache first
    if (request.cache) {
        Asset *cached = getCachedAsset(request.uuid);
        if (cached != nullptr) {
            return IntrusivePtr<Asset>(cached);
        }
    }

    // 2. Check registered bundle
    if (!request.bundleName.empty()) {
        NativeBundle *bundle = getBundle(request.bundleName);
        if (bundle != nullptr) {
            Asset *asset = bundle->get(request.uuid);
            if (asset != nullptr) {
                if (request.cache) {
                    cacheAsset(request.uuid, asset);
                }
                return IntrusivePtr<Asset>(asset);
            }
        }
    }

    // 3. Fallback: remote loading not yet implemented
    CC_LOG_DEBUG("AssetManager::loadSync — asset not found for uuid '%s'", request.uuid.c_str());
    return nullptr;
}

void AssetManager::load(const AssetLoadRequest &request, const AssetLoadCallback &callback) {
    // Current stub: execute synchronously then invoke callback.
    // TODO: integrate thread-pool for true async loading.
    IntrusivePtr<Asset> asset = loadSync(request);
    if (asset != nullptr) {
        callback(asset.get(), "");
    } else {
        callback(nullptr, "Asset not found: " + request.uuid);
    }
}

IntrusivePtr<Asset> AssetManager::loadFromBundle(const ccstd::string &bundleName, const ccstd::string &path) {
    NativeBundle *bundle = getBundle(bundleName);
    if (bundle == nullptr) {
        CC_LOG_ERROR("AssetManager::loadFromBundle — bundle '%s' not registered", bundleName.c_str());
        return nullptr;
    }

    Asset *asset = bundle->get(path);
    if (asset != nullptr) {
        // Derive uuid from path for caching
        ccstd::string uuid = bundle->getUuidByPath(path);
        if (!uuid.empty()) {
            cacheAsset(uuid, asset);
        }
        return IntrusivePtr<Asset>(asset);
    }

    return nullptr;
}

void AssetManager::cacheAsset(const ccstd::string &uuid, Asset *asset) {
    if (uuid.empty() || asset == nullptr) {
        return;
    }
    _cache[uuid] = IntrusivePtr<Asset>(asset);
}

Asset *AssetManager::getCachedAsset(const ccstd::string &uuid) {
    auto it = _cache.find(uuid);
    if (it != _cache.end()) {
        return it->second.get();
    }
    return nullptr;
}

void AssetManager::removeCachedAsset(const ccstd::string &uuid) {
    _cache.erase(uuid);
}

void AssetManager::clearCache() {
    _cache.clear();
}

void AssetManager::registerBundle(NativeBundle *bundle) {
    if (bundle == nullptr) {
        return;
    }
    _bundles[bundle->getName()] = bundle;
}

void AssetManager::unregisterBundle(const ccstd::string &name) {
    _bundles.erase(name);
}

NativeBundle *AssetManager::getBundle(const ccstd::string &name) {
    auto it = _bundles.find(name);
    if (it != _bundles.end()) {
        return it->second;
    }
    return nullptr;
}

void AssetManager::preload(const ccstd::vector<AssetLoadRequest> &requests, const std::function<void()> &onComplete) {
    for (const auto &req : requests) {
        loadSync(req);
    }
    if (onComplete) {
        onComplete();
    }
}

bool AssetManager::isNativeFastMode() const {
    return (_pipeline != nullptr) && (_pipeline->getMode() == NativePipeline::Mode::NATIVE_FAST);
}

} // namespace cc
