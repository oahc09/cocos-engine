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

// bench_type_registry.cpp — TypeRegistry micro-benchmark (M1-S3, G-15)
// Gate condition: getTypeInfo() query < 1μs/op

#include "core/serialization/TypeRegistry.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

using Clock = std::chrono::high_resolution_clock;
using Us = std::chrono::microseconds;
using Ns = std::chrono::nanoseconds;

static void printResult(const char *label, int64_t totalUs, int64_t ops) {
    double nsPerOp = static_cast<double>(totalUs) * 1000.0 / static_cast<double>(ops);
    printf("  %-35s %8lld us  (%8.2f ns/op)\n", label,
           static_cast<long long>(totalUs), nsPerOp);
}

int main() {
    printf("========================================\n");
    printf("  TypeRegistry Micro-Benchmark (M1-S3)\n");
    printf("========================================\n\n");

    auto &registry = cc::TypeRegistry::getInstance();

    // ── Test 1: Register 10000 built-in types ────────────────────────────
    {
        auto start = Clock::now();
        for (int i = 0; i < 10000; ++i) {
            cc::TypeRegistry::TypeInfo info;
            info.classId = 20000 + i;
            info.className = "TestScript" + std::to_string(i);
            info.isComponent = true;
            info.isBuiltin = false;
            registry.registerType(info);
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("registerType x10000", elapsed, 10000);
    }

    // ── Test 2: Query 100000 types (getTypeInfo by classId) ──────────────
    int64_t queryUs = 0;
    {
        auto start = Clock::now();
        for (int i = 0; i < 100000; ++i) {
            volatile auto *info = registry.getTypeInfo(20000 + (i % 10000));
            (void)info;
        }
        auto end = Clock::now();
        queryUs = std::chrono::duration_cast<Us>(end - start).count();
        printResult("getTypeInfo x100000", queryUs, 100000);
    }

    // ── Test 3: getClassIdByName x100000 ─────────────────────────────────
    {
        auto start = Clock::now();
        for (int i = 0; i < 100000; ++i) {
            volatile auto id = registry.getClassIdByName("TestScript" + std::to_string(i % 10000));
            (void)id;
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("getClassIdByName x100000", elapsed, 100000);
    }

    // ── Test 4: hasType x100000 ──────────────────────────────────────────
    {
        auto start = Clock::now();
        for (int i = 0; i < 100000; ++i) {
            volatile auto has = registry.hasType(20000 + (i % 10000));
            (void)has;
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("hasType x100000", elapsed, 100000);
    }

    // ── Test 5: registerScriptType x1000 ─────────────────────────────────
    {
        auto start = Clock::now();
        for (int i = 0; i < 1000; ++i) {
            registry.registerScriptType("BenchScript" + std::to_string(i),
                                        true, false, i % 100);
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("registerScriptType x1000", elapsed, 1000);
    }

    // ── Test 6: create x10000 (with constructor) ─────────────────────────
    {
        // Register a type with a trivial constructor
        cc::TypeRegistry::TypeInfo info;
        info.classId = 99999;
        info.className = "BenchCreateType";
        info.isComponent = true;
        info.isBuiltin = true;
        info.constructor = []() -> cc::CCObject * { return nullptr; }; // trivial
        registry.registerType(info);

        auto start = Clock::now();
        for (int i = 0; i < 10000; ++i) {
            volatile auto obj = registry.create(99999);
            (void)obj;
        }
        auto end = Clock::now();
        auto elapsed = std::chrono::duration_cast<Us>(end - start).count();
        printResult("create (trivial) x10000", elapsed, 10000);
    }

    // ── Gate Check ───────────────────────────────────────────────────────
    printf("\n────────────────────────────────────────\n");
    double queryNsPerOp = static_cast<double>(queryUs) * 1000.0 / 100000.0;
    bool gatePassed = queryNsPerOp < 1000.0; // < 1μs = < 1000ns
    printf("  GATE CHECK: getTypeInfo query = %.2f ns/op\n", queryNsPerOp);
    printf("  Threshold:   1000.00 ns/op (1 μs)\n");
    printf("  Result:      %s\n", gatePassed ? "PASS" : "FAIL");
    printf("────────────────────────────────────────\n");

    return gatePassed ? 0 : 1;
}
