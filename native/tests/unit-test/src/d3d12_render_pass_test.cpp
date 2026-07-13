#include "gtest/gtest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#if CC_USE_D3D12

    #include <d3dcompiler.h>
    #include <d3d12sdklayers.h>
    #include <wrl/client.h>
    #include "cocos/renderer/gfx-d3d12/D3D12DescriptorHeapPool.h"
    #include "cocos/renderer/gfx-d3d12/D3D12ShaderCacheScheduler.h"
    #include "cocos/renderer/gfx-d3d12/D3D12ShaderCompileScheduler.h"
    #include "cocos/renderer/gfx-d3d12/D3D12Shader.h"
    #include "cocos/renderer/gfx-d3d12/D3D12RenderPass.h"
    #include "cocos/renderer/gfx-d3d12/D3D12Device.h"
    #include "cocos/renderer/gfx-d3d12/D3D12Texture.h"

namespace cc {
namespace gfx {

TEST(D3D12DescriptorHeapPoolTest, CoalescesAdjacentFreedCPUBlocks) {
    D3D12DescriptorHeapPool pool;
    pool.initialize(D3D12DescriptorHeapPool::HeapType::SAMPLER, 32, false);

    const auto first = pool.allocate(16);
    const auto second = pool.allocate(16);
    ASSERT_TRUE(first.isValid);
    ASSERT_TRUE(second.isValid);
    ASSERT_EQ(pool.getHeapCount(), 1U);

    pool.deallocate(first);
    pool.deallocate(second);
    const auto combined = pool.allocate(32);
    ASSERT_TRUE(combined.isValid);
    EXPECT_EQ(combined.heapIndex, first.heapIndex);
    EXPECT_EQ(pool.getHeapCount(), 1U);

    pool.deallocate(combined);
}

TEST(D3D12ShaderCacheSessionTest, ConcurrentStoreOfSameKeyIsIdempotent) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    const ccstd::string key = "d3d12-session-idempotent-" +
                              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::vector<uint8_t> value{0x44, 0x58, 0x42, 0x43, 0x12, 0x34, 0x56, 0x78};
    auto store = [&]() {
        return device->storeShaderCacheValue(key.data(), static_cast<uint32_t>(key.size()), value);
    };

    auto first = std::async(std::launch::async, store);
    auto second = std::async(std::launch::async, store);
    EXPECT_TRUE(first.get());
    EXPECT_TRUE(second.get());

