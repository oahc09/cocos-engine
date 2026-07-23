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

#include "InstancedBuffer.h"
#include "Define.h"
#include "gfx-base/GFXBuffer.h"
#include "gfx-base/GFXCommandBuffer.h"
#include "gfx-base/GFXDescriptorSet.h"
#include "gfx-base/GFXDevice.h"
#include "gfx-base/GFXInputAssembler.h"
#include "gfx-base/GFXShader.h"
#include "scene/Model.h"
#include "scene/SubModel.h"

namespace cc {
namespace pipeline {

InstancedBuffer::InstancedBuffer(const scene::Pass *pass)
: _pass(pass),
  _device(gfx::Device::getInstance()) {
}

InstancedBuffer::~InstancedBuffer() {
    destroy();
}

void InstancedBuffer::destroy() {
    for (auto &instance : _instances) {
        CC_SAFE_DESTROY_AND_DELETE(instance.vb);
        CC_SAFE_DESTROY_AND_DELETE(instance.ia);
        CC_FREE(instance.data);
    }
    _instances.clear();
}

void InstancedBuffer::merge(scene::SubModel *subModel, uint32_t passIdx) {
    merge(subModel, passIdx, nullptr);
}

void InstancedBuffer::merge(scene::SubModel *subModel, uint32_t passIdx, gfx::Shader *shaderImplant) {
    auto &attrs = subModel->getInstancedAttributeBlock();

    const auto stride = attrs.buffer.length();
    if (!stride) return; // we assume per-instance attributes are always present

    auto *sourceIA = subModel->getInputAssembler();
    auto *descriptorSet = subModel->getDescriptorSet();
    auto *lightingMap = descriptorSet->getTexture(LIGHTMAPTEXTURE::BINDING);
    auto *reflectionProbeCubemap = descriptorSet->getTexture(REFLECTIONPROBECUBEMAP::BINDING);
    auto *reflectionProbePlanarMap = descriptorSet->getTexture(REFLECTIONPROBEPLANARMAP::BINDING);
    gfx::Texture *reflectionProbeBlendCubemap = ENABLE_PROBE_BLEND
        ? descriptorSet->getTexture(REFLECTIONPROBEBLENDCUBEMAP::BINDING)
        : nullptr;
    const uint32_t reflectionProbeType = subModel->getReflectionProbeType();
    auto *shader = shaderImplant;
    if (!shader) {
        shader = subModel->getShader(passIdx);
    }
    auto passPriority = static_cast<uint32_t>(subModel->getPass(passIdx)->getPriority());
    auto modelPriority = static_cast<uint32_t>(subModel->getPriority());
    auto shaderId = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(subModel->getShader(passIdx)));
    const auto hash = (passPriority << 16) | (modelPriority << 8) | passIdx;
    _sortRender.hash = hash;
    _sortRender.shaderID = shaderId;
    _sortRender.passIndex = passIdx;
    for (auto &instance : _instances) {
        if (instance.ia->getIndexBuffer() != sourceIA->getIndexBuffer() || instance.drawInfo.instanceCount >= MAX_CAPACITY) {
            continue;
        }

        // check same binding
        if (instance.lightingMap != lightingMap) {
            continue;
        }

        if (instance.reflectionProbeType != reflectionProbeType) {
            continue;
        }
        if (instance.reflectionProbeCubemap != reflectionProbeCubemap) {
            continue;
        }
        if (instance.reflectionProbePlanarMap != reflectionProbePlanarMap) {
            continue;
        }
        if (instance.reflectionProbeBlendCubemap != reflectionProbeBlendCubemap) {
            continue;
        }

        if (instance.stride != stride) {
            continue;
        }
        if (instance.drawInfo.instanceCount >= instance.capacity) { // resize buffers
            instance.capacity <<= 1;
            const auto newSize = instance.stride * instance.capacity;
            // NOLINTNEXTLINE(bugprone-suspicious-realloc-usage)
            instance.data = static_cast<uint8_t *>(CC_REALLOC(instance.data, newSize));
            instance.vb->resize(newSize);
        }
        if (instance.shader != shader) {
            instance.shader = shader;
        }
        if (instance.descriptorSet != descriptorSet) {
            instance.descriptorSet = descriptorSet;
        }
        memcpy(instance.data + static_cast<size_t>(instance.stride) * instance.drawInfo.instanceCount++, attrs.buffer.buffer()->getData(), stride);
        _hasPendingModels = true;
        return;
    }

    // Create a new instance
    const auto newSize = stride * INITIAL_CAPACITY;
    auto *vb = _device->createBuffer({
        gfx::BufferUsageBit::VERTEX | gfx::BufferUsageBit::TRANSFER_DST,
        gfx::MemoryUsageBit::DEVICE,
        static_cast<uint32_t>(newSize),
        static_cast<uint32_t>(stride),
    });

