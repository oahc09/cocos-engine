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

#include "RenderQueue.h"

#include <utility>
#include "base/std/container/unordered_set.h"
#include "PipelineSceneData.h"
#include "PipelineStateManager.h"
#include "RenderPipeline.h"
#include "InstancedBuffer.h"
#include "gfx-base/GFXCommandBuffer.h"
#include "gfx-base/GFXDescriptorSetLayout.h"
#include "gfx-base/GFXDevice.h"
#include "gfx-base/GFXShader.h"
#include "scene/Model.h"
#include "scene/Pass.h"
#include "scene/SubModel.h"

namespace cc {
namespace pipeline {

namespace {
constexpr uint32_t AUTO_INSTANCE_MIN_RUN = 4;

bool macroValueEnabled(const MacroValue &value) {
    if (const auto *enabled = ccstd::get_if<bool>(&value)) return *enabled;
    if (const auto *enabled = ccstd::get_if<int32_t>(&value)) return *enabled != 0;
    return false;
}

const ccstd::unordered_set<ccstd::string> &unsupportedAutoInstanceMacros() {
    static const ccstd::unordered_set<ccstd::string> unsupported = {
        "CC_USE_LIGHTMAP", "CC_USE_LIGHT_PROBE", "CC_USE_REFLECTION_PROBE", "CC_USE_SKINNING",
        "CC_USE_BAKED_ANIMATION", "CC_USE_MORPH", "CC_USE_REAL_TIME_JOINT_TEXTURE"};
    return unsupported;
}

bool hasUnsupportedAutoInstancePatch(const scene::SubModel *subModel) {
    const auto &unsupported = unsupportedAutoInstanceMacros();
    for (const auto &patch : subModel->getPatches()) {
        if (unsupported.count(patch.name) && macroValueEnabled(patch.value)) return true;
    }
    return false;
}

bool hasUnsupportedAutoInstanceMacro(const scene::Pass *pass, const scene::SubModel *subModel) {
    const auto &unsupported = unsupportedAutoInstanceMacros();
    for (const auto &define : pass->getDefines()) {
        if (unsupported.count(define.first) && macroValueEnabled(define.second)) return true;
    }
    return hasUnsupportedAutoInstancePatch(subModel);
}

bool hasWorldLocalBinding(const gfx::DescriptorSet *descriptorSet) {
    if (!descriptorSet || !descriptorSet->getLayout()) return false;
    const auto &bindings = descriptorSet->getLayout()->getBindings();
    return std::any_of(bindings.begin(), bindings.end(), [](const gfx::DescriptorSetLayoutBinding &binding) {
        return binding.binding == 0 && binding.count == 1 &&
               hasAnyFlags(binding.descriptorType, gfx::DESCRIPTOR_BUFFER_TYPE);
    });
}

bool isAutoInstanceCandidate(const scene::SubModel *subModel, uint32_t passIdx) {
    if (!subModel || !subModel->getOwner()) return false;
    const auto *model = subModel->getOwner();
    const auto *pass = subModel->getPass(passIdx);
    if (!pass) return false;
    const auto &program = pass->getProgram();
    const bool supportedStandardProgram =
        program.find("builtin-standard|") == 0 ||
        program.find("legacy/standard|") == 0;
    return model->getType() == scene::Model::Type::DEFAULT && !model->getUseLightProbe() &&
           model->getReflectionProbeType() == scene::UseReflectionProbeType::NONE &&
           pass->getBatchingScheme() == scene::BatchingSchemes::NONE &&
           supportedStandardProgram &&
           hasWorldLocalBinding(subModel->getDescriptorSet()) &&
           !hasUnsupportedAutoInstanceMacro(pass, subModel);
}

bool sameDrawInfo(const gfx::DrawInfo &a, const gfx::DrawInfo &b) {
    return a.vertexCount == b.vertexCount && a.firstVertex == b.firstVertex &&
           a.indexCount == b.indexCount && a.firstIndex == b.firstIndex &&
           a.vertexOffset == b.vertexOffset && a.firstInstance == b.firstInstance;
}

bool sameGeometry(const gfx::InputAssembler *a, const gfx::InputAssembler *b) {
    if (a == b) return true;
    if (!a || !b || a->getIndexBuffer() != b->getIndexBuffer() ||
        a->getAttributesHash() != b->getAttributesHash() ||
        !sameDrawInfo(a->getDrawInfo(), b->getDrawInfo())) return false;
    const auto &aBuffers = a->getVertexBuffers();
    const auto &bBuffers = b->getVertexBuffers();
    if (aBuffers.size() != bBuffers.size()) return false;
    for (uint32_t i = 0; i < aBuffers.size(); ++i) {
        if (aBuffers[i] != bBuffers[i]) return false;
    }
    return true;
}

AutoInstanceQueueSignature makeAutoInstanceQueueSignature(const RenderPass &entry) {
    return {entry.pass, entry.shaderID, entry.passIndex};
}

bool sameAutoInstanceQueueSignature(const AutoInstanceQueueSignature &a,
                                    const AutoInstanceQueueSignature &b) {
    return a.pass == b.pass && a.shaderID == b.shaderID &&
           a.passIndex == b.passIndex;
}

bool sameAutoInstanceRun(const scene::SubModel *first, const scene::SubModel *next, uint32_t passIdx) {
    if (!next || !next->getOwner() || first->getPass(passIdx) != next->getPass(passIdx)) return false;
    const auto *nextModel = next->getOwner();
    if (nextModel->getType() != scene::Model::Type::DEFAULT || nextModel->getUseLightProbe() ||
        nextModel->getReflectionProbeType() != scene::UseReflectionProbeType::NONE ||
        hasUnsupportedAutoInstancePatch(next)) return false;
    if (first->getShader(passIdx) != next->getShader(passIdx) ||
        first->getPass(passIdx)->getDescriptorSet() != next->getPass(passIdx)->getDescriptorSet() ||
        !sameGeometry(first->getInputAssembler(), next->getInputAssembler()) ||
        first->getDescriptorSet()->getLayout() != next->getDescriptorSet()->getLayout()) return false;
    const auto *a = first->getOwner();
    const auto *b = next->getOwner();
    return a->isReceiveShadow() == b->isReceiveShadow() && a->isReceiveDirLight() == b->isReceiveDirLight() &&
           a->getShadowBias() == b->getShadowBias() && a->getShadowNormalBias() == b->getShadowNormalBias();
}
} // namespace

RenderQueue::RenderQueue(RenderPipeline *pipeline, RenderQueueCreateInfo desc, bool useOcclusionQuery)
: _pipeline(pipeline), _passDesc(std::move(desc)), _useOcclusionQuery(useOcclusionQuery) {
}

RenderQueue::~RenderQueue() {
    for (auto *buffer : _autoInstancedBuffers) CC_SAFE_DELETE(buffer);
}

void RenderQueue::clear() {
    _queue.clear();
}

bool RenderQueue::insertRenderPass(const RenderObject &renderObj, uint32_t subModelIdx, uint32_t passIdx) {
    const auto *subModel = renderObj.model->getSubModels()[subModelIdx].get();
    const auto *const pass = subModel->getPass(passIdx);
    const bool isTransparent = pass->getBlendState()->targets[0].blend;

    if (isTransparent != _passDesc.isTransparent || !(pass->getPhase() & _passDesc.phases)) {
        return false;
    }

    auto passPriority = static_cast<uint32_t>(pass->getPriority());
    auto modelPriority = static_cast<uint32_t>(subModel->getPriority());
    auto shaderId = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(subModel->getShader(passIdx)));
    const auto hash = (0 << 30) | (passPriority << 16) | (modelPriority << 8) | passIdx;
    const auto priority = renderObj.model->getPriority();
    RenderPass renderPass = {priority, hash, renderObj.depth, shaderId, passIdx, subModel, pass};
    _queue.emplace_back(renderPass);