    std::vector<uint8_t> loaded;
    ASSERT_TRUE(device->loadShaderCacheValue(key.data(), static_cast<uint32_t>(key.size()), loaded));
    EXPECT_EQ(loaded, value);
}

TEST(D3D12ShaderCachePolicyTest, BackgroundProbeSkipsLegacyButDemandMigrates) {
    const auto background = detail::getD3D12ShaderCacheLookupPolicy(true);
    EXPECT_FALSE(background.probeLegacyV3);
    EXPECT_FALSE(background.deferLegacyFilePersistence);

    const auto demand = detail::getD3D12ShaderCacheLookupPolicy(false);
    EXPECT_TRUE(demand.probeLegacyV3);
    EXPECT_TRUE(demand.deferLegacyFilePersistence);
}

TEST(D3D12ShaderCachePersistenceQueueTest, EnqueueReturnsWhileWriterIsBlocked) {
    detail::D3D12ShaderCachePersistenceQueue queue(1);
    std::promise<void> writerStarted;
    auto writerStartedFuture = writerStarted.get_future();
    std::promise<void> releaseWriter;
    auto releaseWriterFuture = releaseWriter.get_future().share();
    std::atomic<bool> secondWriteRan{false};

    ASSERT_TRUE(queue.enqueue([&]() {
        writerStarted.set_value();
        releaseWriterFuture.wait_for(std::chrono::seconds(5));
    }));
    if (writerStartedFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        releaseWriter.set_value();
        FAIL() << "persistence writer did not start";
    }

    auto enqueueSecond = std::async(std::launch::async, [&]() {
        return queue.enqueue([&]() { secondWriteRan.store(true, std::memory_order_relaxed); });
    });
    const auto enqueueStatus = enqueueSecond.wait_for(std::chrono::seconds(2));
    if (enqueueStatus != std::future_status::ready) {
        releaseWriter.set_value();
        FAIL() << "enqueue waited for the active persistence writer";
    }
    EXPECT_TRUE(enqueueSecond.get());

    EXPECT_FALSE(queue.drainFor(std::chrono::milliseconds(20)));
    releaseWriter.set_value();
    EXPECT_TRUE(queue.drainFor(std::chrono::seconds(2)));
    EXPECT_TRUE(secondWriteRan.load(std::memory_order_relaxed));
}

TEST(D3D12ShaderCachePersistenceQueueTest, DestructionDrainsQueuedWrites) {
    std::atomic<uint32_t> completed{0};
    {
        detail::D3D12ShaderCachePersistenceQueue queue(1);
        for (uint32_t i = 0; i < 64; ++i) {
            ASSERT_TRUE(queue.enqueue([&]() {
                std::this_thread::yield();
                completed.fetch_add(1, std::memory_order_relaxed);
            }));
        }
    }
    EXPECT_EQ(completed.load(std::memory_order_relaxed), 64U);
}

TEST(D3D12ShaderCachePersistenceQueueTest, CloseRejectsWritesUntilReopened) {
    detail::D3D12ShaderCachePersistenceQueue queue(1);
    queue.closeAndDrain();

    std::atomic<bool> ran{false};
    EXPECT_FALSE(queue.enqueue([&]() { ran.store(true, std::memory_order_relaxed); }));
    EXPECT_FALSE(ran.load(std::memory_order_relaxed));

    queue.reopen();
    EXPECT_TRUE(queue.enqueue([&]() { ran.store(true, std::memory_order_relaxed); }));
    EXPECT_TRUE(queue.drainFor(std::chrono::seconds(2)));
    EXPECT_TRUE(ran.load(std::memory_order_relaxed));
}

TEST(D3D12ShaderCachePersistenceQueueTest, ThrownTaskDoesNotBlockDrainOrFollowingWrites) {
    detail::D3D12ShaderCachePersistenceQueue queue(1);
    std::atomic<bool> followingWriteRan{false};
    EXPECT_TRUE(queue.enqueue([]() { throw 7; }));
    EXPECT_TRUE(queue.enqueue([&]() { followingWriteRan.store(true, std::memory_order_relaxed); }));
    EXPECT_TRUE(queue.drainFor(std::chrono::seconds(2)));
    EXPECT_TRUE(followingWriteRan.load(std::memory_order_relaxed));
}

TEST(D3D12ShaderCompileSchedulerTest, DemandedQueuedTaskRunsInline) {
    detail::D3D12ShaderCompileScheduler scheduler(1);
    std::promise<void> backgroundStarted;
    std::promise<void> releaseBackground;
    auto releaseFuture = releaseBackground.get_future().share();

    auto background = scheduler.submit([&](bool demand) {
        EXPECT_FALSE(demand);
        backgroundStarted.set_value();
        releaseFuture.wait();
        return true;
    });
    backgroundStarted.get_future().wait();

    std::atomic<uint32_t> demandRuns{0};
    auto demanded = scheduler.submit([&](bool demand) {
        EXPECT_TRUE(demand);
        demandRuns.fetch_add(1, std::memory_order_relaxed);
        return true;
    });

    bool ranInline = false;
    EXPECT_TRUE(scheduler.runNowOrWait(demanded, ranInline));
    EXPECT_TRUE(ranInline);
    EXPECT_EQ(demandRuns.load(std::memory_order_relaxed), 1U);

    releaseBackground.set_value();
    scheduler.cancelAndWait(background);
}

TEST(D3D12ShaderCompileSchedulerTest, CancelledQueuedTaskNeverRuns) {
    detail::D3D12ShaderCompileScheduler scheduler(1);
    std::promise<void> backgroundStarted;
    std::promise<void> releaseBackground;
    auto releaseFuture = releaseBackground.get_future().share();

    auto background = scheduler.submit([&](bool) {
        backgroundStarted.set_value();
        releaseFuture.wait();
        return true;
    });
    backgroundStarted.get_future().wait();

    std::atomic<uint32_t> cancelledRuns{0};
    auto cancelled = scheduler.submit([&](bool) {
        cancelledRuns.fetch_add(1, std::memory_order_relaxed);
        return true;
    });
    scheduler.cancelAndWait(cancelled);
    EXPECT_EQ(cancelledRuns.load(std::memory_order_relaxed), 0U);

    releaseBackground.set_value();
    scheduler.cancelAndWait(background);
}

TEST(D3D12ShaderCompileSchedulerTest, MultipleDemandersExecuteQueuedTaskOnce) {
    detail::D3D12ShaderCompileScheduler scheduler(1);
    std::promise<void> backgroundStarted;
    std::promise<void> releaseBackground;
    auto releaseFuture = releaseBackground.get_future().share();

    auto background = scheduler.submit([&](bool) {
        backgroundStarted.set_value();
        releaseFuture.wait();
        return true;
    });
    backgroundStarted.get_future().wait();

    std::atomic<uint32_t> demandRuns{0};
    auto demanded = scheduler.submit([&](bool demand) {
        EXPECT_TRUE(demand);
        demandRuns.fetch_add(1, std::memory_order_relaxed);
        return true;
    });

    auto demand = [&]() {
        bool ranInline = false;
        return scheduler.runNowOrWait(demanded, ranInline);
    };
    auto first = std::async(std::launch::async, demand);
    auto second = std::async(std::launch::async, demand);
    EXPECT_TRUE(first.get());
    EXPECT_TRUE(second.get());
    EXPECT_EQ(demandRuns.load(std::memory_order_relaxed), 1U);

    releaseBackground.set_value();
    scheduler.cancelAndWait(background);
}

TEST(D3D12ShaderCompileSchedulerTest, UsesThreeBackgroundWorkersAndOneDemandSlot) {
    detail::D3D12ShaderCompileScheduler scheduler(3);
    std::atomic<uint32_t> active{0};
    std::atomic<uint32_t> peak{0};
    std::mutex startedMutex;
    std::condition_variable startedCondition;
    uint32_t started = 0;
    std::promise<void> releaseBackground;
    auto releaseFuture = releaseBackground.get_future().share();

    auto recordPeak = [&]() {
        const uint32_t now = active.fetch_add(1, std::memory_order_relaxed) + 1;
        uint32_t observed = peak.load(std::memory_order_relaxed);
        while (observed < now &&
               !peak.compare_exchange_weak(observed, now, std::memory_order_relaxed)) {
        }
    };

    std::vector<detail::D3D12ShaderCompileScheduler::Handle> backgrounds;
    for (uint32_t i = 0; i < 3; ++i) {
        backgrounds.emplace_back(scheduler.submit([&](bool demand) {
            EXPECT_FALSE(demand);
            recordPeak();
            {
                std::lock_guard<std::mutex> lock(startedMutex);
                ++started;
            }
            startedCondition.notify_one();
            releaseFuture.wait();
            active.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }));
    }
    {
        std::unique_lock<std::mutex> lock(startedMutex);
        ASSERT_TRUE(startedCondition.wait_for(lock, std::chrono::seconds(2), [&]() { return started == 3; }));
    }

    auto demanded = scheduler.submit([&](bool demand) {
        EXPECT_TRUE(demand);
        recordPeak();
        active.fetch_sub(1, std::memory_order_relaxed);
        return true;
    });
    bool ranInline = false;
    EXPECT_TRUE(scheduler.runNowOrWait(demanded, ranInline));
    EXPECT_TRUE(ranInline);
    EXPECT_EQ(peak.load(std::memory_order_relaxed), 4U);

    releaseBackground.set_value();
    for (const auto &background : backgrounds) {
        scheduler.cancelAndWait(background);
    }
}

TEST(D3D12ShaderCompileSchedulerTest, CancelWaitsForRunningTask) {
    detail::D3D12ShaderCompileScheduler scheduler(1);
    std::promise<void> started;
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    auto running = scheduler.submit([&](bool) {
        started.set_value();
        releaseFuture.wait();
        return true;
    });
    started.get_future().wait();

    auto cancelling = std::async(std::launch::async, [&]() {
        scheduler.cancelAndWait(running);
    });
    EXPECT_EQ(cancelling.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    release.set_value();
    EXPECT_EQ(cancelling.wait_for(std::chrono::seconds(2)), std::future_status::ready);
}

TEST(D3D12ShaderCompileSchedulerTest, CompletedTaskBypassesBusyForegroundDemand) {
    detail::D3D12ShaderCompileScheduler scheduler(1);

    auto completed = scheduler.submit([](bool /*demanded*/) {
        return true;
    });
    bool completedRanInline = false;
    ASSERT_TRUE(scheduler.runNowOrWait(completed, completedRanInline));

    std::promise<void> backgroundStarted;
    std::promise<void> releaseBackground;
    auto releaseBackgroundFuture = releaseBackground.get_future().share();
    auto background = scheduler.submit([&](bool /*demanded*/) {
        backgroundStarted.set_value();
        releaseBackgroundFuture.wait();
        return true;
    });
    backgroundStarted.get_future().wait();

    std::promise<void> foregroundStarted;
    std::promise<void> releaseForeground;
    auto releaseForegroundFuture = releaseForeground.get_future().share();
    auto foreground = scheduler.submit([&](bool demanded) {
        EXPECT_TRUE(demanded);
        foregroundStarted.set_value();
        releaseForegroundFuture.wait();
        return true;
    });
    auto foregroundDemand = std::async(std::launch::async, [&]() {
        bool ranInline = false;
        return scheduler.runNowOrWait(foreground, ranInline) && ranInline;
    });
    foregroundStarted.get_future().wait();

    auto completedDemand = std::async(std::launch::async, [&]() {
        bool ranInline = false;
        return scheduler.runNowOrWait(completed, ranInline) && !ranInline;
    });
    EXPECT_EQ(completedDemand.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);

    releaseForeground.set_value();
    EXPECT_TRUE(foregroundDemand.get());
    EXPECT_TRUE(completedDemand.get());
    releaseBackground.set_value();
    scheduler.cancelAndWait(background);
}

TEST(D3D12ShaderCompileSchedulerTest, ThrownTaskCompletesAndWakesWaiters) {
    detail::D3D12ShaderCompileScheduler scheduler(1);
    auto task = scheduler.submit([](bool /*demanded*/) -> bool {
        throw 7;
    });

    auto waiter = std::async(std::launch::async, [&]() {
        bool ranInline = false;
        return scheduler.runNowOrWait(task, ranInline);
    });
    EXPECT_EQ(waiter.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(waiter.get());
    scheduler.cancelAndWait(task);
}

TEST(D3D12ShaderLifecycleTest, DestroyRejectsLateForegroundCompileResult) {
    ccstd::string source = R"(
layout(location = 0) in vec3 a_position;
void main() {
    float value = a_position.x;
)";
    for (uint32_t i = 0; i < 800; ++i) {
        source += "value = value * 1.000001 + 0.000001;\n";
    }
    source += R"(
    gl_Position = vec4(value, a_position.yz, 1.0);
}
)";
    source += "// lifecycle-generation-" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());

