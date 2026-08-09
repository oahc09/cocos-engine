#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#if CC_USE_D3D12

    #include <d3dcompiler.h>
    #include <d3d12sdklayers.h>
    #include <wrl/client.h>
    #include "cocos/renderer/gfx-d3d12/D3D12Buffer.h"
    #include "cocos/renderer/gfx-d3d12/D3D12CommandBuffer.h"
    #include "cocos/renderer/gfx-d3d12/D3D12DescriptorSet.h"
    #include "cocos/renderer/gfx-d3d12/D3D12DescriptorHeapPool.h"
    #include "cocos/renderer/gfx-d3d12/D3D12Framebuffer.h"
    #include "cocos/renderer/gfx-d3d12/D3D12InputAssembler.h"
    #include "cocos/renderer/gfx-d3d12/D3D12PipelineLayout.h"
    #include "cocos/renderer/gfx-d3d12/D3D12PipelineState.h"
    #include "cocos/renderer/gfx-d3d12/D3D12ShaderCacheScheduler.h"
    #include "cocos/renderer/gfx-d3d12/D3D12ShaderCompileScheduler.h"
    #include "cocos/renderer/gfx-d3d12/D3D12Shader.h"
    #include "cocos/renderer/gfx-d3d12/D3D12RenderPass.h"
    #include "cocos/renderer/gfx-d3d12/D3D12ResourceState.h"
    #include "cocos/renderer/gfx-d3d12/D3D12Swapchain.h"
    #include "cocos/renderer/gfx-d3d12/D3D12Device.h"
    #include "cocos/renderer/gfx-d3d12/D3D12Texture.h"
    #include "cocos/renderer/gfx-validator/BufferValidator.h"
    #include "cocos/renderer/gfx-validator/ValidationUtils.h"

namespace cc {
namespace gfx {

namespace {

class ResizeResultBuffer final : public Buffer {
public:
    void update(const void * /*buffer*/, uint32_t /*size*/) override {}
    void setResizeResult(bool result) { _resizeResult = result; }

protected:
    void doInit(const BufferInfo & /*info*/) override {}
    void doInit(const BufferViewInfo & /*info*/) override {}
    bool doResize(uint32_t /*size*/, uint32_t /*count*/) override { return _resizeResult; }
    void doDestroy() override {}

private:
    bool _resizeResult{true};
};

} // namespace

TEST(GFXBufferResizeTest, PreservesLogicalSizeWhenBackendResizeFails) {
    ResizeResultBuffer buffer;
    BufferInfo info{};
    info.usage = BufferUsageBit::VERTEX;
    info.memUsage = MemoryUsageBit::DEVICE;
    info.size = 64;
    info.stride = 4;
    buffer.initialize(info);

    buffer.setResizeResult(false);
    EXPECT_FALSE(buffer.resize(128));

    EXPECT_EQ(buffer.getSize(), 64U);
    EXPECT_EQ(buffer.getCount(), 16U);
}

TEST(GFXBufferResizeTest, ValidatorPropagatesBackendResizeFailure) {
    auto *actor = ccnew ResizeResultBuffer;
    BufferValidator buffer(actor);
    DeviceResourceTracker<Buffer>::push(static_cast<Buffer *>(&buffer));
    BufferInfo info{};
    info.usage = BufferUsageBit::VERTEX;
    info.memUsage = MemoryUsageBit::DEVICE;
    info.size = 64;
    info.stride = 4;
    buffer.initialize(info);

    actor->setResizeResult(false);
    EXPECT_FALSE(buffer.resize(128));
    EXPECT_EQ(buffer.getSize(), 64U);
    EXPECT_EQ(actor->getSize(), 64U);

    buffer.destroy();
}

TEST(D3D12ResourceStateTest, TracksSparseSubresourcesAndCollapsesUniformState) {
    D3D12ResourceState states(4, D3D12_RESOURCE_STATE_COMMON);
    D3D12_RESOURCE_STATES uniform{};
    ASSERT_TRUE(states.tryGetUniform(uniform));
    EXPECT_EQ(uniform, D3D12_RESOURCE_STATE_COMMON);

    states.set(2, D3D12_RESOURCE_STATE_COPY_DEST);
    EXPECT_FALSE(states.tryGetUniform(uniform));
    EXPECT_EQ(states.get(0), D3D12_RESOURCE_STATE_COMMON);
    EXPECT_EQ(states.get(2), D3D12_RESOURCE_STATE_COPY_DEST);

    states.set(0, D3D12_RESOURCE_STATE_COPY_DEST);
    states.set(1, D3D12_RESOURCE_STATE_COPY_DEST);
    states.set(3, D3D12_RESOURCE_STATE_COPY_DEST);
    ASSERT_TRUE(states.tryGetUniform(uniform));
    EXPECT_EQ(uniform, D3D12_RESOURCE_STATE_COPY_DEST);
}

TEST(D3D12ResourceStateTest, UsesStandardMipArrayPlaneIndexing) {
    D3D12ResourceBacking backing;
    backing.mipLevels = 4;
    backing.arraySize = 3;
    backing.planeCount = 2;

    EXPECT_EQ(backing.subresourceCount(), 24U);
    EXPECT_EQ(backing.subresourceIndex(2, 1, 1), 18U);
}

TEST(D3D12ResourceStateTest, MixedTransitionRangeStillRequiresUavOrderingBarrier) {
    EXPECT_TRUE(needsD3D12UavOrderingBarrier(
        true, true, false, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    EXPECT_TRUE(needsD3D12UavOrderingBarrier(
        true, false, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    EXPECT_FALSE(needsD3D12UavOrderingBarrier(
        false, true, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    EXPECT_FALSE(needsD3D12UavOrderingBarrier(
        true, true, true, D3D12_RESOURCE_STATE_COPY_DEST));
}

TEST(D3D12ResourceStateTest, RecordedJournalSurvivesOwnerBackingInvalidation) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    TextureInfo textureInfo{};
    textureInfo.type = TextureType::TEX2D;
    textureInfo.usage = TextureUsageBit::SAMPLED | TextureUsageBit::TRANSFER_DST;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 8;
    textureInfo.height = 8;
    auto *texture = static_cast<CCD3D12Texture *>(device->createTexture(textureInfo));
    ASSERT_NE(texture, nullptr);

    const auto backing = texture->getD3D12ResourceBacking();
    ASSERT_NE(backing, nullptr);

    D3D12ResourceStateJournal journal;
    D3D12_RESOURCE_BARRIER barrier{};
    ASSERT_TRUE(journal.transition(
        backing, backing->resource.Get(), 0,
        D3D12_RESOURCE_STATE_COPY_DEST, barrier));

    texture->resize(16, 16);
    ASSERT_FALSE(backing->valid);
    std::vector<D3D12_RESOURCE_BARRIER> fixups;
    EXPECT_TRUE(journal.appendSubmissionFixupBarriers(fixups));
    EXPECT_TRUE(journal.commit());
    EXPECT_EQ(backing->states.get(0), D3D12_RESOURCE_STATE_COPY_DEST);

    CC_SAFE_DESTROY_AND_DELETE(texture);
}

TEST(D3D12ResourceStateTest, TextureViewSharesOwnerBackingAndRangeState) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    TextureInfo textureInfo{};
    textureInfo.type = TextureType::TEX2D_ARRAY;
    textureInfo.usage = TextureUsageBit::SAMPLED | TextureUsageBit::TRANSFER_SRC |
                        TextureUsageBit::TRANSFER_DST;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 8;
    textureInfo.height = 8;
    textureInfo.layerCount = 2;
    textureInfo.levelCount = 2;

    auto *owner = static_cast<CCD3D12Texture *>(device->createTexture(textureInfo));
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(owner->getD3D12OwnedResourceHandle(), nullptr);

    TextureViewInfo viewInfo{};
    viewInfo.texture = owner;
    viewInfo.type = TextureType::TEX2D;
    viewInfo.format = Format::RGBA8;
    viewInfo.baseLevel = 1;
    viewInfo.levelCount = 1;
    viewInfo.baseLayer = 1;
    viewInfo.layerCount = 1;
    viewInfo.basePlane = 0;
    viewInfo.planeCount = 1;

    auto *view = static_cast<CCD3D12Texture *>(device->createTexture(viewInfo));
    ASSERT_NE(view, nullptr);
    ASSERT_EQ(owner->getD3D12ResourceBacking(), view->getD3D12ResourceBacking());

    const auto backing = owner->getD3D12ResourceBacking();
    const uint32_t selected = backing->subresourceIndex(1, 1, 0);
    view->setCurrentState(D3D12_RESOURCE_STATE_COPY_SOURCE);
    EXPECT_EQ(owner->getSubresourceState(selected), D3D12_RESOURCE_STATE_COPY_SOURCE);
    EXPECT_EQ(owner->getSubresourceState(0), D3D12_RESOURCE_STATE_COMMON);

    CC_SAFE_DESTROY_AND_DELETE(view);
    CC_SAFE_DESTROY_AND_DELETE(owner);
}

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

TEST(D3D12DescriptorSetTest, InputAttachmentOccupiesOneSrvSlot) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    DescriptorSetLayoutInfo layoutInfo;
    layoutInfo.bindings.push_back({
        0, DescriptorType::INPUT_ATTACHMENT, 1, ShaderStageFlagBit::FRAGMENT});
    auto *layout = device->createDescriptorSetLayout(layoutInfo);
    ASSERT_NE(layout, nullptr);
    auto *descriptorSet = static_cast<CCD3D12DescriptorSet *>(
        device->createDescriptorSet({layout}));
    ASSERT_NE(descriptorSet, nullptr);

    EXPECT_EQ(descriptorSet->getCbvSrvUavDescriptorCount(), 1U);

    descriptorSet->destroy();
    delete descriptorSet;
    layout->destroy();
    delete layout;
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
    ASSERT_TRUE(device->waitIdle());

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

TEST(D3D12FramebufferTest, RejectsMismatchedAttachmentsWithoutSubstitution) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    TextureInfo firstInfo;
    firstInfo.type = TextureType::TEX2D;
    firstInfo.usage = TextureUsageBit::COLOR_ATTACHMENT;
    firstInfo.format = Format::RGBA8;
    firstInfo.width = 16;
    firstInfo.height = 16;
    TextureInfo secondInfo = firstInfo;
    secondInfo.width = 8;
    secondInfo.height = 8;
    auto *first = device->createTexture(firstInfo);
    auto *second = device->createTexture(secondInfo);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    RenderPassInfo renderPassInfo;
    renderPassInfo.colorAttachments.resize(2);
    renderPassInfo.colorAttachments[0].format = Format::RGBA8;
    renderPassInfo.colorAttachments[1].format = Format::RGBA8;
    auto *renderPass = device->createRenderPass(renderPassInfo);
    auto *framebuffer = static_cast<CCD3D12Framebuffer *>(
        device->createFramebuffer({renderPass, {first, second}}));
    ASSERT_NE(renderPass, nullptr);
    ASSERT_NE(framebuffer, nullptr);

    EXPECT_FALSE(framebuffer->isValid());
    EXPECT_EQ(
        framebuffer->getColorBacking(0),
        static_cast<CCD3D12Texture *>(first)->getD3D12ResourceBacking());
    EXPECT_EQ(
        framebuffer->getColorBacking(1),
        static_cast<CCD3D12Texture *>(second)->getD3D12ResourceBacking());

    framebuffer->destroy();
    delete framebuffer;
    renderPass->destroy();
    delete renderPass;
    first->destroy();
    delete first;
    second->destroy();
    delete second;
}

TEST(D3D12FramebufferTest, RejectsRenderPassColorAttachmentCountMismatch) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    TextureInfo textureInfo;
    textureInfo.type = TextureType::TEX2D;
    textureInfo.usage = TextureUsageBit::COLOR_ATTACHMENT;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 16;
    textureInfo.height = 16;
    auto *first = device->createTexture(textureInfo);
    auto *second = device->createTexture(textureInfo);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    RenderPassInfo renderPassInfo;
    renderPassInfo.colorAttachments.resize(1);
    renderPassInfo.colorAttachments[0].format = Format::RGBA8;
    auto *renderPass = device->createRenderPass(renderPassInfo);
    auto *framebuffer = static_cast<CCD3D12Framebuffer *>(
        device->createFramebuffer({renderPass, {first, second}}));
    ASSERT_NE(renderPass, nullptr);
    ASSERT_NE(framebuffer, nullptr);

    EXPECT_FALSE(framebuffer->isValid());

    framebuffer->destroy();
    delete framebuffer;
    renderPass->destroy();
    delete renderPass;
    first->destroy();
    delete first;
    second->destroy();
    delete second;
}

TEST(D3D12FramebufferTest, TextureViewRTVTargetsRequestedMipAndLayer) {
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
    textureInfo.type = TextureType::TEX2D_ARRAY;
    textureInfo.usage = TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::TRANSFER_SRC;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 8;
    textureInfo.height = 8;
    textureInfo.layerCount = 2;
    textureInfo.levelCount = 2;
    auto *owner = device->createTexture(textureInfo);
    ASSERT_NE(owner, nullptr);

    TextureViewInfo viewInfo;
    viewInfo.texture = owner;
    viewInfo.type = TextureType::TEX2D;
    viewInfo.format = Format::RGBA8;
    viewInfo.baseLevel = 1;
    viewInfo.levelCount = 1;
    viewInfo.baseLayer = 1;
    viewInfo.layerCount = 1;
    auto *view = device->createTexture(viewInfo);
    ASSERT_NE(view, nullptr);

    RenderPassInfo renderPassInfo;
    renderPassInfo.colorAttachments.resize(1);
    renderPassInfo.colorAttachments[0].format = Format::RGBA8;
    renderPassInfo.colorAttachments[0].loadOp = LoadOp::CLEAR;
    renderPassInfo.colorAttachments[0].storeOp = StoreOp::STORE;
    auto *renderPass = device->createRenderPass(renderPassInfo);
    auto *framebuffer = static_cast<CCD3D12Framebuffer *>(
        device->createFramebuffer({renderPass, {view}}));
    ASSERT_NE(renderPass, nullptr);
    ASSERT_NE(framebuffer, nullptr);
    ASSERT_TRUE(framebuffer->isValid());
    EXPECT_EQ(framebuffer->getWidth(), 4U);
    EXPECT_EQ(framebuffer->getHeight(), 4U);

    const Color clearColor{1.F, 0.F, 0.F, 1.F};
    auto *commandBuffer = device->getCommandBuffer();
    commandBuffer->begin();
    commandBuffer->beginRenderPass(
        renderPass, framebuffer, {0, 0, 4, 4}, &clearColor, 1.F, 0);
    commandBuffer->endRenderPass();
    commandBuffer->end();
    CommandBuffer *commandBuffers[] = {commandBuffer};
    device->getQueue()->submit(commandBuffers, 1);

    std::vector<uint8_t> pixels(4U * 4U * 4U);
    uint8_t *readbackBuffers[] = {pixels.data()};
    BufferTextureCopy readbackRegion;
    readbackRegion.texExtent = {4, 4, 1};
    static_cast<Device *>(device)->copyTextureToBuffers(
        view, readbackBuffers, &readbackRegion, 1);
    EXPECT_EQ(pixels[0], 255U);
    EXPECT_EQ(pixels[1], 0U);
    EXPECT_EQ(pixels[2], 0U);
    EXPECT_EQ(pixels[3], 255U);

    bool hasValidationError = false;
    const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < messageCount; ++i) {
        SIZE_T messageSize = 0;
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, nullptr, &messageSize)));
        std::vector<uint8_t> storage(messageSize);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, message, &messageSize)));
        hasValidationError |= message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                              message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
    }
    EXPECT_FALSE(hasValidationError);

    framebuffer->destroy();
    delete framebuffer;
    renderPass->destroy();
    delete renderPass;
    view->destroy();
    delete view;
    owner->destroy();
    delete owner;
}