    return true;
}

void RenderQueue::sort() {
#if CC_PLATFORM != CC_PLATFORM_LINUX && CC_PLATFORM != CC_PLATFORM_QNX
    std::sort(_queue.begin(), _queue.end(), _passDesc.sortFunc);
#else
    //cannot use std::function as comparator in algorithms https://gcc.gnu.org/bugzilla/show_bug.cgi?id=65942#c11
    std::sort(_queue.begin(), _queue.end(), [this](const RenderPass &a, const RenderPass &b) -> bool {
        return _passDesc.sortFunc(a, b);
    });
#endif
}

void RenderQueue::recordCommandBuffer(gfx::Device * /*device*/, scene::Camera *camera, gfx::RenderPass *renderPass, gfx::CommandBuffer *cmdBuff, uint32_t subpassIndex) {
    PipelineSceneData *const sceneData = _pipeline->getPipelineSceneData();
    bool enableOcclusionQuery = _pipeline->isOcclusionQueryEnabled() && _useOcclusionQuery;
    auto *queryPool = _pipeline->getQueryPools()[0];
    const bool useDrawBatch = !enableOcclusionQuery && cmdBuff->supportsDrawBatch();
    if (useDrawBatch) {
        _drawPackets.clear();
        _drawPackets.reserve(_queue.size());
        cmdBuff->beginDrawBatch();
    }
    const auto flushDrawPackets = [&]() {
        if (!_drawPackets.empty()) {
            cmdBuff->drawPackets(_drawPackets.data(), utils::toUint(_drawPackets.size()), materialSet, localSet);
            _drawPackets.clear();
        }
    };
    uint32_t runIndex = 0;
    for (uint32_t queueIndex = 0; queueIndex < _queue.size();) {
        if (useDrawBatch && runIndex < _autoInstancedRuns.size() &&
            _autoInstancedRuns[runIndex].first == queueIndex) {
            const auto &run = _autoInstancedRuns[runIndex++];
            if (!run.ready) {
                // The complete-run proof failed; preserve every original draw.
            } else {
                flushDrawPackets();
                const auto *pass = run.buffer->getPass();
                cmdBuff->bindDescriptorSet(materialSet, pass->getDescriptorSet());
                for (const auto &instance : run.buffer->getInstances()) {
                    if (!instance.drawInfo.instanceCount) continue;
                    auto *pso = PipelineStateManager::getOrCreatePipelineState(
                        pass, instance.shader, instance.ia, renderPass, subpassIndex);
                    cmdBuff->bindPipelineState(pso);
                    cmdBuff->bindDescriptorSet(localSet, instance.descriptorSet);
                    cmdBuff->bindInputAssembler(instance.ia);
                    cmdBuff->draw(instance.ia);
                }
                queueIndex += run.count;
                continue;
            }
        }

        auto &i = _queue[queueIndex++];
        const auto *subModel = i.subModel;
        if (enableOcclusionQuery) {
            cmdBuff->beginQuery(queryPool, subModel->getId());
        }

        if (enableOcclusionQuery && _pipeline->isOccluded(camera, subModel)) {
            gfx::InputAssembler *inputAssembler = sceneData->getOcclusionQueryInputAssembler();
            const scene::Pass *pass = sceneData->getOcclusionQueryPass();
            gfx::Shader *shader = sceneData->getOcclusionQueryShader();
            auto *pso = PipelineStateManager::getOrCreatePipelineState(pass, shader, inputAssembler, renderPass, subpassIndex);

            cmdBuff->bindPipelineState(pso);
            cmdBuff->bindDescriptorSet(materialSet, pass->getDescriptorSet());
            cmdBuff->bindDescriptorSet(localSet, subModel->getWorldBoundDescriptorSet());
            cmdBuff->bindInputAssembler(inputAssembler);
            cmdBuff->draw(inputAssembler);
        } else {
            const auto passIdx = i.passIndex;
            auto *inputAssembler = subModel->getInputAssembler();
            const auto *pass = subModel->getPass(passIdx);
            auto *shader = subModel->getShader(passIdx);
            auto *pso = PipelineStateManager::getOrCreatePipelineState(pass, shader, inputAssembler, renderPass, subpassIndex);

            if (useDrawBatch) {
                _drawPackets.push_back({pso, pass->getDescriptorSet(), inputAssembler,
                                        subModel->getDescriptorSet(), inputAssembler->getDrawInfo()});
            } else {
                cmdBuff->bindPipelineState(pso);
                cmdBuff->bindDescriptorSet(materialSet, pass->getDescriptorSet());
                cmdBuff->drawWithInputAssemblerAndDescriptorSet(
                    inputAssembler, localSet, subModel->getDescriptorSet(), inputAssembler->getDrawInfo());
            }
        }

        if (enableOcclusionQuery) {
            cmdBuff->endQuery(queryPool, subModel->getId());
        }
    }
    if (useDrawBatch) {
        flushDrawPackets();
        cmdBuff->endDrawBatch();
    }
}

void RenderQueue::prepareAutoInstancing(gfx::Device *device, gfx::CommandBuffer *cmdBuff) {
    _autoInstancedRuns.clear();
    for (auto *buffer : _autoInstancedBuffers) buffer->clear();
    const bool enableOcclusionQuery = _pipeline->isOcclusionQueryEnabled() && _useOcclusionQuery;
    if (!device || !cmdBuff || device->getGfxAPI() != gfx::API::D3D12 ||
        !device->hasFeature(gfx::Feature::INSTANCED_ARRAYS) || _passDesc.isTransparent || enableOcclusionQuery) {
        return;
    }
    if (_queue.empty()) return;

    uint32_t bufferIndex = 0;
    bool topologyCacheHit = _autoInstanceQueueSignatures.size() == _queue.size();
    if (topologyCacheHit) {
        for (uint32_t i = 0; i < _queue.size(); ++i) {
            if (!sameAutoInstanceQueueSignature(
                    _autoInstanceQueueSignatures[i], makeAutoInstanceQueueSignature(_queue[i]))) {
                topologyCacheHit = false;
                break;
            }
        }
    }
    if (!topologyCacheHit) {
        _autoInstanceCachedRanges.clear();
        _autoInstanceQueueSignatures.resize(_queue.size());
        for (uint32_t i = 0; i < _queue.size(); ++i) {
            _autoInstanceQueueSignatures[i] = makeAutoInstanceQueueSignature(_queue[i]);
        }
        for (uint32_t first = 0; first < _queue.size();) {
            const auto &entry = _queue[first];
            auto *subModel = entry.subModel;
            const uint32_t passIdx = entry.passIndex;
            if (!isAutoInstanceCandidate(subModel, passIdx)) {
                ++first;
                continue;
            }
            uint32_t end = first + 1;
            while (end < _queue.size() && _queue[end].passIndex == passIdx &&
                   sameAutoInstanceRun(subModel, _queue[end].subModel, passIdx)) ++end;
            const uint32_t count = end - first;
            if (count >= AUTO_INSTANCE_MIN_RUN) {
                _autoInstanceCachedRanges.push_back({first, count});
            }
            first = end;
        }
    }

    for (const auto &range : _autoInstanceCachedRanges) {
        const uint32_t first = range.first;
        const uint32_t count = range.count;
        const uint32_t end = first + count;
        auto *subModel = _queue[first].subModel;
        const uint32_t passIdx = _queue[first].passIndex;

        if (bufferIndex == _autoInstancedBuffers.size()) {
            _autoInstancedBuffers.emplace_back(ccnew InstancedBuffer(subModel->getPass(passIdx)));
        }
        auto *buffer = _autoInstancedBuffers[bufferIndex++];
        buffer->setPass(subModel->getPass(passIdx));
        auto *shader = buffer->getControlledShader(subModel, passIdx);
        bool merged = shader != nullptr;
        for (uint32_t i = first; merged && i < end; ++i) {
            merged = buffer->mergeWorldMatrix(_queue[i].subModel, passIdx, shader);
        }
        const uint32_t pendingInstanceCount = buffer->getPendingInstanceCount();
        AutoInstancedRun run{first, count, buffer, false};
        run.ready = merged && pendingInstanceCount == run.count;
        if (run.ready) {
            buffer->uploadBuffers(cmdBuff);
        }
        else buffer->clear();
        _autoInstancedRuns.emplace_back(run);
    }
}

} // namespace pipeline
} // namespace cc
