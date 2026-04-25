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

#pragma once

#include "base/Macros.h"
#include "base/std/container/string.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"
#include "base/std/container/set.h"

namespace cc {

class Asset;
class Node;
class Component;

// 资源引用信息（供调试和强制释放用）
struct AssetRefInfo {
    ccstd::string uuid;
    uint32_t refCount{0};
    ccstd::vector<ccstd::string> dependUuids;  // 依赖的其他资源
};

class ReleaseManager {
public:
    static ReleaseManager& getInstance();

    // 初始化
    void init();

    // 注册/注销资源（由 AssetManager 调用）
    void registerAsset(Asset* asset);
    void unregisterAsset(Asset* asset);

    // 增加/减少引用（由 Node/Component 调用）
    void addRef(const ccstd::string& uuid);
    void decRef(const ccstd::string& uuid);

    // 获取引用计数
    uint32_t getRefCount(const ccstd::string& uuid) const;

    // 自动释放：引用计数为 0 的资源移入待释放队列
    void autoRelease();

    // 强制释放所有待释放资源
    void releaseAll();

    // 按 UUID 强制释放单个资源
    void releaseAsset(const ccstd::string& uuid);

    // 获取待释放队列大小（调试用）
    size_t getPendingReleaseCount() const;

    // 获取所有已注册资源信息（调试用）
    ccstd::vector<AssetRefInfo> getAllRefInfos() const;

    // 依赖管理：设置资源 A 依赖资源 B
    void addDependency(const ccstd::string& assetUuid, const ccstd::string& dependUuid);
    void removeDependency(const ccstd::string& assetUuid, const ccstd::string& dependUuid);
    ccstd::vector<ccstd::string> getDependencies(const ccstd::string& assetUuid) const;

private:
    ReleaseManager() = default;
    ~ReleaseManager() = default;

    struct RefRecord {
        uint32_t count{0};
        Asset* asset{nullptr};
        ccstd::set<ccstd::string> dependencies;
    };

    ccstd::unordered_map<ccstd::string, RefRecord> _records;
    ccstd::vector<ccstd::string> _pendingRelease;  // 引用计数为 0 的 UUID 队列

    CC_DISALLOW_COPY_MOVE_ASSIGN(ReleaseManager);
};

} // namespace cc