TEST(D3D12FramebufferTest, RetainsAttachmentBackingAfterActorRelease) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    TextureInfo textureInfo;
    textureInfo.type = TextureType::TEX2D;
    textureInfo.usage = TextureUsageBit::COLOR_ATTACHMENT;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 8;
    textureInfo.height = 8;
    auto *texture = device->createTexture(textureInfo);
    ASSERT_NE(texture, nullptr);

    RenderPassInfo renderPassInfo;
    renderPassInfo.colorAttachments.resize(1);
    renderPassInfo.colorAttachments[0].format = Format::RGBA8;
    auto *renderPass = device->createRenderPass(renderPassInfo);
    auto *framebuffer = static_cast<CCD3D12Framebuffer *>(
        device->createFramebuffer({renderPass, {texture}}));
    ASSERT_NE(renderPass, nullptr);
    ASSERT_NE(framebuffer, nullptr);
    auto *resource = static_cast<CCD3D12Texture *>(texture)->getD3D12ResourceHandle();
    ASSERT_NE(resource, nullptr);

    texture->destroy();
    delete texture;
    EXPECT_TRUE(framebuffer->isValid());
    EXPECT_EQ(framebuffer->getColorResource(0), resource);

    framebuffer->destroy();
    delete framebuffer;
    renderPass->destroy();
    delete renderPass;
}

TEST(D3D12FramebufferTest, AttachmentResizeInvalidatesFramebufferSnapshot) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    TextureInfo textureInfo;
    textureInfo.type = TextureType::TEX2D;
    textureInfo.usage = TextureUsageBit::COLOR_ATTACHMENT;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 8;
    textureInfo.height = 8;
    auto *texture = static_cast<CCD3D12Texture *>(
        device->createTexture(textureInfo));
    ASSERT_NE(texture, nullptr);

    RenderPassInfo renderPassInfo;
    renderPassInfo.colorAttachments.resize(1);
    renderPassInfo.colorAttachments[0].format = Format::RGBA8;
    auto *renderPass = device->createRenderPass(renderPassInfo);
    auto *framebuffer = static_cast<CCD3D12Framebuffer *>(
        device->createFramebuffer({renderPass, {texture}}));
    ASSERT_NE(renderPass, nullptr);
    ASSERT_NE(framebuffer, nullptr);
    ASSERT_TRUE(framebuffer->isValid());

    const auto oldBacking = texture->getD3D12ResourceBacking();
    ASSERT_NE(oldBacking, nullptr);
    texture->resize(16, 16);

    const auto newBacking = texture->getD3D12ResourceBacking();
    ASSERT_NE(newBacking, nullptr);
    EXPECT_NE(newBacking, oldBacking);
    EXPECT_FALSE(oldBacking->valid);
    EXPECT_FALSE(framebuffer->isValid());
    EXPECT_EQ(framebuffer->getColorResource(0), nullptr);

    framebuffer->destroy();
    delete framebuffer;
    renderPass->destroy();
    delete renderPass;
    texture->destroy();
    delete texture;
}