    ShaderInfo info;
    info.name = "d3d12-foreground-lifecycle-test";
    info.stages.emplace_back(ShaderStage{ShaderStageFlagBit::VERTEX, source});

    CCD3D12Shader shader;
    shader.initialize(info);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::promise<void> demandStarted;
    auto demand = std::async(std::launch::async, [&]() {
        demandStarted.set_value();
        return shader.getVertexBytecode().size;
    });
    demandStarted.get_future().wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    shader.destroy();

    ASSERT_EQ(demand.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_EQ(demand.get(), 0U);
    EXPECT_EQ(shader.getVertexBytecode().size, 0U);
}

TEST(D3D12ShaderCacheTest, RuntimeBytecodeIsDeterministicAndKeepsReflection) {
    ShaderInfo info;
    info.name = "d3d12-scheduler-canonical-dxbc-test";
    info.stages.emplace_back(ShaderStage{
        ShaderStageFlagBit::VERTEX,
        R"(
#define TEST_JOIN_IMPL(lhs, rhs) lhs ## rhs
#define TEST_JOIN(lhs, rhs) TEST_JOIN_IMPL(lhs, rhs)
#define CC_TEST_POSITION_SCALE 1
layout(location = 0) in vec3 a_position;
void main() {
    gl_Position = vec4(a_position * float(TEST_JOIN(CC_TEST_, POSITION_SCALE)), 1.0);
}
)",
    });

