/****************************************************************************
Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

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

#include "bindings/jswrapper/SeApi.h"
#include "core/scripting/ScriptBridge.h"
#include "gtest/gtest.h"

using namespace cc;

namespace {

TEST(ScriptBridgeTest, registerUnregisterScriptInstances1000Cycles) {
    auto *engine = se::ScriptEngine::getInstance();
    ASSERT_NE(engine, nullptr);
    auto *global = engine->getGlobalObject();
    ASSERT_NE(global, nullptr);

    auto &bridge = ScriptBridge::getInstance();
    bridge.shutdown();
    bridge.init(global);

    for (uint32_t i = 1; i <= 1000; ++i) {
        se::HandleObject jsComp(se::Object::createPlainObject());
        ASSERT_NE(jsComp.get(), nullptr);

        const uint32_t compId = bridge.registerScriptInstance(i, jsComp.get(), "ScriptBridgeStressComp", nullptr);
        EXPECT_EQ(compId, i);
        EXPECT_EQ(bridge.getInstanceCount(), 1U);
        EXPECT_TRUE(bridge.isInstanceOf(compId, "ScriptBridgeStressComp"));

        bridge.unregisterScriptInstance(compId);
        EXPECT_EQ(bridge.getInstanceCount(), 0U);
        EXPECT_FALSE(bridge.isInstanceOf(compId, "ScriptBridgeStressComp"));

        if (i % 100 == 0) {
            engine->garbageCollect();
            EXPECT_FALSE(engine->isGarbageCollecting());
        }
    }

    bridge.shutdown();
}

TEST(ScriptBridgeTest, gcSafetyAfterBurstRegistration) {
    auto *engine = se::ScriptEngine::getInstance();
    ASSERT_NE(engine, nullptr);
    auto *global = engine->getGlobalObject();
    ASSERT_NE(global, nullptr);

    auto &bridge = ScriptBridge::getInstance();
    bridge.shutdown();
    bridge.init(global);

    for (uint32_t i = 1; i <= 2000; ++i) {
        se::HandleObject jsComp(se::Object::createPlainObject());
        const uint32_t compId = bridge.registerScriptInstance(i, jsComp.get(), "ScriptBridgeGCComp", nullptr);
        bridge.unregisterScriptInstance(compId);
    }

    EXPECT_EQ(bridge.getInstanceCount(), 0U);
    engine->garbageCollect();
    EXPECT_FALSE(engine->isGarbageCollecting());

    bridge.shutdown();
}

} // namespace
