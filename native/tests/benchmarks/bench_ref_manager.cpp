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

// bench_ref_manager.cpp — AssetRefManager micro-benchmark (M1-S3, G-15)
//
// Uses a self-contained mock Asset to isolate AssetRefManager overhead
// from the real Asset class implementation. This allows the benchmark
// to compile standalone without linking the full engine.
//
// Test matrix:
//   - addRef / decRef / getRefCount single-op throughput
//   - Batch operations (addRefBatch / decRefBatch)
//   - Callback dispatch overhead
//   - Null-asset guard overhead

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

// ─── Minimal mock Asset (mirrors cc::Asset ref-counting interface) ────────
namespace cc {

class Asset {
public:
    Asset() = default;
    ~Asset() = default;

    void addAssetRef() { ++_assetRefCount; }
    void decAssetRef(bool /*autoRelease*/ = true) {
        if (_assetRefCount > 0) --_assetRefCount;
    }
    uint32_t getAssetRefCount() const { return _assetRefCount; }

private:
    uint32_t _assetRefCount{0};
};

// ─── AssetRefManager (re-implemented for standalone benchmark) ────────────
// This mirrors the exact logic in native/cocos/core/assets/AssetRefManager.cpp
class AssetRefManager {
public:
    static AssetRefManager &getInstance() {
        static AssetRefManager instance;
        return instance;
    }

    using RefCountChangedCallback = std::function<void(Asset *, uint32_t, uint32_t)>;

    void addRef(Asset *asset) {
        if (!asset) return;
        uint32_t oldCount = asset->getAssetRefCount();
        asset->addAssetRef();
        uint32_t newCount = asset->getAssetRefCount();
        if (_callback) _callback(asset, oldCount, newCount);
    }

    void decRef(Asset *asset, bool autoRelease = true) {
        if (!asset) return;
        uint32_t oldCount = asset->getAssetRefCount();
        if (oldCount == 0) return;
        asset->decAssetRef(autoRelease);
        uint32_t newCount = asset->getAssetRefCount();
        if (_callback) _callback(asset, oldCount, newCount);
    }

    uint32_t getRefCount(Asset *asset) const {
        if (!asset) return 0;
        return asset->getAssetRefCount();
    }

    void addRefBatch(const std::vector<Asset *> &assets) {
        for (auto *asset : assets) addRef(asset);
    }

    void decRefBatch(const std::vector<Asset *> &assets, bool autoRelease = true) {
        for (auto *asset : assets) decRef(asset, autoRelease);
    }

    void setRefCountChangedCallback(const RefCountChangedCallback &cb) {
        _callback = cb;
    }

private:
    AssetRefManager() = default;
    RefCountChangedCallback _callback;
};

} // namespace cc

// ─── Benchmark infrastructure ─────────────────────────────────────────────
using Clock = std::chrono::high_resolution_clock;
using Us = std::chrono::microseconds;
using Ns = std::chrono::nanoseconds;

static void printResult(const char *label, int64_t totalUs, int64_t ops) {
    double nsPerOp = static_cast<double>(totalUs) * 1000.0 / static_cast<double>(ops);
    printf("  %-40s %8lld us  (%8.2f ns/op)\n", label,
           static_cast<long long>(totalUs), nsPerOp);
}