TEST(D3D12SwapchainTest, FramebufferAndSubmittedContextReleaseBackBufferForResize) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    HWND window = CreateWindowExW(
        0, L"STATIC", L"D3D12 swapchain resize test", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 64, 64,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(window, nullptr);

    CCD3D12Swapchain swapchain;
    SwapchainInfo swapchainInfo;
    swapchainInfo.windowHandle = window;
    swapchainInfo.width = 64;
    swapchainInfo.height = 64;
    swapchain.initialize(swapchainInfo);
    ASSERT_TRUE(swapchain.isReady());

    auto backBuffer = swapchain.getCurrentBackBufferBacking();
    ASSERT_NE(backBuffer, nullptr);
    const long ownersBeforeView = backBuffer.use_count();

    TextureViewInfo viewInfo;
    viewInfo.texture = swapchain.getColorTexture();
    viewInfo.type = TextureType::TEX2D;
    viewInfo.format = Format::RGBA8;
    viewInfo.baseLevel = 0;
    viewInfo.levelCount = 1;
    viewInfo.baseLayer = 0;
    viewInfo.layerCount = 1;
    CCD3D12Texture swapchainView;
    swapchainView.initialize(viewInfo);
    EXPECT_TRUE(swapchainView.isSwapchainColorTexture());
    EXPECT_EQ(backBuffer.use_count(), ownersBeforeView);

    const long ownersBeforeFramebuffer = backBuffer.use_count();

    RenderPassInfo renderPassInfo;
    renderPassInfo.colorAttachments.resize(1);
    renderPassInfo.colorAttachments[0].format = Format::RGBA8;
    CCD3D12RenderPass renderPass;
    renderPass.initialize(renderPassInfo);

    CCD3D12Framebuffer framebuffer;
    framebuffer.initialize({&renderPass, {swapchain.getColorTexture()}});
    ASSERT_TRUE(framebuffer.isValid());
    EXPECT_EQ(backBuffer.use_count(), ownersBeforeFramebuffer);
    backBuffer.reset();

    auto *commandBuffer = device->getCommandBuffer();
    ASSERT_NE(commandBuffer, nullptr);
    const Color clearColor{0.F, 0.F, 0.F, 1.F};
    commandBuffer->begin();
    commandBuffer->beginRenderPass(
        &renderPass, &framebuffer, {0, 0, 64, 64}, &clearColor, 1.F, 0);
    commandBuffer->endRenderPass();
    commandBuffer->end();
    CommandBuffer *commandBuffers[] = {commandBuffer};
    device->getQueue()->submit(commandBuffers, 1);

    swapchain.resize(96, 80, SurfaceTransform::IDENTITY);
    EXPECT_TRUE(swapchain.isReady());
    EXPECT_EQ(swapchain.getColorTexture()->getWidth(), 96U);
    EXPECT_EQ(swapchain.getColorTexture()->getHeight(), 80U);
    EXPECT_EQ(swapchainView.getD3D12ResourceHandle(), swapchain.getCurrentBackBufferHandle());
    EXPECT_TRUE(framebuffer.isValid());
    EXPECT_EQ(static_cast<CCD3D12CommandBuffer *>(commandBuffer)->getD3D12RetainedResourceCount(), 0U);

    framebuffer.destroy();
    renderPass.destroy();
    swapchainView.destroy();
    swapchain.destroy();
    DestroyWindow(window);
}

TEST(D3D12TextureTest, OwnerResizeInvalidatesExistingViews) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    TextureInfo textureInfo;
    textureInfo.type = TextureType::TEX2D;
    textureInfo.usage = TextureUsageBit::SAMPLED;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 8;
    textureInfo.height = 8;
    auto *owner = static_cast<CCD3D12Texture *>(
        device->createTexture(textureInfo));
    ASSERT_NE(owner, nullptr);

    TextureViewInfo viewInfo;
    viewInfo.texture = owner;
    viewInfo.type = TextureType::TEX2D;
    viewInfo.format = Format::RGBA8;
    viewInfo.levelCount = 1;
    viewInfo.layerCount = 1;
    viewInfo.planeCount = 1;
    auto *view = static_cast<CCD3D12Texture *>(
        device->createTexture(viewInfo));
    ASSERT_NE(view, nullptr);
    ASSERT_NE(view->getD3D12ResourceHandle(), nullptr);

    owner->resize(16, 16);

    EXPECT_NE(owner->getD3D12ResourceHandle(), nullptr);
    EXPECT_EQ(view->getD3D12ResourceHandle(), nullptr);
    EXPECT_EQ(view->getD3D12ResourceBacking(), nullptr);

    view->destroy();
    delete view;
    owner->destroy();
    delete owner;
}

TEST(D3D12DescriptorSetTest, TextureResizeRefreshesSrvDescriptor) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    DescriptorSetLayoutInfo layoutInfo;
    layoutInfo.bindings.push_back({
        0, DescriptorType::TEXTURE, 1, ShaderStageFlagBit::FRAGMENT});
    auto *layout = device->createDescriptorSetLayout(layoutInfo);
    auto *descriptorSet = static_cast<CCD3D12DescriptorSet *>(
        device->createDescriptorSet({layout}));
    ASSERT_NE(layout, nullptr);
    ASSERT_NE(descriptorSet, nullptr);

    TextureInfo textureInfo;
    textureInfo.type = TextureType::TEX2D;
    textureInfo.usage = TextureUsageBit::SAMPLED;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 8;
    textureInfo.height = 8;
    auto *texture = static_cast<CCD3D12Texture *>(
        device->createTexture(textureInfo));
    ASSERT_NE(texture, nullptr);

    descriptorSet->bindTexture(0, texture);
    descriptorSet->update();
    const uint64_t oldVersion = descriptorSet->getVersion();
    ccstd::vector<void *> oldResources;
    ccstd::vector<std::shared_ptr<void>> oldBufferBackings;
    descriptorSet->collectBoundD3D12Resources(oldResources, oldBufferBackings);
    ASSERT_EQ(oldResources.size(), 1U);

    texture->resize(16, 16);
    descriptorSet->update();

    ccstd::vector<void *> newResources;
    ccstd::vector<std::shared_ptr<void>> newBufferBackings;
    descriptorSet->collectBoundD3D12Resources(newResources, newBufferBackings);
    ASSERT_EQ(newResources.size(), 1U);
    EXPECT_NE(newResources[0], oldResources[0]);
    EXPECT_GT(descriptorSet->getVersion(), oldVersion);

    descriptorSet->destroy();
    delete descriptorSet;
    layout->destroy();
    delete layout;
    texture->destroy();
    delete texture;
}

TEST(D3D12BufferTest, ViewRetainsNativeBackingAfterOwnerDestroy) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    BufferInfo bufferInfo;
    bufferInfo.usage = BufferUsageBit::VERTEX;
    bufferInfo.memUsage = MemoryUsageBit::DEVICE;
    bufferInfo.size = 512;
    bufferInfo.stride = 16;
    auto *owner = static_cast<CCD3D12Buffer *>(device->createBuffer(bufferInfo));
    ASSERT_NE(owner, nullptr);
    auto *resource = owner->getD3D12ResourceHandle();
    ASSERT_NE(resource, nullptr);

    BufferViewInfo viewInfo;
    viewInfo.buffer = owner;
    viewInfo.offset = 128;
    viewInfo.range = 128;
    auto *view = static_cast<CCD3D12Buffer *>(device->createBuffer(viewInfo));
    ASSERT_NE(view, nullptr);

    owner->destroy();
    delete owner;
    EXPECT_EQ(view->getD3D12ResourceHandle(), resource);
    EXPECT_EQ(view->getD3D12ResourceOffset(), 128U);

    view->destroy();
    delete view;
}

TEST(D3D12BufferTest, RejectsViewsOutsideParentRangeWithoutOffsetOverflow) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    BufferInfo bufferInfo;
    bufferInfo.usage = BufferUsageBit::VERTEX;
    bufferInfo.memUsage = MemoryUsageBit::DEVICE;
    bufferInfo.size = 64;
    bufferInfo.stride = 16;
    auto *owner = static_cast<CCD3D12Buffer *>(
        device->createBuffer(bufferInfo));
    ASSERT_NE(owner, nullptr);

    BufferViewInfo outOfRangeInfo;
    outOfRangeInfo.buffer = owner;
    outOfRangeInfo.offset = 48;
    outOfRangeInfo.range = 32;
    auto *outOfRange = static_cast<CCD3D12Buffer *>(
        device->createBuffer(outOfRangeInfo));
    ASSERT_NE(outOfRange, nullptr);
    EXPECT_EQ(outOfRange->getD3D12ResourceHandle(), nullptr);

    BufferViewInfo validViewInfo;
    validViewInfo.buffer = owner;
    validViewInfo.offset = 16;
    validViewInfo.range = 32;
    auto *validView = static_cast<CCD3D12Buffer *>(
        device->createBuffer(validViewInfo));
    ASSERT_NE(validView, nullptr);
    ASSERT_NE(validView->getD3D12ResourceHandle(), nullptr);

    BufferViewInfo overflowingInfo;
    overflowingInfo.buffer = validView;
    overflowingInfo.offset = std::numeric_limits<uint32_t>::max();
    overflowingInfo.range = 1;
    auto *overflowing = static_cast<CCD3D12Buffer *>(
        device->createBuffer(overflowingInfo));
    ASSERT_NE(overflowing, nullptr);
    EXPECT_EQ(overflowing->getD3D12ResourceHandle(), nullptr);

    overflowing->destroy();
    delete overflowing;
    validView->destroy();
    delete validView;
    outOfRange->destroy();
    delete outOfRange;
    owner->destroy();
    delete owner;
}

