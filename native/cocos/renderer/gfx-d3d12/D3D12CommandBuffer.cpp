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

#include "D3D12CommandBuffer.h"
#include "base/Log.h"

namespace cc {
namespace gfx {

namespace {
constexpr float D3D12_POC_CLEAR_COLOR[4] = {0.1F, 0.2F, 0.8F, 1.0F};
bool gLoggedClearBegin = false;
bool gLoggedClearEnd = false;
}

void CCD3D12CommandBuffer::doInit(const CommandBufferInfo &info) {
    (void)info;
}

void CCD3D12CommandBuffer::doDestroy() {
}

void CCD3D12CommandBuffer::begin(RenderPass *renderPass, uint32_t subpass, Framebuffer *frameBuffer) {
    (void)renderPass;
    (void)subpass;
    (void)frameBuffer;
    if (!gLoggedClearBegin) {
        gLoggedClearBegin = true;
        CC_LOG_INFO("D3D12 clear begin: color=(%.2f, %.2f, %.2f, %.2f), barrier PRESENT->RENDER_TARGET (stub).",
                    D3D12_POC_CLEAR_COLOR[0], D3D12_POC_CLEAR_COLOR[1], D3D12_POC_CLEAR_COLOR[2], D3D12_POC_CLEAR_COLOR[3]);
    }
}

void CCD3D12CommandBuffer::end() {
    if (!gLoggedClearEnd) {
        gLoggedClearEnd = true;
        CC_LOG_INFO("D3D12 clear end: barrier RENDER_TARGET->PRESENT (stub).");
    }
}

void CCD3D12CommandBuffer::beginRenderPass(RenderPass *renderPass, Framebuffer *fbo, const Rect &renderArea, const Color *colors, float depth, uint32_t stencil, CommandBuffer *const *secondaryCBs, uint32_t secondaryCBCount) {
    (void)renderPass;
    (void)fbo;
    (void)renderArea;
    (void)colors;
    (void)depth;
    (void)stencil;
    (void)secondaryCBs;
    (void)secondaryCBCount;
}

void CCD3D12CommandBuffer::endRenderPass() {
}

void CCD3D12CommandBuffer::insertMarker(const MarkerInfo &marker) {
    (void)marker;
}

void CCD3D12CommandBuffer::beginMarker(const MarkerInfo &marker) {
    (void)marker;
}

void CCD3D12CommandBuffer::endMarker() {
}

void CCD3D12CommandBuffer::execute(CommandBuffer *const *cmdBuffs, uint32_t count) {
    (void)cmdBuffs;
    (void)count;
}

void CCD3D12CommandBuffer::bindPipelineState(PipelineState *pso) {
    (void)pso;
}

void CCD3D12CommandBuffer::bindDescriptorSet(uint32_t set, DescriptorSet *descriptorSet, uint32_t dynamicOffsetCount, const uint32_t *dynamicOffsets) {
    (void)set;
    (void)descriptorSet;
    (void)dynamicOffsetCount;
    (void)dynamicOffsets;
}

void CCD3D12CommandBuffer::bindInputAssembler(InputAssembler *ia) {
    (void)ia;
}

void CCD3D12CommandBuffer::setViewport(const Viewport &vp) {
    (void)vp;
}

void CCD3D12CommandBuffer::setScissor(const Rect &rect) {
    (void)rect;
}

void CCD3D12CommandBuffer::setLineWidth(float width) {
    (void)width;
}

void CCD3D12CommandBuffer::setDepthBias(float constant, float clamp, float slope) {
    (void)constant;
    (void)clamp;
    (void)slope;
}

void CCD3D12CommandBuffer::setBlendConstants(const Color &constants) {
    (void)constants;
}

void CCD3D12CommandBuffer::setDepthBound(float minBounds, float maxBounds) {
    (void)minBounds;
    (void)maxBounds;
}

void CCD3D12CommandBuffer::setStencilWriteMask(StencilFace face, uint32_t mask) {
    (void)face;
    (void)mask;
}

void CCD3D12CommandBuffer::setStencilCompareMask(StencilFace face, uint32_t ref, uint32_t mask) {
    (void)face;
    (void)ref;
    (void)mask;
}

void CCD3D12CommandBuffer::nextSubpass() {
}

void CCD3D12CommandBuffer::draw(const DrawInfo &info) {
    (void)info;
}

void CCD3D12CommandBuffer::updateBuffer(Buffer *buff, const void *data, uint32_t size) {
    (void)buff;
    (void)data;
    (void)size;
}

void CCD3D12CommandBuffer::copyBuffersToTexture(const uint8_t *const *buffers, Texture *texture, const BufferTextureCopy *regions, uint32_t count) {
    (void)buffers;
    (void)texture;
    (void)regions;
    (void)count;
}

void CCD3D12CommandBuffer::blitTexture(Texture *srcTexture, Texture *dstTexture, const TextureBlit *regions, uint32_t count, Filter filter) {
    (void)srcTexture;
    (void)dstTexture;
    (void)regions;
    (void)count;
    (void)filter;
}

void CCD3D12CommandBuffer::copyTexture(Texture *srcTexture, Texture *dstTexture, const TextureCopy *regions, uint32_t count) {
    (void)srcTexture;
    (void)dstTexture;
    (void)regions;
    (void)count;
}

void CCD3D12CommandBuffer::resolveTexture(Texture *srcTexture, Texture *dstTexture, const TextureCopy *regions, uint32_t count) {
    (void)srcTexture;
    (void)dstTexture;
    (void)regions;
    (void)count;
}

void CCD3D12CommandBuffer::dispatch(const DispatchInfo &info) {
    (void)info;
}

void CCD3D12CommandBuffer::pipelineBarrier(const GeneralBarrier *barrier, const BufferBarrier *const *bufferBarriers, const Buffer *const *buffers, uint32_t bufferCount, const TextureBarrier *const *textureBarriers, const Texture *const *textures, uint32_t textureBarrierCount) {
    (void)barrier;
    (void)bufferBarriers;
    (void)buffers;
    (void)bufferCount;
    (void)textureBarriers;
    (void)textures;
    (void)textureBarrierCount;
}

void CCD3D12CommandBuffer::beginQuery(QueryPool *queryPool, uint32_t id) {
    (void)queryPool;
    (void)id;
}

void CCD3D12CommandBuffer::endQuery(QueryPool *queryPool, uint32_t id) {
    (void)queryPool;
    (void)id;
}

void CCD3D12CommandBuffer::resetQueryPool(QueryPool *queryPool) {
    (void)queryPool;
}

} // namespace gfx
} // namespace cc
