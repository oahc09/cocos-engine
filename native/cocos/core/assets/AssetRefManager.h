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

#include <functional>
#include "base/std/container/unordered_map.h"
#include "base/std/container/string.h"
#include "base/std/container/vector.h"

namespace cc {
class Asset;

/**
 * @brief Unified reference counting manager for assets.
 * C++ side is the authoritative source; JS side syncs via bridge callback.
 * Wraps the existing Asset::addAssetRef/decAssetRef with centralized tracking.
 */
class AssetRefManager {
public:
    static AssetRefManager &getInstance();

    // 统一引用计数操作（权威源）
    void addRef(Asset *asset);
    void decRef(Asset *asset, bool autoRelease = true);
    uint32_t getRefCount(Asset *asset) const;

    // 批量操作
    void addRefBatch(const ccstd::vector<Asset *> &assets);
    void decRefBatch(const ccstd::vector<Asset *> &assets, bool autoRelease = true);

    // 引用计数变化回调（通知 JS 侧）
    using RefCountChangedCallback = std::function<void(Asset *, uint32_t oldCount, uint32_t newCount)>;
    void setRefCountChangedCallback(const RefCountChangedCallback &cb);

private:
    AssetRefManager() = default;
    RefCountChangedCallback _callback;
};

} // namespace cc
