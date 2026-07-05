/****************************************************************************
 Copyright (c) 2020-2023 Xiamen Yaji Software Co., Ltd.

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

#include "gfx-base/GFXDevice.h"
#include <cstdint>
#include <memory>
#include <vector>

#ifndef CC_D3D12_PERF_COUNTERS
    #define CC_D3D12_PERF_COUNTERS 1
#endif

namespace cc {
namespace gfx {

class D3D12DescriptorHeapPool;

class CCD3D12Queue;
class CCD3D12Swapchain;
class CCD3D12Texture;

struct D3D12UploadAllocation {
    void *resource{nullptr};
    void *mappedData{nullptr};
    uint64_t offset{0};
    uint64_t gpuAddress{0};
    uint64_t size{0};
    bool isValid{false};
};

class CC_DLL CCD3D12Device final : public Device {
public:
    static CCD3D12Device *getInstance();

    CCD3D12Device();
    ~CCD3D12Device() override;

    using Device::copyBuffersToTexture;
    using Device::createBuffer;
    using Device::createCommandBuffer;
    using Device::createDescriptorSet;
    using Device::createDescriptorSetLayout;
    using Device::createFramebuffer;
    using Device::createGeneralBarrier;
    using Device::createInputAssembler;
    using Device::createPipelineLayout;
    using Device::createPipelineState;
    using Device::createQueryPool;
    using Device::createQueue;
    using Device::createRenderPass;
    using Device::createSampler;
    using Device::createShader;
    using Device::createSwapchain;
    using Device::createTexture;
    using Device::createTextureBarrier;

    void frameSync() override {}
    void acquire(Swapchain *const *swapchains, uint32_t count) override;
    void present() override;

    void *getD3D12DeviceHandle() const;
    void *getGraphicsQueueHandle() const;
    void *getDXGIFactoryHandle() const;

    // GPU-visible descriptor heap pool for CBV/SRV/UAV (used by CommandBuffer::bindDescriptorSet)
    D3D12DescriptorHeapPool *getGPUDescriptorHeapPool() const;
    // GPU-visible sampler heap pool
    D3D12DescriptorHeapPool *getSamplerDescriptorHeapPool() const;

    // Dummy resources for null descriptor bindings (safe SRV/UAV fallback)
    class CCD3D12Texture *getDummyTexture() const;
    class CCD3D12Buffer *getDummyBuffer() const;

    // Command signatures for ExecuteIndirect (indirect draw / dispatch)
    void *getDrawIndirectSignature() const;
    void *getDrawIndexedIndirectSignature() const;
    void *getDispatchIndirectSignature() const;
    D3D12UploadAllocation allocateUploadBuffer(uint64_t size, uint64_t alignment);
    void notifySubmittedFence(void *fence, uint64_t value);
    void retireFrameResources();
    bool isSwapchainBackBuffer(void *resource) const;
    bool loadShaderCacheValue(const void *key, uint32_t keySize, std::vector<uint8_t> &outValue) const;
    bool storeShaderCacheValue(const void *key, uint32_t keySize, const std::vector<uint8_t> &value) const;
    void recordDescriptorFlush(uint32_t copyCalls, uint32_t copiedDescriptors,
                               uint32_t dynamicOffsetRewrites, uint32_t dynamicOffsetDescriptors,
                               uint32_t setDescriptorHeapCalls, uint32_t rootDescriptorTableBinds);
    void recordDescriptorStateBinds(uint32_t setDescriptorHeapCalls, uint32_t rootDescriptorTableBinds);
    void recordResourceBarriers(uint32_t barrierCount);
    void recordFenceWait(uint64_t waitMicroseconds);

protected:
    static CCD3D12Device *instance;

    friend class DeviceManager;
    friend class CCD3D12Queue;
    friend class CCD3D12Swapchain;
    friend class CCD3D12Texture;

    bool doInit(const DeviceInfo &info) override;
    void doDestroy() override;
    CommandBuffer *createCommandBuffer(const CommandBufferInfo &info, bool hasAgent) override;
    Queue *createQueue() override;
    QueryPool *createQueryPool() override;
    Swapchain *createSwapchain() override;
    Buffer *createBuffer() override;
    Texture *createTexture() override;
    Shader *createShader() override;
    InputAssembler *createInputAssembler() override;
    RenderPass *createRenderPass() override;
    Framebuffer *createFramebuffer() override;
    DescriptorSet *createDescriptorSet() override;
    DescriptorSetLayout *createDescriptorSetLayout() override;
    PipelineLayout *createPipelineLayout() override;
    PipelineState *createPipelineState() override;

    void copyBuffersToTexture(const uint8_t *const *buffers, Texture *dst, const BufferTextureCopy *regions, uint32_t count) override;
    void copyBuffersToTextureImmediate(const uint8_t *const *buffers, Texture *dst, const BufferTextureCopy *regions, uint32_t count);
    void flushDeferredCubeUploads();
    void flushDeferredCubeUploadsForTexture(Texture *texture);
    void discardDeferredCubeUploadsForTexture(Texture *texture);
    bool tryDeferCubeFaceUpload(const uint8_t *const *buffers, Texture *dst, const BufferTextureCopy *regions, uint32_t count);
    void copyTextureToBuffers(Texture *src, uint8_t *const *buffers, const BufferTextureCopy *region, uint32_t count) override;
    void getQueryPoolResults(QueryPool *queryPool) override;
    SampleCount getMaxSampleCount(Format format, TextureUsage usage, TextureFlags flags) const override;

    bool initializeD3D12Context();
    void initializeShaderCacheSession();
    void initFormatFeatures();
    void initCapabilities();
    void waitForGpu();
    void reportAndResetFramePerfCounters();

    struct Impl;
    std::unique_ptr<Impl> _impl;

    // Raw swapchain references for present().
    // The DeviceAgent/DeviceValidator layers wrap swapchains and store them
    // in THEIR _swapchains list (which is private to Device base class).
    // CCD3D12Device::present() needs access to the actual swapchain objects,
    // so we maintain our own tracking here.
    ccstd::vector<CCD3D12Swapchain *> _d3d12Swapchains;
};

} // namespace gfx
} // namespace cc