    CCD3D12Shader first;
    first.initialize(info);
    const auto firstBlob = first.getVertexBytecode();
    ASSERT_NE(firstBlob.data, nullptr);
    ASSERT_GT(firstBlob.size, 0U);

    Microsoft::WRL::ComPtr<ID3DBlob> debugBlob;
    EXPECT_TRUE(FAILED(D3DGetBlobPart(firstBlob.data, firstBlob.size, D3D_BLOB_DEBUG_INFO, 0, &debugBlob)));

    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection;
    ASSERT_TRUE(SUCCEEDED(D3DReflect(firstBlob.data, firstBlob.size, IID_PPV_ARGS(&reflection))));
    D3D12_SHADER_DESC shaderDesc{};
    ASSERT_TRUE(SUCCEEDED(reflection->GetDesc(&shaderDesc)));
    EXPECT_EQ(shaderDesc.InputParameters, 1U);

    std::vector<uint8_t> firstBytes(
        static_cast<const uint8_t *>(firstBlob.data),
        static_cast<const uint8_t *>(firstBlob.data) + firstBlob.size);
    CCD3D12Shader second;
    second.initialize(info);
    const auto secondBlob = second.getVertexBytecode();
    ASSERT_EQ(secondBlob.size, firstBytes.size());
    EXPECT_TRUE(std::equal(firstBytes.begin(), firstBytes.end(), static_cast<const uint8_t *>(secondBlob.data)));
}