TEST(D3D12VertexFormatTest, MapsPackedUnsignedTenBitFormatExactly) {
    EXPECT_EQ(
        toD3D12VertexFormat(Format::RGB10A2UI),
        DXGI_FORMAT_R10G10B10A2_UINT);
    EXPECT_EQ(toD3D12VertexFormat(Format::RGB8), DXGI_FORMAT_UNKNOWN);
}

TEST(D3D12ResolveTest, ExplicitResolveUsesTypedCompatibleSubresources) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);
    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    ASSERT_NE(d3dDevice, nullptr);

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
        GTEST_SKIP() << "D3D12 debug layer is unavailable";
    }
    infoQueue->ClearStoredMessages();

    TextureInfo sourceInfo;
    sourceInfo.type = TextureType::TEX2D;
    sourceInfo.usage = TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::TRANSFER_SRC;
    sourceInfo.format = Format::RGBA8;
    sourceInfo.width = 16;
    sourceInfo.height = 16;
    sourceInfo.samples = SampleCount::X4;
    TextureInfo destinationInfo = sourceInfo;
    destinationInfo.usage = TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::TRANSFER_DST;
    destinationInfo.samples = SampleCount::X1;
    auto *source = device->createTexture(sourceInfo);
    auto *destination = device->createTexture(destinationInfo);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(destination, nullptr);

    TextureCopy region;
    region.extent = {16, 16, 1};
    auto *commandBuffer = device->getCommandBuffer();
    ASSERT_NE(commandBuffer, nullptr);
    commandBuffer->begin();
    commandBuffer->resolveTexture(source, destination, &region, 1);
    commandBuffer->end();
    EXPECT_NE(
        static_cast<CCD3D12CommandBuffer *>(commandBuffer)->getD3D12CommandList(),
        nullptr);
    CommandBuffer *commandBuffers[] = {commandBuffer};
    device->getQueue()->submit(commandBuffers, 1);
    ASSERT_TRUE(device->waitIdle());

    bool hasValidationError = false;
    const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < messageCount; ++i) {
        SIZE_T messageSize = 0;
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, nullptr, &messageSize)));
        std::vector<uint8_t> storage(messageSize);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, message, &messageSize)));
        hasValidationError |= message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                              message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
    }
    EXPECT_FALSE(hasValidationError);

    source->destroy();
    delete source;
    destination->destroy();
    delete destination;
}

TEST(D3D12ResolveTest, RejectsUnsupportedDepthStencilResolveWithoutRecordingCommands) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    TextureInfo sourceInfo;
    sourceInfo.type = TextureType::TEX2D;
    sourceInfo.usage =
        TextureUsageBit::DEPTH_STENCIL_ATTACHMENT | TextureUsageBit::TRANSFER_SRC;
    sourceInfo.format = Format::DEPTH_STENCIL;
    sourceInfo.width = 16;
    sourceInfo.height = 16;
    sourceInfo.samples = SampleCount::X4;
    TextureInfo destinationInfo = sourceInfo;
    destinationInfo.usage =
        TextureUsageBit::DEPTH_STENCIL_ATTACHMENT | TextureUsageBit::TRANSFER_DST;
    destinationInfo.samples = SampleCount::X1;
    auto *source = static_cast<CCD3D12Texture *>(
        device->createTexture(sourceInfo));
    auto *destination = static_cast<CCD3D12Texture *>(
        device->createTexture(destinationInfo));
    ASSERT_NE(source, nullptr);
    ASSERT_NE(destination, nullptr);

    TextureCopy region;
    region.extent = {16, 16, 1};
    auto *commandBuffer = device->getCommandBuffer();
    ASSERT_NE(commandBuffer, nullptr);
    commandBuffer->begin();
    commandBuffer->resolveTexture(source, destination, &region, 1);
    commandBuffer->end();
    EXPECT_NE(
        static_cast<CCD3D12CommandBuffer *>(commandBuffer)->getD3D12CommandList(),
        nullptr);
    CommandBuffer *commandBuffers[] = {commandBuffer};
    device->getQueue()->submit(commandBuffers, 1);
    ASSERT_TRUE(device->waitIdle());

    const auto sourceBacking = source->getD3D12ResourceBacking();
    const auto destinationBacking = destination->getD3D12ResourceBacking();
    ASSERT_NE(sourceBacking, nullptr);
    ASSERT_NE(destinationBacking, nullptr);
    for (uint32_t subresource = 0;
         subresource < sourceBacking->subresourceCount();
         ++subresource) {
        EXPECT_EQ(
            sourceBacking->states.get(subresource),
            D3D12_RESOURCE_STATE_COMMON);
    }
    for (uint32_t subresource = 0;
         subresource < destinationBacking->subresourceCount();
         ++subresource) {
        EXPECT_EQ(
            destinationBacking->states.get(subresource),
            D3D12_RESOURCE_STATE_COMMON);
    }

    source->destroy();
    delete source;
    destination->destroy();
    delete destination;
}

TEST(D3D12ResourceStateTest, PartialBarrierThenCopyUsesExactTrackedSubresources) {
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
    textureInfo.type = TextureType::TEX2D_ARRAY;
    textureInfo.usage = TextureUsageBit::SAMPLED | TextureUsageBit::TRANSFER_SRC |
                        TextureUsageBit::TRANSFER_DST;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 8;
    textureInfo.height = 8;
    textureInfo.layerCount = 2;
    textureInfo.levelCount = 2;
    auto *source = device->createTexture(textureInfo);
    auto *destination = device->createTexture(textureInfo);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(destination, nullptr);

    TextureBarrierInfo barrierInfo;
    barrierInfo.nextAccesses = AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE;
    barrierInfo.range.firstSlice = 1;
    barrierInfo.range.numSlices = 1;
    barrierInfo.range.mipLevel = 1;
    barrierInfo.range.levelCount = 1;
    barrierInfo.range.basePlane = 0;
    barrierInfo.range.planeCount = 1;
    const TextureBarrier *textureBarriers[] = {device->getTextureBarrier(barrierInfo)};
    const Texture *textures[] = {source};

    TextureCopy region;
    region.srcSubres.mipLevel = 1;
    region.srcSubres.baseArrayLayer = 1;
    region.dstSubres.mipLevel = 1;
    region.dstSubres.baseArrayLayer = 1;
    region.extent = {4, 4, 1};

    auto *commandBuffer = device->getCommandBuffer();
    ASSERT_NE(commandBuffer, nullptr);
    commandBuffer->begin();
    commandBuffer->pipelineBarrier(
        nullptr, nullptr, nullptr, 0, textureBarriers, textures, 1);
    commandBuffer->copyTexture(source, destination, &region, 1);
    commandBuffer->end();
    CommandBuffer *commandBuffers[] = {commandBuffer};
    device->getQueue()->submit(commandBuffers, 1);
    ASSERT_TRUE(device->waitIdle());

    const auto sourceBacking =
        static_cast<CCD3D12Texture *>(source)->getD3D12ResourceBacking();
    const auto destinationBacking =
        static_cast<CCD3D12Texture *>(destination)->getD3D12ResourceBacking();
    ASSERT_NE(sourceBacking, nullptr);
    ASSERT_NE(destinationBacking, nullptr);
    const uint32_t copiedSubresource = sourceBacking->subresourceIndex(1, 1, 0);
    const auto sampledState = static_cast<D3D12_RESOURCE_STATES>(
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    EXPECT_EQ(sourceBacking->states.get(copiedSubresource),
              sampledState);
    EXPECT_EQ(sourceBacking->states.get(0), D3D12_RESOURCE_STATE_COMMON);
    EXPECT_EQ(destinationBacking->states.get(copiedSubresource),
              sampledState);
    EXPECT_EQ(destinationBacking->states.get(0), D3D12_RESOURCE_STATE_COMMON);

    bool hasValidationError = false;
    const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < messageCount; ++i) {
        SIZE_T messageSize = 0;
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, nullptr, &messageSize)));
        std::vector<uint8_t> storage(messageSize);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, message, &messageSize)));
        hasValidationError |= message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                              message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
    }
    EXPECT_FALSE(hasValidationError);

    source->destroy();
    delete source;
    destination->destroy();
    delete destination;
}

