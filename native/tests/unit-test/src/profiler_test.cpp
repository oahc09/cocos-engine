/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

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

#include "gtest/gtest.h"
#include "profiler/Profiler.h"

using namespace cc;

TEST(ProfilerTest, recordsScriptBridgeBatchCounters) {
    Profiler profiler;

    profiler.recordScriptBridgeBatch("start", 2, 100);
    profiler.recordScriptBridgeBatch("update", 3, 250);
    profiler.recordScriptBridgeBatch("update", 1, 50);

    const auto &stats = profiler.getScriptBridgeStats();
    EXPECT_EQ(stats.totalBatchCalls, 3U);
    EXPECT_EQ(stats.totalComponents, 6U);
    EXPECT_EQ(stats.totalDurationUs, 400U);
    EXPECT_EQ(stats.startCalls, 1U);
    EXPECT_EQ(stats.updateCalls, 2U);
    EXPECT_EQ(stats.lateUpdateCalls, 0U);
}