TEST(D3D12RenderPassTest, UsesOnlyCurrentSubpassColorAttachmentsForPipelineState) {
    RenderPassInfo info;
    info.colorAttachments.resize(2);
    info.colorAttachments[0].format = Format::RGBA8;
    info.colorAttachments[0].sampleCount = SampleCount::X4;
    info.colorAttachments[1].format = Format::RGBA8;
    info.colorAttachments[1].sampleCount = SampleCount::X1;

    auto &subpass = info.subpasses.emplace_back();
    subpass.colors.emplace_back(0);
    subpass.resolves = {1};

    CCD3D12RenderPass renderPass;
    renderPass.initialize(info);

    const auto formats = renderPass.getRTVFormats(0);
    ASSERT_EQ(formats.size(), 1U);
    EXPECT_EQ(formats[0], renderPass.getRTVFormats()[0]);
    EXPECT_EQ(renderPass.getSampleCount(0), 4U);
}

TEST(D3D12RenderPassTest, SelectsSampleCountFromRequestedSubpass) {
    RenderPassInfo info;
    info.colorAttachments.resize(2);
    info.colorAttachments[0].format = Format::RGBA8;
    info.colorAttachments[0].sampleCount = SampleCount::X4;
    info.colorAttachments[1].format = Format::RGBA8;
    info.colorAttachments[1].sampleCount = SampleCount::X1;

    info.subpasses.emplace_back().colors.emplace_back(0);
    info.subpasses.emplace_back().colors.emplace_back(1);

    CCD3D12RenderPass renderPass;
    renderPass.initialize(info);

    EXPECT_EQ(renderPass.getSampleCount(0), 4U);
    EXPECT_EQ(renderPass.getSampleCount(1), 1U);
}

TEST(D3D12DescriptorSetTest, NullSamplerWritesValidDefaultDescriptor) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);
    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    ASSERT_NE(d3dDevice, nullptr);

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
        GTEST_SKIP() << "D3D12 debug layer is unavailable";
    }
    infoQueue->ClearStoredMessages();

    DescriptorSetLayoutInfo layoutInfo;
    layoutInfo.bindings.push_back({
        0, DescriptorType::SAMPLER_TEXTURE, 1, ShaderStageFlagBit::FRAGMENT});
    auto *layout = device->createDescriptorSetLayout(layoutInfo);
    auto *descriptorSet = device->createDescriptorSet({layout});
    ASSERT_NE(layout, nullptr);
    ASSERT_NE(descriptorSet, nullptr);

    TextureInfo textureInfo;
    textureInfo.type = TextureType::TEX2D;
    textureInfo.usage = TextureUsageBit::SAMPLED;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 1;
    textureInfo.height = 1;
    auto *texture = device->createTexture(textureInfo);
    ASSERT_NE(texture, nullptr);

    descriptorSet->bindTexture(0, texture);
    descriptorSet->bindSampler(0, nullptr);
    descriptorSet->update();

    bool hasDescriptorError = false;
    const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < messageCount; ++i) {
        SIZE_T messageSize = 0;
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, nullptr, &messageSize)));
        std::vector<uint8_t> storage(messageSize);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, message, &messageSize)));
        hasDescriptorError |= message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                              message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
    }
    EXPECT_FALSE(hasDescriptorError);

    descriptorSet->destroy();
    delete descriptorSet;
    layout->destroy();
    delete layout;
    texture->destroy();
    delete texture;
}