TEST(D3D12ResourceStateTest, SubmissionOrderOverridesRecordingOrder) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);
    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    ASSERT_NE(d3dDevice, nullptr);

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(d3dDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
        infoQueue->ClearStoredMessages();
    }

    TextureInfo textureInfo;
    textureInfo.type = TextureType::TEX2D;
    textureInfo.usage = TextureUsageBit::SAMPLED | TextureUsageBit::TRANSFER_DST;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 8;
    textureInfo.height = 8;
    auto *texture = device->createTexture(textureInfo);
    ASSERT_NE(texture, nullptr);

    CommandBufferInfo commandBufferInfo;
    commandBufferInfo.queue = device->getQueue();
    commandBufferInfo.type = CommandBufferType::PRIMARY;
    auto *copyDestinationCommandBuffer = device->createCommandBuffer(commandBufferInfo);
    auto *shaderReadCommandBuffer = device->createCommandBuffer(commandBufferInfo);
    ASSERT_NE(copyDestinationCommandBuffer, nullptr);
    ASSERT_NE(shaderReadCommandBuffer, nullptr);

    TextureBarrierInfo copyDestinationBarrierInfo;
    copyDestinationBarrierInfo.nextAccesses = AccessFlagBit::TRANSFER_WRITE;
    const TextureBarrier *copyDestinationBarriers[] = {
        device->getTextureBarrier(copyDestinationBarrierInfo)};
    const Texture *textures[] = {texture};
    copyDestinationCommandBuffer->begin();
    copyDestinationCommandBuffer->pipelineBarrier(
        nullptr, nullptr, nullptr, 0,
        copyDestinationBarriers, textures, 1);
    copyDestinationCommandBuffer->end();

    TextureBarrierInfo shaderReadBarrierInfo;
    shaderReadBarrierInfo.nextAccesses = AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE;
    const TextureBarrier *shaderReadBarriers[] = {
        device->getTextureBarrier(shaderReadBarrierInfo)};
    shaderReadCommandBuffer->begin();
    shaderReadCommandBuffer->pipelineBarrier(
        nullptr, nullptr, nullptr, 0,
        shaderReadBarriers, textures, 1);
    shaderReadCommandBuffer->end();

    CommandBuffer *submittedCommandBuffers[] = {
        shaderReadCommandBuffer,
        copyDestinationCommandBuffer,
    };
    device->getQueue()->submit(submittedCommandBuffers, 2);
    ASSERT_TRUE(device->waitIdle());

    const auto backing =
        static_cast<CCD3D12Texture *>(texture)->getD3D12ResourceBacking();
    ASSERT_NE(backing, nullptr);
    EXPECT_EQ(backing->states.get(0), D3D12_RESOURCE_STATE_COPY_DEST);

    if (infoQueue) {
        bool hasValidationError = false;
        const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < messageCount; ++i) {
            SIZE_T messageSize = 0;
            ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, nullptr, &messageSize)));
            std::vector<uint8_t> storage(messageSize);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, message, &messageSize)));
            hasValidationError |= message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                                  message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
        }
        EXPECT_FALSE(hasValidationError);
    }

    copyDestinationCommandBuffer->destroy();
    delete copyDestinationCommandBuffer;
    shaderReadCommandBuffer->destroy();
    delete shaderReadCommandBuffer;
    texture->destroy();
    delete texture;
}

TEST(D3D12ResourceStateTest, MipmapGenerationTracksEverySubresourceTransition) {
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
    textureInfo.usage = TextureUsageBit::SAMPLED | TextureUsageBit::TRANSFER_DST;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 8;
    textureInfo.height = 8;
    textureInfo.levelCount = 4;
    textureInfo.flags = TextureFlagBit::GEN_MIPMAP;
    auto *texture = device->createTexture(textureInfo);
    ASSERT_NE(texture, nullptr);

    std::vector<uint8_t> pixels(8U * 8U * 4U, 0x7F);
    const uint8_t *uploadBuffers[] = {pixels.data()};
    BufferTextureCopy region;
    region.texExtent = {8, 8, 1};

    auto *commandBuffer = device->getCommandBuffer();
    ASSERT_NE(commandBuffer, nullptr);
    commandBuffer->begin();
    commandBuffer->copyBuffersToTexture(uploadBuffers, texture, &region, 1);
    commandBuffer->end();
    CommandBuffer *commandBuffers[] = {commandBuffer};
    device->getQueue()->submit(commandBuffers, 1);
    ASSERT_TRUE(device->waitIdle());

    const auto backing =
        static_cast<CCD3D12Texture *>(texture)->getD3D12ResourceBacking();
    ASSERT_NE(backing, nullptr);
    const auto sampledState = static_cast<D3D12_RESOURCE_STATES>(
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (uint32_t subresource = 0; subresource < backing->subresourceCount(); ++subresource) {
        EXPECT_EQ(backing->states.get(subresource), sampledState);
    }

    bool hasValidationError = false;
    const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < messageCount; ++i) {
        SIZE_T messageSize = 0;
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, nullptr, &messageSize)));
        std::vector<uint8_t> storage(messageSize);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, message, &messageSize)));
        hasValidationError |= message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                              message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
    }
    EXPECT_FALSE(hasValidationError);

    texture->destroy();
    delete texture;
}

TEST(D3D12CommandBufferTest, SecondaryBackingOutlivesDestroyedWrapper) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);
    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    ASSERT_NE(d3dDevice, nullptr);

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
        GTEST_SKIP() << "D3D12 debug layer is unavailable";
    }
    infoQueue->ClearStoredMessages();

    CommandBufferInfo secondaryInfo;
    secondaryInfo.queue = device->getQueue();
    secondaryInfo.type = CommandBufferType::SECONDARY;
    auto *secondary = device->createCommandBuffer(secondaryInfo);
    ASSERT_NE(secondary, nullptr);
    secondary->begin();
    secondary->end();

    auto *primary = device->getCommandBuffer();
    ASSERT_NE(primary, nullptr);
    primary->begin(nullptr, 0, nullptr);
    CommandBuffer *secondaryBuffers[] = {secondary};
    primary->execute(secondaryBuffers, 1);
    primary->end();

    secondary->destroy();
    delete secondary;

    CommandBuffer *primaryBuffers[] = {primary};
    device->getQueue()->submit(primaryBuffers, 1);
    ASSERT_TRUE(device->waitIdle());

    bool hasValidationError = false;
    const UINT64 messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < messageCount; ++i) {
        SIZE_T messageSize = 0;
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, nullptr, &messageSize)));
        std::vector<uint8_t> storage(messageSize);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        ASSERT_TRUE(SUCCEEDED(infoQueue->GetMessage(i, message, &messageSize)));
        hasValidationError |= message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                              message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
    }
    EXPECT_FALSE(hasValidationError);
}

TEST(D3D12CommandBufferTest, RecordingContextRetainsInputAssemblerResources) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    BufferInfo bufferInfo;
    bufferInfo.usage = BufferUsageBit::VERTEX;
    bufferInfo.memUsage = MemoryUsageBit::DEVICE;
    bufferInfo.size = 48;
    bufferInfo.stride = 12;
    auto *vertexBuffer = device->createBuffer(bufferInfo);
    ASSERT_NE(vertexBuffer, nullptr);

    InputAssemblerInfo inputAssemblerInfo;
    inputAssemblerInfo.attributes.push_back({
        ATTR_NAME_POSITION, Format::RGB32F, false, 0, false, 0});
    inputAssemblerInfo.vertexBuffers.push_back(vertexBuffer);
    auto *inputAssembler = device->createInputAssembler(inputAssemblerInfo);
    ASSERT_NE(inputAssembler, nullptr);

    CommandBufferInfo commandBufferInfo;
    commandBufferInfo.queue = device->getQueue();
    commandBufferInfo.type = CommandBufferType::PRIMARY;
    auto *commandBuffer = static_cast<CCD3D12CommandBuffer *>(
        device->createCommandBuffer(commandBufferInfo));
    ASSERT_NE(commandBuffer, nullptr);
    commandBuffer->begin(nullptr, 0, nullptr);
    commandBuffer->bindInputAssembler(inputAssembler);
    commandBuffer->end();

    EXPECT_EQ(commandBuffer->getD3D12RetainedResourceCount(), 1U);

    inputAssembler->destroy();
    delete inputAssembler;
    vertexBuffer->destroy();
    delete vertexBuffer;

    CommandBuffer *submitted[] = {commandBuffer};
    device->getQueue()->submit(submitted, 1);
    ASSERT_TRUE(device->waitIdle());

    commandBuffer->destroy();
    delete commandBuffer;
}

TEST(D3D12CommandBufferTest, RecordingContextRetainsDescriptorResources) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    DescriptorSetLayoutInfo layoutInfo;
    layoutInfo.bindings.push_back({
        0, DescriptorType::UNIFORM_BUFFER, 1, ShaderStageFlagBit::VERTEX});
    auto *layout = device->createDescriptorSetLayout(layoutInfo);
    auto *descriptorSet = device->createDescriptorSet({layout});
    ASSERT_NE(layout, nullptr);
    ASSERT_NE(descriptorSet, nullptr);

    BufferInfo bufferInfo;
    bufferInfo.usage = BufferUsageBit::UNIFORM | BufferUsageBit::TRANSFER_DST;
    bufferInfo.memUsage = MemoryUsageBit::DEVICE;
    bufferInfo.size = 256;
    bufferInfo.stride = 256;
    auto *uniformBuffer = device->createBuffer(bufferInfo);
    ASSERT_NE(uniformBuffer, nullptr);
    descriptorSet->bindBuffer(0, uniformBuffer);
    descriptorSet->update();

    CommandBufferInfo commandBufferInfo;
    commandBufferInfo.queue = device->getQueue();
    commandBufferInfo.type = CommandBufferType::PRIMARY;
    auto *commandBuffer = static_cast<CCD3D12CommandBuffer *>(
        device->createCommandBuffer(commandBufferInfo));
    ASSERT_NE(commandBuffer, nullptr);
    commandBuffer->begin(nullptr, 0, nullptr);
    commandBuffer->bindDescriptorSet(0, descriptorSet, 0, nullptr);
    commandBuffer->end();

    EXPECT_EQ(commandBuffer->getD3D12RetainedResourceCount(), 1U);

    descriptorSet->destroy();
    delete descriptorSet;
    layout->destroy();
    delete layout;
    uniformBuffer->destroy();
    delete uniformBuffer;

    CommandBuffer *submitted[] = {commandBuffer};
    device->getQueue()->submit(submitted, 1);
    ASSERT_TRUE(device->waitIdle());

    commandBuffer->destroy();
    delete commandBuffer;
}