int main() {
    printf("============================================\n");
    printf("  AssetRefManager Micro-Benchmark (M1-S3)\n");
    printf("============================================\n\n");

    auto &manager = cc::AssetRefManager::getInstance();

    // Create test assets
    const int ASSET_COUNT = 1000;
    std::vector<cc::Asset *> assets;
    assets.reserve(ASSET_COUNT);
    for (int i = 0; i < ASSET_COUNT; ++i) {
        assets.push_back(new cc::Asset());
    }

    // ── Test 1: addRef single-op throughput ───────────────────────────────
    {
        auto start = Clock::now();
        for (int i = 0; i < 10000; ++i) {
            manager.addRef(assets[i % ASSET_COUNT]);
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("addRef x10000", elapsed, 10000);
    }

    // ── Test 2: decRef single-op throughput ───────────────────────────────
    {
        auto start = Clock::now();
        for (int i = 0; i < 10000; ++i) {
            manager.decRef(assets[i % ASSET_COUNT], false);
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("decRef x10000", elapsed, 10000);
    }

    // ── Test 3: getRefCount throughput ────────────────────────────────────
    {
        auto start = Clock::now();
        for (int i = 0; i < 100000; ++i) {
            volatile auto count = manager.getRefCount(assets[i % ASSET_COUNT]);
            (void)count;
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("getRefCount x100000", elapsed, 100000);
    }

    // Reset all ref counts to 0 for clean batch test
    for (auto *asset : assets) {
        while (asset->getAssetRefCount() > 0) {
            asset->decAssetRef(false);
        }
    }

    // ── Test 4: addRefBatch throughput ────────────────────────────────────
    {
        auto start = Clock::now();
        for (int i = 0; i < 100; ++i) {
            manager.addRefBatch(assets);
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("addRefBatch 100x1000", elapsed, 100000);
    }

    // ── Test 5: decRefBatch throughput ────────────────────────────────────
    {
        auto start = Clock::now();
        for (int i = 0; i < 100; ++i) {
            manager.decRefBatch(assets, false);
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("decRefBatch 100x1000", elapsed, 100000);
    }

    // Reset for callback test
    for (auto *asset : assets) {
        while (asset->getAssetRefCount() > 0) {
            asset->decAssetRef(false);
        }
    }

    // ── Test 6: addRef WITHOUT callback (baseline) ───────────────────────
    {
        auto start = Clock::now();
        for (int i = 0; i < 10000; ++i) {
            manager.addRef(assets[i % ASSET_COUNT]);
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("addRef (no callback) x10000", elapsed, 10000);
    }

    // Reset for callback test
    for (auto *asset : assets) {
        while (asset->getAssetRefCount() > 0) {
            asset->decAssetRef(false);
        }
    }

    // ── Test 7: addRef WITH callback ──────────────────────────────────────
    int callbackCount = 0;
    manager.setRefCountChangedCallback([&](cc::Asset *, uint32_t, uint32_t) {
        callbackCount++;
    });
    {
        auto start = Clock::now();
        for (int i = 0; i < 10000; ++i) {
            manager.addRef(assets[i % ASSET_COUNT]);
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("addRef (with callback) x10000", elapsed, 10000);
        printf("    (callback invoked %d times)\n", callbackCount);
    }

    // ── Test 8: Null-asset guard overhead ─────────────────────────────────
    {
        auto start = Clock::now();
        for (int i = 0; i < 100000; ++i) {
            manager.addRef(nullptr);
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("addRef(nullptr) x100000", elapsed, 100000);
    }

    // ── Test 9: Raw Asset::addAssetRef (baseline, no manager) ────────────
    // Reset for baseline test
    manager.setRefCountChangedCallback(nullptr);
    for (auto *asset : assets) {
        while (asset->getAssetRefCount() > 0) {
            asset->decAssetRef(false);
        }
    }
    {
        auto start = Clock::now();
        for (int i = 0; i < 100000; ++i) {
            assets[i % ASSET_COUNT]->addAssetRef();
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("raw Asset::addAssetRef x100000", elapsed, 100000);
    }

    // ── Summary ───────────────────────────────────────────────────────────
    printf("\n──────────────────────────────────────────\n");
    printf("  Summary: AssetRefManager overhead is the delta\n");
    printf("  between 'addRef (no callback)' and 'raw Asset::addAssetRef'\n");
    printf("──────────────────────────────────────────\n");

    // Cleanup
    for (auto *asset : assets) {
        delete asset;
    }

    return 0;
}