TEST(D3D12DescriptorSetTest, ManySamplerSetsSharePagedCPUStagingHeaps) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);
    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    ASSERT_NE(d3dDevice, nullptr);
    auto *cbvPool = device->getCPUDescriptorHeapPool();
    auto *samplerPool = device->getCPUSamplerDescriptorHeapPool();
    ASSERT_NE(cbvPool, nullptr);
    ASSERT_NE(samplerPool, nullptr);

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
        GTEST_SKIP() << "D3D12 debug layer is unavailable";
    }
    infoQueue->ClearStoredMessages();

    DescriptorSetLayoutInfo layoutInfo;
    layoutInfo.bindings.push_back({
        0, DescriptorType::SAMPLER_TEXTURE, 9, ShaderStageFlagBit::FRAGMENT});
    auto *layout = device->createDescriptorSetLayout(layoutInfo);
    ASSERT_NE(layout, nullptr);

    TextureInfo textureInfo;
    textureInfo.type = TextureType::TEX2D;
    textureInfo.usage = TextureUsageBit::SAMPLED;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 1;
    textureInfo.height = 1;
    auto *texture = device->createTexture(textureInfo);
    ASSERT_NE(texture, nullptr);

    const uint32_t initialCbvHeapCount = cbvPool->getHeapCount();
    const uint32_t initialSamplerHeapCount = samplerPool->getHeapCount();
    std::vector<DescriptorSet *> descriptorSets;
    descriptorSets.reserve(256);
    for (uint32_t i = 0; i < 256; ++i) {
        auto *descriptorSet = device->createDescriptorSet({layout});
        ASSERT_NE(descriptorSet, nullptr);
        descriptorSet->bindTexture(0, texture);
        descriptorSet->update();
        descriptorSets.push_back(descriptorSet);
    }

    EXPECT_LE(cbvPool->getHeapCount(), initialCbvHeapCount + 1U);
    EXPECT_LE(samplerPool->getHeapCount(), initialSamplerHeapCount + 2U);

    bool hasDescriptorError = false;
    const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < messageCount; ++i) {
        SIZE_T messageSize = 0;
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, nullptr, &messageSize)));
        std::vector<uint8_t> storage(messageSize);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, message, &messageSize)));
        hasDescriptorError |= message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                              message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
    }
    EXPECT_FALSE(hasDescriptorError);

    for (auto *descriptorSet : descriptorSets) {
        descriptorSet->destroy();
        delete descriptorSet;
    }
    layout->destroy();
    delete layout;
    texture->destroy();
    delete texture;
}

TEST(D3D12RenderPassTest, ZeroClearMatchesColorAttachmentOptimizedClearValue) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);
    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    ASSERT_NE(d3dDevice, nullptr);

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
        GTEST_SKIP() << "D3D12 debug layer is unavailable";
    }
    infoQueue->ClearStoredMessages();

    TextureInfo textureInfo;
    textureInfo.type = TextureType::TEX2D;
    textureInfo.usage = TextureUsageBit::COLOR_ATTACHMENT;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 16;
    textureInfo.height = 16;
    auto *texture = device->createTexture(textureInfo);
    ASSERT_NE(texture, nullptr);

    RenderPassInfo renderPassInfo;
    renderPassInfo.colorAttachments.resize(1);
    renderPassInfo.colorAttachments[0].format = Format::RGBA8;
    renderPassInfo.colorAttachments[0].loadOp = LoadOp::CLEAR;
    auto *renderPass = device->createRenderPass(renderPassInfo);
    auto *framebuffer = device->createFramebuffer({renderPass, {texture}});
    ASSERT_NE(renderPass, nullptr);
    ASSERT_NE(framebuffer, nullptr);

    auto *commandBuffer = device->getCommandBuffer();
    ASSERT_NE(commandBuffer, nullptr);
    const Color clearColor{0.F, 0.F, 0.F, 0.F};
    commandBuffer->begin();
    commandBuffer->beginRenderPass(renderPass, framebuffer, {0, 0, 16, 16}, &clearColor, 1.F, 0);
    commandBuffer->endRenderPass();
    commandBuffer->end();

    bool mismatchingClearValue = false;
    const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < messageCount; ++i) {
        SIZE_T messageSize = 0;
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, nullptr, &messageSize)));
        std::vector<uint8_t> storage(messageSize);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, message, &messageSize)));
        mismatchingClearValue |= message->ID == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE;
    }
    EXPECT_FALSE(mismatchingClearValue);

    framebuffer->destroy();
    delete framebuffer;
    renderPass->destroy();
    delete renderPass;
    texture->destroy();
    delete texture;
}