TEST(D3D12CommandBufferTest, QueueRetainsSubmittedPrimaryBackingAfterWrapperDestroy) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    CommandBufferInfo primaryInfo;
    primaryInfo.queue = device->getQueue();
    primaryInfo.type = CommandBufferType::PRIMARY;
    auto *primary = static_cast<CCD3D12CommandBuffer *>(
        device->createCommandBuffer(primaryInfo));
    ASSERT_NE(primary, nullptr);
    primary->begin(nullptr, 0, nullptr);
    primary->end();

    auto context = primary->getD3D12CommandRecordingContext();
    ASSERT_NE(context, nullptr);
    CommandBuffer *primaryBuffers[] = {primary};
    device->getQueue()->submit(primaryBuffers, 1);

    primary->destroy();
    delete primary;
    EXPECT_GT(context.use_count(), 1);

    ASSERT_TRUE(device->waitIdle());
    EXPECT_EQ(context.use_count(), 1);
}

TEST(D3D12SkinningTest, ReadsJointVertexAttributesAndUpdatedLocalJointUniformBuffer) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    DescriptorSetLayoutInfo emptyLayoutInfo;
    auto *setLayout0 = device->createDescriptorSetLayout(emptyLayoutInfo);
    auto *setLayout1 = device->createDescriptorSetLayout(emptyLayoutInfo);

    DescriptorSetLayoutInfo localLayoutInfo;
    localLayoutInfo.bindings.push_back({
        0, DescriptorType::UNIFORM_BUFFER, 1, ShaderStageFlagBit::VERTEX});
    localLayoutInfo.bindings.push_back({
        1, DescriptorType::DYNAMIC_UNIFORM_BUFFER, 1, ShaderStageFlagBit::FRAGMENT});
    for (uint32_t binding = 2; binding <= 6; ++binding) {
        localLayoutInfo.bindings.push_back({
            binding, DescriptorType::UNIFORM_BUFFER, 1, ShaderStageFlagBit::VERTEX});
    }
    for (uint32_t binding = 7; binding <= 15; ++binding) {
        localLayoutInfo.bindings.push_back({
            binding, DescriptorType::SAMPLER_TEXTURE, 1,
            ShaderStageFlagBit::VERTEX | ShaderStageFlagBit::FRAGMENT});
    }
    auto *localLayout = device->createDescriptorSetLayout(localLayoutInfo);
    auto *localSet = device->createDescriptorSet({localLayout});
    ASSERT_NE(setLayout0, nullptr);
    ASSERT_NE(setLayout1, nullptr);
    ASSERT_NE(localLayout, nullptr);
    ASSERT_NE(localSet, nullptr);

    auto *pipelineLayout = device->createPipelineLayout({
        {setLayout0, setLayout1, localLayout}});
    ASSERT_NE(pipelineLayout, nullptr);

    ShaderInfo shaderInfo;
    shaderInfo.name = "d3d12-skinning-local-uniform-test";
    shaderInfo.stages = {
        {ShaderStageFlagBit::VERTEX, R"(
layout(location = 0) in vec2 a_position;
layout(location = 1) in uvec4 a_joints;
layout(location = 2) in vec4 a_weights;
layout(set = 2, binding = 0) uniform Local {
    vec4 localMarker;
};
layout(set = 2, binding = 3) uniform Skin {
    vec4 jointRows[768];
};
layout(location = 0) out vec4 v_color;
void main() {
    uint jointRow = a_joints.x * 3u;
    float marker = jointRows[jointRow].x * a_weights.x + localMarker.x;
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_color = vec4(marker, 0.0, 0.0, 1.0);
}
)",
        },
        {ShaderStageFlagBit::FRAGMENT, R"(
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 fragColor;
void main() {
    fragColor = v_color;
}
)",
        },
    };
    shaderInfo.attributes = {
        {ATTR_NAME_POSITION, Format::RG32F, false, 0, false, 0},
        {ATTR_NAME_JOINTS, Format::RGBA8UI, false, 0, false, 1},
        {ATTR_NAME_WEIGHTS, Format::RGBA32F, false, 0, false, 2},
    };
    shaderInfo.blocks = {
        {2, 0, "Local", {{"localMarker", Type::FLOAT4, 1}}, 1},
        {2, 3, "Skin", {{"jointRows", Type::FLOAT4, 768}}, 1},
    };
    auto *shader = device->createShader(shaderInfo);
    ASSERT_NE(shader, nullptr);

    struct SkinVertex {
        float position[2];
        uint8_t joints[4];
        float weights[4];
    };
    static_assert(sizeof(SkinVertex) == 28);
    const SkinVertex vertices[] = {
        {{-0.75F, -0.75F}, {0, 0, 0, 0}, {1.F, 0.F, 0.F, 0.F}},
        {{0.75F, -0.75F}, {0, 0, 0, 0}, {1.F, 0.F, 0.F, 0.F}},
        {{0.F, 0.75F}, {0, 0, 0, 0}, {1.F, 0.F, 0.F, 0.F}},
    };
    auto *vertexBuffer = device->createBuffer({
        BufferUsageBit::VERTEX | BufferUsageBit::TRANSFER_DST,
        MemoryUsageBit::DEVICE,
        sizeof(vertices),
        sizeof(SkinVertex)});
    ASSERT_NE(vertexBuffer, nullptr);
    vertexBuffer->update(vertices, sizeof(vertices));

    InputAssemblerInfo inputAssemblerInfo;
    inputAssemblerInfo.attributes = shaderInfo.attributes;
    inputAssemblerInfo.vertexBuffers = {vertexBuffer};
    auto *inputAssembler = device->createInputAssembler(inputAssemblerInfo);
    ASSERT_NE(inputAssembler, nullptr);

    constexpr uint32_t LOCAL_BUFFER_SIZE = 256;
    constexpr uint32_t JOINT_BUFFER_SIZE = 256 * 12 * sizeof(float);
    auto *localBuffer = device->createBuffer({
        BufferUsageBit::UNIFORM | BufferUsageBit::TRANSFER_DST,
        MemoryUsageBit::HOST | MemoryUsageBit::DEVICE,
        LOCAL_BUFFER_SIZE,
        LOCAL_BUFFER_SIZE});
    auto *jointBuffer = device->createBuffer({
        BufferUsageBit::UNIFORM | BufferUsageBit::TRANSFER_DST,
        MemoryUsageBit::HOST | MemoryUsageBit::DEVICE,
        JOINT_BUFFER_SIZE,
        JOINT_BUFFER_SIZE});
    ASSERT_NE(localBuffer, nullptr);
    ASSERT_NE(jointBuffer, nullptr);

    localSet->bindBuffer(0, localBuffer);
    localSet->bindBuffer(3, jointBuffer);
    localSet->update();

    std::vector<float> localData(LOCAL_BUFFER_SIZE / sizeof(float), 0.F);
    std::vector<float> jointData(JOINT_BUFFER_SIZE / sizeof(float), 0.F);
    localBuffer->update(localData.data(), LOCAL_BUFFER_SIZE);
    jointData[0] = 1.F;
    jointBuffer->update(jointData.data(), JOINT_BUFFER_SIZE);

    TextureInfo textureInfo;
    textureInfo.type = TextureType::TEX2D;
    textureInfo.usage = TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::TRANSFER_SRC;
    textureInfo.format = Format::RGBA8;
    textureInfo.width = 16;
    textureInfo.height = 16;
    auto *texture = device->createTexture(textureInfo);
    ASSERT_NE(texture, nullptr);

    RenderPassInfo renderPassInfo;
    renderPassInfo.colorAttachments.resize(1);
    renderPassInfo.colorAttachments[0].format = Format::RGBA8;
    renderPassInfo.colorAttachments[0].loadOp = LoadOp::CLEAR;
    renderPassInfo.colorAttachments[0].storeOp = StoreOp::STORE;
    auto *renderPass = device->createRenderPass(renderPassInfo);
    auto *framebuffer = device->createFramebuffer({renderPass, {texture}});
    ASSERT_NE(renderPass, nullptr);
    ASSERT_NE(framebuffer, nullptr);

    PipelineStateInfo pipelineStateInfo;
    pipelineStateInfo.shader = shader;
    pipelineStateInfo.pipelineLayout = pipelineLayout;
    pipelineStateInfo.renderPass = renderPass;
    pipelineStateInfo.inputState = {shaderInfo.attributes};
    pipelineStateInfo.rasterizerState.cullMode = CullMode::NONE;
    pipelineStateInfo.depthStencilState.depthTest = false;
    pipelineStateInfo.depthStencilState.depthWrite = false;
    auto *pipelineState = device->createPipelineState(pipelineStateInfo);
    ASSERT_NE(pipelineState, nullptr);

    const Color clearColor{0.F, 0.F, 0.F, 1.F};
    auto *commandBuffer = device->getCommandBuffer();
    ASSERT_NE(commandBuffer, nullptr);
    const DrawPacket drawPacket{
        pipelineState, nullptr, inputAssembler, localSet, inputAssembler->getDrawInfo()};
    const size_t centerPixel = (8U * 16U + 8U) * 4U;
    const auto renderCenterPixel = [&]() {
        commandBuffer->begin();
        commandBuffer->beginRenderPass(
            renderPass, framebuffer, {0, 0, 16, 16}, &clearColor, 1.F, 0);
        commandBuffer->beginDrawBatch();
        commandBuffer->drawPackets(&drawPacket, 1, 1, 2);
        commandBuffer->endDrawBatch();
        commandBuffer->endRenderPass();
        commandBuffer->end();
        CommandBuffer *commandBuffers[] = {commandBuffer};
        device->getQueue()->submit(commandBuffers, 1);

        std::vector<uint8_t> pixels(16U * 16U * 4U);
        uint8_t *readbackBuffers[] = {pixels.data()};
        BufferTextureCopy readbackRegion;
        readbackRegion.texExtent = {16, 16, 1};
        static_cast<Device *>(device)->copyTextureToBuffers(
            texture, readbackBuffers, &readbackRegion, 1);
        EXPECT_EQ(pixels[centerPixel + 1U], 0U);
        EXPECT_EQ(pixels[centerPixel + 2U], 0U);
        EXPECT_EQ(pixels[centerPixel + 3U], 255U);
        return pixels[centerPixel];
    };

    EXPECT_GE(renderCenterPixel(), 250U);
    jointData[0] = 0.25F;
    jointBuffer->update(jointData.data(), JOINT_BUFFER_SIZE);
    EXPECT_NEAR(renderCenterPixel(), 64U, 2U);

    pipelineState->destroy();
    delete pipelineState;
    framebuffer->destroy();
    delete framebuffer;
    renderPass->destroy();
    delete renderPass;
    texture->destroy();
    delete texture;
    inputAssembler->destroy();
    delete inputAssembler;
    vertexBuffer->destroy();
    delete vertexBuffer;
    localSet->destroy();
    delete localSet;
    jointBuffer->destroy();
    delete jointBuffer;
    localBuffer->destroy();
    delete localBuffer;
    shader->destroy();
    delete shader;
    pipelineLayout->destroy();
    delete pipelineLayout;
    localLayout->destroy();
    delete localLayout;
    setLayout1->destroy();
    delete setLayout1;
    setLayout0->destroy();
    delete setLayout0;
}

