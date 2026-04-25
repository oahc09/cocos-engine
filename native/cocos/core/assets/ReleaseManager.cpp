/****************************************************************************
 Copyright (c) 2021-2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
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

#include "core/assets/ReleaseManager.h"
#include "core/assets/Asset.h"
#include "base/Macros.h"
#include "base/Log.h"

namespace cc {

ReleaseManager& ReleaseManager::getInstance() {
    static ReleaseManager instance;
    return instance;
}

void ReleaseManager::init() {
    _records.clear();
    _pendingRelease.clear();
}

void ReleaseManager::registerAsset(Asset* asset) {
    if (asset == nullptr) {
        return;
    }
    const ccstd::string& uuid = asset->getUuid();
    if (uuid.empty()) {
        return;
    }
    auto it = _records.find(uuid);
    if (it != _records.end()) {
        it->second.asset = asset;
    } else {
        RefRecord record;
        record.asset = asset;
        record.count = 0;
        _records.emplace(uuid, record);
    }
}

void ReleaseManager::unregisterAsset(Asset* asset) {
    if (asset == nullptr) {
        return;
    }
    const ccstd::string& uuid = asset->getUuid();
    if (uuid.empty()) {
        return;
    }
    _records.erase(uuid);
}

void ReleaseManager::addRef(const ccstd::string& uuid) {
    auto it = _records.find(uuid);
    if (it != _records.end()) {
        ++it->second.count;
    }
}

void ReleaseManager::decRef(const ccstd::string& uuid) {
    auto it = _records.find(uuid);
    if (it == _records.end()) {
        return;
    }
    if (it->second.count > 0) {
        --it->second.count;
    }
    if (it->second.count == 0) {
        _pendingRelease.emplace_back(uuid);
    }
}

uint32_t ReleaseManager::getRefCount(const ccstd::string& uuid) const {
    auto it = _records.find(uuid);
    if (it != _records.end()) {
        return it->second.count;
    }
    return 0;
}

void ReleaseManager::autoRelease() {
    // Copy pending list to avoid modification during iteration
    ccstd::vector<ccstd::string> pending = _pendingRelease;
    _pendingRelease.clear();
    for (const auto& uuid : pending) {
        releaseAsset(uuid);
    }
}

void ReleaseManager::releaseAll() {
    for (const auto& pair : _records) {
        _pendingRelease.emplace_back(pair.first);
    }
    autoRelease();
}

void ReleaseManager::releaseAsset(const ccstd::string& uuid) {
    auto it = _records.find(uuid);
    if (it == _records.end()) {
        return;
    }

    // Recursively release dependencies first
    RefRecord record = it->second;
    for (const auto& dependUuid : record.dependencies) {
        decRef(dependUuid);
    }

    // Remove from records; Asset lifetime is managed by IntrusivePtr
    _records.erase(it);
}

size_t ReleaseManager::getPendingReleaseCount() const {
    return _pendingRelease.size();
}

ccstd::vector<AssetRefInfo> ReleaseManager::getAllRefInfos() const {
    ccstd::vector<AssetRefInfo> infos;
    infos.reserve(_records.size());
    for (const auto& pair : _records) {
        AssetRefInfo info;
        info.uuid = pair.first;
        info.refCount = pair.second.count;
        info.dependUuids.reserve(pair.second.dependencies.size());
        for (const auto& dep : pair.second.dependencies) {
            info.dependUuids.emplace_back(dep);
        }
        infos.emplace_back(info);
    }
    return infos;
}

void ReleaseManager::addDependency(const ccstd::string& assetUuid, const ccstd::string& dependUuid) {
    auto it = _records.find(assetUuid);
    if (it != _records.end()) {
        it->second.dependencies.emplace(dependUuid);
    }
}

void ReleaseManager::removeDependency(const ccstd::string& assetUuid, const ccstd::string& dependUuid) {
    auto it = _records.find(assetUuid);
    if (it != _records.end()) {
        it->second.dependencies.erase(dependUuid);
    }
}

ccstd::vector<ccstd::string> ReleaseManager::getDependencies(const ccstd::string& assetUuid) const {
    ccstd::vector<ccstd::string> deps;
    auto it = _records.find(assetUuid);
    if (it != _records.end()) {
        deps.reserve(it->second.dependencies.size());
        for (const auto& dep : it->second.dependencies) {
            deps.emplace_back(dep);
        }
    }
    return deps;
}

} // namespace cc
