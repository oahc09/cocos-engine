/****************************************************************************
 Copyright (c) 2021 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated engine source code (the "Software"), a limited,
 worldwide, royalty-free, non-assignable, revocable and non-exclusive license
 to use Cocos Creator solely to develop games on your target platforms. You shall
 not use Cocos Creator software for developing other software or tools that's
 used for developing games. You are not granted to publish, distribute,
 sublicense, and/or sell copies of Cocos Creator.

 The software or tools in this License Agreement are licensed, not sold.
 Xiamen Yaji Software Co., Ltd. reserves all rights not expressly granted to you.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/

#include "core/Director.h"
#include "base/Scheduler.h"
#include "core/components/Component.h"
#include "core/scene-graph/Node.h"
#include "gtest/gtest.h"

using namespace cc;

namespace {

class LifecycleTestComponent final : public Component {
public:
    int preloadCalls{0};
    int loadCalls{0};
    int enableCalls{0};
    int disableCalls{0};
    int startCalls{0};
    int updateCalls{0};
    int lateUpdateCalls{0};

    void __preload() override { ++preloadCalls; }
    void onLoad() override { ++loadCalls; }
    void onEnable() override { ++enableCalls; }
    void onDisable() override { ++disableCalls; }
    void start() override { ++startCalls; }
    void update(float) override { ++updateCalls; }
    void lateUpdate(float) override { ++lateUpdateCalls; }

    bool hasStartMethod() const override { return true; }
    bool hasUpdateMethod() const override { return true; }
    bool hasLateUpdateMethod() const override { return true; }
};

class StartOnlyComponent final : public Component {
public:
    int startCalls{0};

    void start() override { ++startCalls; }
    bool hasStartMethod() const override { return true; }
};

void resetDirectorState() {
    auto *director = Director::getInstance();
    director->getCompScheduler()->clear();
    director->setScene(nullptr);
}

void stepSchedulerFrame(float dt) {
    auto *director = Director::getInstance();
    director->getScheduler()->update(dt);
    director->tick(dt);
}

} // namespace

TEST(ComponentTest, lifecycleAndSchedulerFollowEnableState) {
    resetDirectorState();

    auto *director = Director::getInstance();
    auto *root = new Node("root");
    auto *node = new Node("child");
    auto *comp = new LifecycleTestComponent();

    director->getNodeActivator()->activateNode(root, true);
    node->setParent(root);
    node->addComponent(comp);

    EXPECT_TRUE(comp->isEnabledInHierarchy());
    EXPECT_EQ(comp->preloadCalls, 1);
    EXPECT_EQ(comp->loadCalls, 1);
    EXPECT_EQ(comp->enableCalls, 1);

    director->tick(1.0F / 60.0F);
    director->tick(1.0F / 60.0F);

    EXPECT_EQ(comp->startCalls, 1);
    EXPECT_EQ(comp->updateCalls, 2);
    EXPECT_EQ(comp->lateUpdateCalls, 2);

    comp->setEnabled(false);
    EXPECT_FALSE(comp->isEnabledInHierarchy());
    EXPECT_EQ(comp->disableCalls, 1);

    director->tick(1.0F / 60.0F);
    EXPECT_EQ(comp->startCalls, 1);
    EXPECT_EQ(comp->updateCalls, 2);
    EXPECT_EQ(comp->lateUpdateCalls, 2);

    comp->setEnabled(true);
    EXPECT_TRUE(comp->isEnabledInHierarchy());
    EXPECT_EQ(comp->preloadCalls, 1);
    EXPECT_EQ(comp->loadCalls, 1);
    EXPECT_EQ(comp->enableCalls, 2);

    director->tick(1.0F / 60.0F);
    EXPECT_EQ(comp->startCalls, 1);
    EXPECT_EQ(comp->updateCalls, 3);
    EXPECT_EQ(comp->lateUpdateCalls, 3);
}

TEST(ComponentTest, startRunsOnlyOnceAcrossDisableEnableCycles) {
    resetDirectorState();

    auto *director = Director::getInstance();
    auto *root = new Node("root");
    auto *node = new Node("child");
    auto *comp = new StartOnlyComponent();

    director->getNodeActivator()->activateNode(root, true);
    node->setParent(root);
    node->addComponent(comp);

    director->tick(1.0F / 60.0F);
    EXPECT_EQ(comp->startCalls, 1);

    comp->setEnabled(false);
    comp->setEnabled(true);
    director->tick(1.0F / 60.0F);

    EXPECT_EQ(comp->startCalls, 1);
}

TEST(ComponentTest, scheduleAndUnscheduleDriveDirectorScheduler) {
    resetDirectorState();

    auto *director = Director::getInstance();
    auto *root = new Node("root");
    auto *node = new Node("child");
    auto *comp = new Component();

    director->getNodeActivator()->activateNode(root, true);
    node->setParent(root);
    node->addComponent(comp);

    int callbackCount = 0;
    std::function<void(float)> callback = [&callbackCount](float) {
        ++callbackCount;
    };

    comp->schedule(callback, 0.05F, 10, 0.0F, false);

    stepSchedulerFrame(0.05F);
    stepSchedulerFrame(0.05F);
    EXPECT_EQ(callbackCount, 1);

    comp->unschedule(callback);

    stepSchedulerFrame(0.05F);
    stepSchedulerFrame(0.05F);
    EXPECT_EQ(callbackCount, 1);
}

TEST(ComponentTest, reschedulingSameCallbackObjectReusesTimerKey) {
    resetDirectorState();

    auto *director = Director::getInstance();
    auto *root = new Node("root");
    auto *node = new Node("child");
    auto *comp = new Component();

    director->getNodeActivator()->activateNode(root, true);
    node->setParent(root);
    node->addComponent(comp);

    int callbackCount = 0;
    std::function<void(float)> callback = [&callbackCount](float) {
        ++callbackCount;
    };

    comp->schedule(callback, 0.05F, 10, 0.0F, false);
    comp->schedule(callback, 0.10F, 10, 0.0F, false);

    stepSchedulerFrame(0.05F);
    stepSchedulerFrame(0.05F);
    EXPECT_EQ(callbackCount, 0);

    stepSchedulerFrame(0.05F);
    EXPECT_EQ(callbackCount, 1);
}