TEST(D3D12SkinningTest, SamplesUpdatedJointTextureAtCustomLayoutOffset) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    DescriptorSetLayoutInfo emptyLayoutInfo;
    auto *setLayout0 = device->createDescriptorSetLayout(emptyLayoutInfo);
    auto *setLayout1 = device->createDescriptorSetLayout(emptyLayoutInfo);

    DescriptorSetLayoutInfo localLayoutInfo;
    localLayoutInfo.bindings.push_back({
        0, DescriptorType::UNIFORM_BUFFER, 1, ShaderStageFlagBit::VERTEX});
    localLayoutInfo.bindings.push_back({
        1, DescriptorType::DYNAMIC_UNIFORM_BUFFER, 1, ShaderStageFlagBit::FRAGMENT});
    for (uint32_t binding = 2; binding <= 6; ++binding) {
        localLayoutInfo.bindings.push_back({
            binding, DescriptorType::UNIFORM_BUFFER, 1, ShaderStageFlagBit::VERTEX});
    }
    for (uint32_t binding = 7; binding <= 15; ++binding) {
        localLayoutInfo.bindings.push_back({
            binding, DescriptorType::SAMPLER_TEXTURE, 1,
            ShaderStageFlagBit::VERTEX | ShaderStageFlagBit::FRAGMENT});
    }
    auto *localLayout = device->createDescriptorSetLayout(localLayoutInfo);
    auto *localSet = device->createDescriptorSet({localLayout});
    auto *secondLocalSet = device->createDescriptorSet({localLayout});
    ASSERT_NE(setLayout0, nullptr);
    ASSERT_NE(setLayout1, nullptr);
    ASSERT_NE(localLayout, nullptr);
    ASSERT_NE(localSet, nullptr);
    ASSERT_NE(secondLocalSet, nullptr);

    auto *pipelineLayout = device->createPipelineLayout({
        {setLayout0, setLayout1, localLayout}});
    ASSERT_NE(pipelineLayout, nullptr);

    ShaderInfo shaderInfo;
    shaderInfo.name = "d3d12-joint-texture-custom-layout-test";
    shaderInfo.stages = {
        {ShaderStageFlagBit::VERTEX, R"(
layout(location = 0) in vec2 a_position;
layout(set = 2, binding = 0) uniform Local {
    vec4 localMarker;
};
layout(set = 2, binding = 3) uniform JointTextureInfo {
    vec4 cc_jointTextureInfo;
};
layout(set = 2, binding = 7) uniform sampler2D cc_jointTexture;
layout(location = 0) out vec4 v_color;
void main() {
    float textureWidth = cc_jointTextureInfo.x;
    float pixelOffset = cc_jointTextureInfo.z;
    float inverseSize = cc_jointTextureInfo.w;
    float y = floor(pixelOffset * inverseSize);
    float x = floor(pixelOffset - y * textureWidth);
    vec2 uv = vec2((x + 0.5) * inverseSize, (y + 0.5) * inverseSize);
    gl_Position = vec4(a_position + vec2(localMarker.x, 0.0), 0.0, 1.0);
    v_color = texture(cc_jointTexture, uv);
}
)",
        },
        {ShaderStageFlagBit::FRAGMENT, R"(
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 fragColor;
void main() {
    fragColor = v_color;
}
)",
        },
    };
    shaderInfo.attributes = {
        {ATTR_NAME_POSITION, Format::RG32F, false, 0, false, 0},
    };
    shaderInfo.blocks = {
        {2, 0, "Local", {{"localMarker", Type::FLOAT4, 1}}, 1},
        {2, 3, "JointTextureInfo", {{"cc_jointTextureInfo", Type::FLOAT4, 1}}, 1},
    };
    shaderInfo.samplerTextures = {
        {2, 7, "cc_jointTexture", Type::SAMPLER2D, 1},
    };
    auto *shader = device->createShader(shaderInfo);
    ASSERT_NE(shader, nullptr);

    const float vertices[] = {
        -0.3F, -0.75F,
        0.3F,  -0.75F,
        0.F,    0.75F,
    };
    auto *vertexBuffer = device->createBuffer({
        BufferUsageBit::VERTEX | BufferUsageBit::TRANSFER_DST,
        MemoryUsageBit::DEVICE,
        sizeof(vertices),
        2U * sizeof(float)});
    ASSERT_NE(vertexBuffer, nullptr);
    vertexBuffer->update(vertices, sizeof(vertices));

    InputAssemblerInfo inputAssemblerInfo;
    inputAssemblerInfo.attributes = shaderInfo.attributes;
    inputAssemblerInfo.vertexBuffers = {vertexBuffer};
    auto *inputAssembler = device->createInputAssembler(inputAssemblerInfo);
    ASSERT_NE(inputAssembler, nullptr);

    auto *localBuffer = device->createBuffer({
        BufferUsageBit::UNIFORM | BufferUsageBit::TRANSFER_DST,
        MemoryUsageBit::HOST | MemoryUsageBit::DEVICE,
        256,
        256});
    auto *secondLocalBuffer = device->createBuffer({
        BufferUsageBit::UNIFORM | BufferUsageBit::TRANSFER_DST,
        MemoryUsageBit::HOST | MemoryUsageBit::DEVICE,
        256,
        256});
    auto *jointTextureInfoBuffer = device->createBuffer({
        BufferUsageBit::UNIFORM | BufferUsageBit::TRANSFER_DST,
        MemoryUsageBit::DEVICE,
        4U * sizeof(float),
        4U * sizeof(float)});
    auto *secondJointTextureInfoBuffer = device->createBuffer({
        BufferUsageBit::UNIFORM | BufferUsageBit::TRANSFER_DST,
        MemoryUsageBit::DEVICE,
        4U * sizeof(float),
        4U * sizeof(float)});
    ASSERT_NE(localBuffer, nullptr);
    ASSERT_NE(secondLocalBuffer, nullptr);
    ASSERT_NE(jointTextureInfoBuffer, nullptr);
    ASSERT_NE(secondJointTextureInfoBuffer, nullptr);

    constexpr uint32_t JOINT_TEXTURE_SIZE = 4;
    constexpr uint32_t FIRST_JOINT_PIXEL_OFFSET = 5;
    constexpr uint32_t SECOND_JOINT_PIXEL_OFFSET = 10;
    const float jointTextureInfo[] = {
        static_cast<float>(JOINT_TEXTURE_SIZE),
        1.F,
        static_cast<float>(FIRST_JOINT_PIXEL_OFFSET) + 0.1F,
        1.F / static_cast<float>(JOINT_TEXTURE_SIZE),
    };
    const float secondJointTextureInfo[] = {
        static_cast<float>(JOINT_TEXTURE_SIZE),
        1.F,
        static_cast<float>(SECOND_JOINT_PIXEL_OFFSET) + 0.1F,
        1.F / static_cast<float>(JOINT_TEXTURE_SIZE),
    };
    std::vector<float> localData(256U / sizeof(float), 0.F);
    std::vector<float> secondLocalData(256U / sizeof(float), 0.F);
    localData[0] = -0.5F;
    secondLocalData[0] = 0.5F;
    localBuffer->update(localData.data(), 256);
    secondLocalBuffer->update(secondLocalData.data(), 256);
    jointTextureInfoBuffer->update(jointTextureInfo, sizeof(jointTextureInfo));
    secondJointTextureInfoBuffer->update(
        secondJointTextureInfo, sizeof(secondJointTextureInfo));

    TextureInfo jointTextureInfoDesc;
    jointTextureInfoDesc.type = TextureType::TEX2D;
    jointTextureInfoDesc.usage =
        TextureUsageBit::SAMPLED | TextureUsageBit::TRANSFER_DST;
    jointTextureInfoDesc.format = Format::RGBA32F;
    jointTextureInfoDesc.width = JOINT_TEXTURE_SIZE;
    jointTextureInfoDesc.height = JOINT_TEXTURE_SIZE;
    auto *jointTexture = device->createTexture(jointTextureInfoDesc);
    ASSERT_NE(jointTexture, nullptr);

    SamplerInfo samplerInfo;
    samplerInfo.minFilter = Filter::POINT;
    samplerInfo.magFilter = Filter::POINT;
    samplerInfo.mipFilter = Filter::NONE;
    samplerInfo.addressU = Address::CLAMP;
    samplerInfo.addressV = Address::CLAMP;
    samplerInfo.addressW = Address::CLAMP;
    auto *jointSampler = device->getSampler(samplerInfo);
    ASSERT_NE(jointSampler, nullptr);

    localSet->bindBuffer(0, localBuffer);
    localSet->bindBuffer(3, jointTextureInfoBuffer);
    localSet->bindTexture(7, jointTexture);
    localSet->bindSampler(7, jointSampler);
    localSet->update();
    secondLocalSet->bindBuffer(0, secondLocalBuffer);
    secondLocalSet->bindBuffer(3, secondJointTextureInfoBuffer);
    secondLocalSet->bindTexture(7, jointTexture);
    secondLocalSet->bindSampler(7, jointSampler);
    secondLocalSet->update();

    TextureInfo outputTextureInfo;
    outputTextureInfo.type = TextureType::TEX2D;
    outputTextureInfo.usage =
        TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::TRANSFER_SRC;
    outputTextureInfo.format = Format::RGBA8;
    outputTextureInfo.width = 16;
    outputTextureInfo.height = 16;
    auto *outputTexture = device->createTexture(outputTextureInfo);
    ASSERT_NE(outputTexture, nullptr);

    RenderPassInfo renderPassInfo;
    renderPassInfo.colorAttachments.resize(1);
    renderPassInfo.colorAttachments[0].format = Format::RGBA8;
    renderPassInfo.colorAttachments[0].loadOp = LoadOp::CLEAR;
    renderPassInfo.colorAttachments[0].storeOp = StoreOp::STORE;
    auto *renderPass = device->createRenderPass(renderPassInfo);
    auto *framebuffer = device->createFramebuffer({renderPass, {outputTexture}});
    ASSERT_NE(renderPass, nullptr);
    ASSERT_NE(framebuffer, nullptr);

    PipelineStateInfo pipelineStateInfo;
    pipelineStateInfo.shader = shader;
    pipelineStateInfo.pipelineLayout = pipelineLayout;
    pipelineStateInfo.renderPass = renderPass;
    pipelineStateInfo.inputState = {shaderInfo.attributes};
    pipelineStateInfo.rasterizerState.cullMode = CullMode::NONE;
    pipelineStateInfo.depthStencilState.depthTest = false;
    pipelineStateInfo.depthStencilState.depthWrite = false;
    auto *pipelineState = device->createPipelineState(pipelineStateInfo);
    ASSERT_NE(pipelineState, nullptr);

    const Color clearColor{0.F, 0.F, 0.F, 1.F};
    auto *commandBuffer = device->getCommandBuffer();
    ASSERT_NE(commandBuffer, nullptr);
    const DrawPacket drawPackets[] = {
        {pipelineState, nullptr, inputAssembler, localSet, inputAssembler->getDrawInfo()},
        {pipelineState, nullptr, inputAssembler, secondLocalSet, inputAssembler->getDrawInfo()},
    };
    const size_t leftPixel = (8U * 16U + 4U) * 4U;
    const size_t rightPixel = (8U * 16U + 12U) * 4U;
    const auto uploadAndRender = [&](float firstMarker, float secondMarker) {
        const float firstJointTexel[] = {firstMarker, 0.F, 0.F, 1.F};
        const float secondJointTexel[] = {secondMarker, 0.F, 0.F, 1.F};
        const uint8_t *uploadBuffers[] = {
            reinterpret_cast<const uint8_t *>(firstJointTexel),
            reinterpret_cast<const uint8_t *>(secondJointTexel),
        };
        BufferTextureCopy uploadRegions[2];
        uploadRegions[0].texOffset.x =
            FIRST_JOINT_PIXEL_OFFSET % JOINT_TEXTURE_SIZE;
        uploadRegions[0].texOffset.y =
            FIRST_JOINT_PIXEL_OFFSET / JOINT_TEXTURE_SIZE;
        uploadRegions[0].texExtent = {1, 1, 1};
        uploadRegions[1].texOffset.x =
            SECOND_JOINT_PIXEL_OFFSET % JOINT_TEXTURE_SIZE;
        uploadRegions[1].texOffset.y =
            SECOND_JOINT_PIXEL_OFFSET / JOINT_TEXTURE_SIZE;
        uploadRegions[1].texExtent = {1, 1, 1};
        static_cast<Device *>(device)->copyBuffersToTexture(
            uploadBuffers, jointTexture, uploadRegions, 2);

        commandBuffer->begin();
        commandBuffer->beginRenderPass(
            renderPass, framebuffer, {0, 0, 16, 16}, &clearColor, 1.F, 0);
        commandBuffer->beginDrawBatch();
        commandBuffer->drawPackets(drawPackets, 2, 1, 2);
        commandBuffer->endDrawBatch();
        commandBuffer->endRenderPass();
        commandBuffer->end();
        CommandBuffer *commandBuffers[] = {commandBuffer};
        device->getQueue()->submit(commandBuffers, 1);

        std::vector<uint8_t> pixels(16U * 16U * 4U);
        uint8_t *readbackBuffers[] = {pixels.data()};
        BufferTextureCopy readbackRegion;
        readbackRegion.texExtent = {16, 16, 1};
        static_cast<Device *>(device)->copyTextureToBuffers(
            outputTexture, readbackBuffers, &readbackRegion, 1);
        EXPECT_EQ(pixels[leftPixel + 1U], 0U);
        EXPECT_EQ(pixels[leftPixel + 2U], 0U);
        EXPECT_EQ(pixels[leftPixel + 3U], 255U);
        EXPECT_EQ(pixels[rightPixel + 1U], 0U);
        EXPECT_EQ(pixels[rightPixel + 2U], 0U);
        EXPECT_EQ(pixels[rightPixel + 3U], 255U);
        return std::array<uint8_t, 2>{
            pixels[leftPixel],
            pixels[rightPixel],
        };
    };

    const auto firstRender = uploadAndRender(1.F, 0.25F);
    EXPECT_GE(firstRender[0], 250U);
    EXPECT_NEAR(firstRender[1], 64U, 2U);
    const auto secondRender = uploadAndRender(0.5F, 0.75F);
    EXPECT_NEAR(secondRender[0], 128U, 2U);
    EXPECT_NEAR(secondRender[1], 191U, 2U);

    pipelineState->destroy();
    delete pipelineState;
    framebuffer->destroy();
    delete framebuffer;
    renderPass->destroy();
    delete renderPass;
    outputTexture->destroy();
    delete outputTexture;
    jointTexture->destroy();
    delete jointTexture;
    inputAssembler->destroy();
    delete inputAssembler;
    vertexBuffer->destroy();
    delete vertexBuffer;
    localSet->destroy();
    delete localSet;
    secondLocalSet->destroy();
    delete secondLocalSet;
    jointTextureInfoBuffer->destroy();
    delete jointTextureInfoBuffer;
    secondJointTextureInfoBuffer->destroy();
    delete secondJointTextureInfoBuffer;
    localBuffer->destroy();
    delete localBuffer;
    secondLocalBuffer->destroy();
    delete secondLocalBuffer;
    shader->destroy();
    delete shader;
    pipelineLayout->destroy();
    delete pipelineLayout;
    localLayout->destroy();
    delete localLayout;
    setLayout1->destroy();
    delete setLayout1;
    setLayout0->destroy();
    delete setLayout0;
}

} // namespace gfx
} // namespace cc

#endif
