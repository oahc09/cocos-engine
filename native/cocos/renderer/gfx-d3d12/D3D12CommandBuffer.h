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

#include "D3D12ResourceState.h"
#include "gfx-base/GFXCommandBuffer.h"
#include <d3d12.h>
#include <memory>
#include <vector>
#include <wrl/client.h>

namespace cc {
namespace gfx {

class CCD3D12Queue;

bool generateD3D12Mipmaps(
    ID3D12Device *device,
    ID3D12GraphicsCommandList *commandList,
    ID3D12Resource *resource,
    const D3D12ResourceBackingPtr &backing,
    const TextureInfo &textureInfo,
    ccstd::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> &pendingDescriptorHeaps,
    D3D12ResourceStateJournal *stateJournal = nullptr);

class CC_DLL CCD3D12CommandBuffer final : public CommandBuffer {
public:
    CCD3D12CommandBuffer();
    ~CCD3D12CommandBuffer() override;

    void begin(RenderPass *renderPass, uint32_t subpass, Framebuffer *frameBuffer) override;
    void end() override;
    void beginRenderPass(RenderPass *renderPass, Framebuffer *fbo, const Rect &renderArea, const Color *colors, float depth, uint32_t stencil, CommandBuffer *const *secondaryCBs, uint32_t secondaryCBCount) override;
    void endRenderPass() override;
    void insertMarker(const MarkerInfo &marker) override;
    void beginMarker(const MarkerInfo &marker) override;
    void endMarker() override;
    void bindPipelineState(PipelineState *pso) override;
    void bindDescriptorSet(uint32_t set, DescriptorSet *descriptorSet, uint32_t dynamicOffsetCount, const uint32_t *dynamicOffsets) override;
    void bindInputAssembler(InputAssembler *ia) override;
    void setViewport(const Viewport &vp) override;
    void setScissor(const Rect &rect) override;
    void setLineWidth(float width) override;
    void setDepthBias(float constant, float clamp, float slope) override;
    void setBlendConstants(const Color &constants) override;
    void setDepthBound(float minBounds, float maxBounds) override;
    void setStencilWriteMask(StencilFace face, uint32_t mask) override;
    void setStencilCompareMask(StencilFace face, uint32_t ref, uint32_t mask) override;
    void nextSubpass() override;
    void draw(const DrawInfo &info) override;
    bool supportsDrawBatch() const override { return _type == CommandBufferType::PRIMARY; }
    void beginDrawBatch() override;
    void endDrawBatch() override;
    void drawWithInputAssemblerAndDescriptorSet(InputAssembler *inputAssembler, uint32_t set,
                                                DescriptorSet *descriptorSet, const DrawInfo &info) override;
    void drawPackets(const DrawPacket *packets, uint32_t count,
                     uint32_t materialSet, uint32_t localSet) override;
    void updateBuffer(Buffer *buff, const void *data, uint32_t size) override;
    void copyBuffersToTexture(const uint8_t *const *buffers, Texture *texture, const BufferTextureCopy *regions, uint32_t count) override;
    void blitTexture(Texture *srcTexture, Texture *dstTexture, const TextureBlit *regions, uint32_t count, Filter filter) override;
    void copyTexture(Texture *srcTexture, Texture *dstTexture, const TextureCopy *regions, uint32_t count) override;
    void resolveTexture(Texture *srcTexture, Texture *dstTexture, const TextureCopy *regions, uint32_t count) override;
    void execute(CommandBuffer *const *cmdBuffs, uint32_t count) override;
    void dispatch(const DispatchInfo &info) override;
    void pipelineBarrier(const GeneralBarrier *barrier, const BufferBarrier *const *bufferBarriers, const Buffer *const *buffers, uint32_t bufferCount, const TextureBarrier *const *textureBarriers, const Texture *const *textures, uint32_t textureBarrierCount) override;
    void beginQuery(QueryPool *queryPool, uint32_t id) override;
    void endQuery(QueryPool *queryPool, uint32_t id) override;
    void resetQueryPool(QueryPool *queryPool) override;
    void customCommand(CustomCommand &&cmd) override;

    // Returns the closed ID3D12GraphicsCommandList as void* for Queue::submit
    void *getD3D12CommandList() const;
    std::shared_ptr<void> getD3D12CommandRecordingContext() const;
    uint32_t getD3D12RetainedResourceCount() const;
    void captureSubmissionResourceStates(std::vector<D3D12ResourceStateSnapshot> &snapshots) const;
    bool appendSubmissionStateFixupBarriers(std::vector<D3D12_RESOURCE_BARRIER> &barriers) const;
    bool commitSubmissionResourceStates() const;

    void notifySubmitted(void *fence, uint64_t fenceValue);
    void notifySubmissionFailed();

    // Flush pending descriptor set bindings to GPU (called internally before draw/dispatch)
    bool flushDescriptorSets();

protected:
    void doInit(const CommandBufferInfo &info) override;
    void doDestroy() override;

private:
    friend class CCD3D12Device;
    friend class CCD3D12Queue;

    // Coalesce transition barriers for unique DEFAULT-buffer updates drained
    // at one synchronization point. Repeated resources retain legacy ordering.
    void startBufferUpdateBatch(bool destinationsAreUnique);
    void finishBufferUpdateBatch();

    bool waitForFenceValue();
    static void retireD3D12CommandRecordingContext(const std::shared_ptr<void> &context);
    void invalidateGraphicsState();
    void invalidateDescriptorTables();
    bool flushDescriptorSetsIncremental();
    bool captureLocalRootCbvBatchBinding(struct D3D12LocalRootCbvBatchData &batchData,
                                         uint32_t *rootParameterIndices,
                                         class CCD3D12DescriptorSet *&descriptorSet,
                                         class CCD3D12DescriptorSet *directDescriptorSet = nullptr);
    bool tryAppendCompatibleLocalRootCbvDraw(InputAssembler *inputAssembler,
                                             class CCD3D12DescriptorSet *descriptorSet,
                                             const DrawInfo &info);
    bool appendLocalRootCbvBatchCommand(const DrawInfo &info,
                                        const uint64_t *gpuAddresses,
                                        uint32_t rootCbvCount);
    void drawLocalRootCbvBatchInternal(const DrawInfo &info,
                                       class CCD3D12DescriptorSet *directDescriptorSet);
    void flushLocalRootCbvBatch();
    void applyDynamicPipelineState();
    void retainDescriptorSetResources(class CCD3D12DescriptorSet *descriptorSet);
    void retainRecordingResource(ID3D12Resource *resource);
    void retainRecordingDeviceObject(ID3D12DeviceChild *object);
    void transitionColorAttachment(uint32_t attachment, D3D12_RESOURCE_STATES state);
    void bindSubpassRenderTargets(uint32_t subpass);
    void resolveSubpass(uint32_t subpass);

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace gfx
} // namespace cc