TEST(D3D12RenderPassTest, RecordsAndSubmitsColorResolve) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    TextureInfo multisampleInfo;
    multisampleInfo.type = TextureType::TEX2D;
    multisampleInfo.usage = TextureUsageBit::COLOR_ATTACHMENT;
    multisampleInfo.format = Format::RGBA8;
    multisampleInfo.width = 16;
    multisampleInfo.height = 16;
    multisampleInfo.samples = SampleCount::X4;

    TextureInfo resolveInfo = multisampleInfo;
    resolveInfo.samples = SampleCount::X1;
    auto *multisampleTexture = device->createTexture(multisampleInfo);
    auto *resolveTexture = device->createTexture(resolveInfo);
    ASSERT_NE(multisampleTexture, nullptr);
    ASSERT_NE(resolveTexture, nullptr);

    RenderPassInfo renderPassInfo;
    renderPassInfo.colorAttachments.resize(2);
    renderPassInfo.colorAttachments[0].format = Format::RGBA8;
    renderPassInfo.colorAttachments[0].sampleCount = SampleCount::X4;
    renderPassInfo.colorAttachments[0].loadOp = LoadOp::CLEAR;
    renderPassInfo.colorAttachments[0].storeOp = StoreOp::DISCARD;
    renderPassInfo.colorAttachments[1].format = Format::RGBA8;
    renderPassInfo.colorAttachments[1].sampleCount = SampleCount::X1;
    renderPassInfo.colorAttachments[1].loadOp = LoadOp::DISCARD;
    renderPassInfo.colorAttachments[1].storeOp = StoreOp::STORE;
    auto &subpass = renderPassInfo.subpasses.emplace_back();
    subpass.colors = {0};
    subpass.resolves = {1};

    auto *renderPass = device->createRenderPass(renderPassInfo);
    auto *framebuffer = device->createFramebuffer({renderPass, {multisampleTexture, resolveTexture}});
    ASSERT_NE(renderPass, nullptr);
    ASSERT_NE(framebuffer, nullptr);

    auto *commandBuffer = device->getCommandBuffer();
    ASSERT_NE(commandBuffer, nullptr);
    ColorList clearColors(2);
    clearColors[0] = {0.F, 0.F, 0.F, 1.F};
    clearColors[1] = {0.F, 0.F, 0.F, 1.F};
    commandBuffer->begin();
    commandBuffer->beginRenderPass(renderPass, framebuffer, {0, 0, 16, 16}, clearColors, 1.F, 0);
    commandBuffer->endRenderPass();
    commandBuffer->end();
    CommandBuffer *commandBuffers[] = {commandBuffer};
    device->getQueue()->submit(commandBuffers, 1);

    EXPECT_EQ(static_cast<CCD3D12Texture *>(multisampleTexture)->getCurrentState(),
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    EXPECT_EQ(static_cast<CCD3D12Texture *>(resolveTexture)->getCurrentState(),
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    framebuffer->destroy();
    delete framebuffer;
    renderPass->destroy();
    delete renderPass;
    multisampleTexture->destroy();
    delete multisampleTexture;
    resolveTexture->destroy();
    delete resolveTexture;
}

} // namespace gfx
} // namespace cc

#endif
