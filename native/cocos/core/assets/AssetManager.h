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

#pragma once

#include <functional>

#include "base/Macros.h"
#include "base/RefCounted.h"
#include "base/Ptr.h"
#include "base/std/container/string.h"
#include "base/std/container/unordered_map.h"
#include "base/std/container/vector.h"

namespace cc {

class Asset;
class NativeBundle;
class NativePipeline;

/**
 * @en Asset load request descriptor.
 * @zh 资源加载请求描述符。
 */
struct AssetLoadRequest {
    ccstd::string uuid;
    ccstd::string bundleName;
    ccstd::string type;       // Asset type name, e.g. "cc.Texture2D"
    bool cache{true};         // Whether to cache after loading
    uint32_t priority{0};     // Load priority
};

/**
 * @en Callback invoked when an async asset load completes.
 * @zh 异步资源加载完成时调用的回调。
 */
using AssetLoadCallback = std::function<void(Asset *asset, const ccstd::string &error)>;

/**
 * @en Central asset manager for C++ fast-path asset loading.
 * @zh C++ 快速路径资源加载的中央管理器。
 *
 * Design:
 * - Maintains an in-memory cache of loaded assets (uuid → Asset*).
 * - Registers NativeBundle instances for bundle-scoped lookups.
 * - Delegates to NativePipeline for dual-mode (JS_ONLY / NATIVE_FAST) support.
 * - Single-threaded; no locks required.
 */
class AssetManager {
public:
    static AssetManager &getInstance();

    /**
     * @en Initialize with the native pipeline for dual-mode support.
     * @zh 使用原生管线初始化，以支持双模式。
     */
    void init(NativePipeline *pipeline);

    // === Synchronous loading ===

    /**
     * @en Load an asset synchronously (blocking, NATIVE_FAST mode).
     * @zh 同步加载资源（阻塞式，NATIVE_FAST 模式）。
     *
     * Lookup order:
     * 1. In-memory cache (_cache)
     * 2. Registered bundle (NativeBundle::get)
     * 3. Return nullptr (remote loading not yet implemented)
     */
    IntrusivePtr<Asset> loadSync(const AssetLoadRequest &request);

    // === Asynchronous loading ===

    /**
     * @en Load an asset asynchronously via callback.
     * @zh 通过回调异步加载资源。
     *
     * Current implementation executes synchronously then invokes the callback.
     * TODO: integrate thread-pool for true async in future.
     */
    void load(const AssetLoadRequest &request, const AssetLoadCallback &callback);

    // === Bundle-scoped loading ===

    /**
     * @en Load an asset from a specific bundle by path.
     * @zh 从指定 bundle 按路径加载资源。
     */
    IntrusivePtr<Asset> loadFromBundle(const ccstd::string &bundleName, const ccstd::string &path);

    // === Cache management ===

    void cacheAsset(const ccstd::string &uuid, Asset *asset);
    Asset *getCachedAsset(const ccstd::string &uuid);
    void removeCachedAsset(const ccstd::string &uuid);
    void clearCache();

    // === Bundle management ===

    void registerBundle(NativeBundle *bundle);
    void unregisterBundle(const ccstd::string &name);
    NativeBundle *getBundle(const ccstd::string &name);

    // === Preload ===

    /**
     * @en Preload a batch of assets.
     * @zh 预加载一批资源。
     */
    void preload(const ccstd::vector<AssetLoadRequest> &requests, const std::function<void()> &onComplete);

    // === Runtime mode ===

    bool isNativeFastMode() const;

private:
    AssetManager() = default;
    ~AssetManager() = default;

    ccstd::unordered_map<ccstd::string, IntrusivePtr<Asset>> _cache;
    ccstd::unordered_map<ccstd::string, NativeBundle *> _bundles;
    NativePipeline *_pipeline{nullptr};

    CC_DISALLOW_COPY_MOVE_ASSIGN(AssetManager);
};

} // namespace cc