    auto vertexBuffers = sourceIA->getVertexBuffers();
    auto attributes = sourceIA->getAttributes();
    auto *indexBuffer = sourceIA->getIndexBuffer();

    for (const auto &attribute : attrs.attributes) {
        attributes.emplace_back(gfx::Attribute{
            attribute.name,
            attribute.format,
            attribute.isNormalized,
            static_cast<uint32_t>(vertexBuffers.size()), // stream
            true,
            attribute.location});
    }

    auto *data = static_cast<uint8_t *>(CC_MALLOC(newSize));
    memcpy(data, attrs.buffer.buffer()->getData(), stride);
    vertexBuffers.emplace_back(vb);
    const gfx::InputAssemblerInfo iaInfo = {attributes, vertexBuffers, indexBuffer};
    auto *ia = _device->createInputAssembler(iaInfo);
    InstancedItem item = {INITIAL_CAPACITY, vb, data, ia, stride, shader, descriptorSet,
                          lightingMap, reflectionProbeCubemap, reflectionProbePlanarMap, reflectionProbeType, reflectionProbeBlendCubemap,
                          ia->getDrawInfo()};
    item.drawInfo.instanceCount = 1;
    _instances.emplace_back(item);
    _hasPendingModels = true;
}

bool InstancedBuffer::mergeWorldMatrix(const scene::SubModel *subModel, uint32_t passIdx, gfx::Shader *shader) {
    if (!subModel || !shader || !_device) return false;
    auto *model = subModel->getOwner();
    auto *transform = model ? model->getTransform() : nullptr;
    auto *sourceIA = subModel->getInputAssembler();
    if (!model || !transform || !sourceIA) return false;

    if (_controlledLayout.shader != shader) {
        _controlledLayout = {};
        _controlledLayout.shader = shader;
        for (const auto &attribute : shader->getAttributes()) {
            if (!attribute.isInstanced) continue;
            int32_t *offset = nullptr;
            if (attribute.name == "a_matWorld0") {
                offset = &_controlledLayout.worldOffsets[0];
            } else if (attribute.name == "a_matWorld1") {
                offset = &_controlledLayout.worldOffsets[1];
            } else if (attribute.name == "a_matWorld2") {
                offset = &_controlledLayout.worldOffsets[2];
            } else if (attribute.name == "a_localShadowBiasAndProbeId") {
                offset = &_controlledLayout.shadowOffset;
            } else {
                return false;
            }
            if (attribute.format != gfx::Format::RGBA32F || *offset >= 0) return false;
            *offset = static_cast<int32_t>(_controlledLayout.stride);
            _controlledLayout.attributes.emplace_back(attribute);
            _controlledLayout.stride += gfx::GFX_FORMAT_INFOS[static_cast<uint32_t>(attribute.format)].size;
        }
        _controlledLayout.valid =
            _controlledLayout.worldOffsets[0] >= 0 &&
            _controlledLayout.worldOffsets[1] >= 0 &&
            _controlledLayout.worldOffsets[2] >= 0 &&
            (_controlledLayout.attributes.size() == 3 || _controlledLayout.attributes.size() == 4);
    }
    if (!_controlledLayout.valid) return false;
    const uint32_t stride = _controlledLayout.stride;

    const auto geometryMatches = [sourceIA](const InstancedItem &item) {
        if (!item.ia || item.ia->getIndexBuffer() != sourceIA->getIndexBuffer()) return false;
        const auto &sourceDraw = sourceIA->getDrawInfo();
        const auto &itemDraw = item.ia->getDrawInfo();
        if (sourceDraw.vertexCount != itemDraw.vertexCount ||
            sourceDraw.firstVertex != itemDraw.firstVertex ||
            sourceDraw.indexCount != itemDraw.indexCount ||
            sourceDraw.firstIndex != itemDraw.firstIndex ||
            sourceDraw.vertexOffset != itemDraw.vertexOffset ||
            sourceDraw.firstInstance != itemDraw.firstInstance) return false;
        const auto &sourceBuffers = sourceIA->getVertexBuffers();
        const auto &itemBuffers = item.ia->getVertexBuffers();
        if (itemBuffers.size() != sourceBuffers.size() + 1) return false;
        for (uint32_t i = 0; i < sourceBuffers.size(); ++i) {
            if (itemBuffers[i] != sourceBuffers[i]) return false;
        }
        return true;
    };

    InstancedItem *target = nullptr;
    for (auto &instance : _instances) {
        if (instance.drawInfo.instanceCount < MAX_CAPACITY && instance.stride == stride && geometryMatches(instance)) {
            target = &instance;
            break;
        }
    }

    if (!target) {
        const uint32_t newSize = stride * INITIAL_CAPACITY;
        const auto memoryUsage = _device->getGfxAPI() == gfx::API::D3D12
                                     ? gfx::MemoryUsageBit::HOST | gfx::MemoryUsageBit::DEVICE
                                     : gfx::MemoryUsageBit::DEVICE;
        auto *vb = _device->createBuffer({gfx::BufferUsageBit::VERTEX | gfx::BufferUsageBit::TRANSFER_DST,
                                          memoryUsage, newSize, stride});
        if (!vb) return false;
        auto vertexBuffers = sourceIA->getVertexBuffers();
        auto attributes = sourceIA->getAttributes();
        const uint32_t stream = static_cast<uint32_t>(vertexBuffers.size());
        for (auto attribute : _controlledLayout.attributes) {
            attribute.stream = stream;
            attribute.isInstanced = true;
            attributes.emplace_back(std::move(attribute));
        }
        vertexBuffers.emplace_back(vb);
        auto *ia = _device->createInputAssembler({attributes, vertexBuffers, sourceIA->getIndexBuffer()});
        auto *data = static_cast<uint8_t *>(CC_MALLOC(newSize));
        if (!ia || !data) {
            CC_SAFE_DESTROY_AND_DELETE(ia);
            CC_SAFE_DESTROY_AND_DELETE(vb);
            CC_FREE(data);
            return false;
        }
        InstancedItem item = {INITIAL_CAPACITY, vb, data, ia, stride, shader, subModel->getDescriptorSet(),
                              nullptr, nullptr, nullptr, 0, nullptr, ia->getDrawInfo()};
        item.drawInfo.instanceCount = 0;
        _instances.emplace_back(item);
        target = &_instances.back();
    }

    if (target->drawInfo.instanceCount >= target->capacity) {
        const uint32_t newCapacity = std::min(target->capacity << 1, MAX_CAPACITY);
        const uint32_t newSize = target->stride * newCapacity;
        auto *newData = static_cast<uint8_t *>(CC_REALLOC(target->data, newSize));
        if (!newData) return false;
        target->data = newData;
        target->capacity = newCapacity;
        target->vb->resize(newSize);
    }

    const Mat4 &world = transform->getWorldMatrix();
    auto *base = target->data + static_cast<size_t>(target->stride) * target->drawInfo.instanceCount;
    auto writeVector = [base](int32_t offset, float x, float y, float z, float w) {
        auto *dst = reinterpret_cast<float *>(base + offset);
        dst[0] = x; dst[1] = y; dst[2] = z; dst[3] = w;
    };
    writeVector(_controlledLayout.worldOffsets[0], world.m[0], world.m[1], world.m[2], world.m[12]);
    writeVector(_controlledLayout.worldOffsets[1], world.m[4], world.m[5], world.m[6], world.m[13]);
    writeVector(_controlledLayout.worldOffsets[2], world.m[8], world.m[9], world.m[10], world.m[14]);
    if (_controlledLayout.shadowOffset >= 0) {
        writeVector(_controlledLayout.shadowOffset,
                    model->getShadowBias(), model->getShadowNormalBias(),
                    static_cast<float>(model->getReflectionProbeId()),
                    static_cast<float>(model->getReflectionProbeBlendId()));
    }
    target->shader = shader;
    target->descriptorSet = subModel->getDescriptorSet();
    ++target->drawInfo.instanceCount;
    _hasPendingModels = true;
    _sortRender.passIndex = passIdx;
    return true;
}

gfx::Shader *InstancedBuffer::getControlledShader(const scene::SubModel *subModel, uint32_t passIdx) {
    if (!subModel) return nullptr;
    const auto *pass = subModel->getPass(passIdx);
    auto *sourceShader = subModel->getShader(passIdx);
    if (!pass || !sourceShader) return nullptr;
    if (_controlledShaderPass == pass && _controlledSourceShader == sourceShader) {
        return _controlledShader;
    }
    MacroRecord overrides{{"USE_INSTANCING", MacroValue(true)}};
    _controlledShaderPass = pass;
    _controlledSourceShader = sourceShader;
    _controlledShader = pass->getShaderVariantWithOverrides(subModel->getPatches(), overrides);
    return _controlledShader;
}

uint32_t InstancedBuffer::getPendingInstanceCount() const {
    uint32_t count = 0;
    for (const auto &instance : _instances) count += instance.drawInfo.instanceCount;
    return count;
}

void InstancedBuffer::uploadBuffers(gfx::CommandBuffer *cmdBuff) const {
    for (const auto &instance : _instances) {
        if (!instance.drawInfo.instanceCount) continue;

        cmdBuff->updateBuffer(instance.vb, instance.data, instance.vb->getSize());
        instance.ia->setInstanceCount(instance.drawInfo.instanceCount);
    }
}

void InstancedBuffer::clear() {
    for (auto &instance : _instances) {
        instance.drawInfo.instanceCount = 0;
    }
    _hasPendingModels = false;
}

void InstancedBuffer::setDynamicOffset(uint32_t idx, uint32_t value) {
    if (_dynamicOffsets.size() <= idx) _dynamicOffsets.resize(1 + idx);
    _dynamicOffsets[idx] = value;
}
} // namespace pipeline
} // namespace cc
