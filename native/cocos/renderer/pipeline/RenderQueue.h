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

#include "Define.h"

namespace cc {
namespace scene {
class Camera;
class Pass;
class SubModel;
}
namespace pipeline {

class RenderPipeline;
class InstancedBuffer;

struct AutoInstancedRun {
    uint32_t first{0};
    uint32_t count{0};
    InstancedBuffer *buffer{nullptr};
    bool ready{false};
};

struct AutoInstanceQueueSignature {
    const scene::Pass *pass{nullptr};
    uint32_t shaderID{0};
    uint32_t passIndex{0};
};

struct AutoInstanceRange {
    uint32_t first{0};
    uint32_t count{0};
};

class CC_DLL RenderQueue final {
public:
    explicit RenderQueue(RenderPipeline *pipeline, RenderQueueCreateInfo desc, bool useOcclusionQuery = false);
    ~RenderQueue();

    void clear();
    bool insertRenderPass(const RenderObject &renderObj, uint32_t subModelIdx, uint32_t passIdx);
    void recordCommandBuffer(gfx::Device *device, scene::Camera *camera, gfx::RenderPass *renderPass, gfx::CommandBuffer *cmdBuff, uint32_t subpassIndex = 0);
    void sort();
    void prepareAutoInstancing(gfx::Device *device, gfx::CommandBuffer *cmdBuff);
    bool empty() { return _queue.empty(); }

private:
    // weak reference
    RenderPipeline *_pipeline{nullptr};
    RenderPassList _queue;
    gfx::DrawPacketList _drawPackets;
    ccstd::vector<InstancedBuffer *> _autoInstancedBuffers;
    ccstd::vector<AutoInstancedRun> _autoInstancedRuns;
    ccstd::vector<AutoInstanceQueueSignature> _autoInstanceQueueSignatures;
    ccstd::vector<AutoInstanceRange> _autoInstanceCachedRanges;
    RenderQueueCreateInfo _passDesc;
    bool _useOcclusionQuery{false};
};

} // namespace pipeline
} // namespace cc
