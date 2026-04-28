/****************************************************************************
 Copyright (c) 2020-2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
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

#include "D3D12DescriptorSetLayout.h"
#include "base/Log.h"
#include "gfx-base/GFXDef.h"

namespace cc {
namespace gfx {

struct CCD3D12DescriptorSetLayout::Impl {
    // Per-type descriptor counts, accumulated from bindings
    uint32_t samplerCount{0};
    uint32_t textureCount{0};       // SRV descriptors (TEXTURE + SAMPLER_TEXTURE)
    uint32_t bufferCount{0};        // CBV/SRV/UAV descriptors for buffers
    uint32_t storageImageCount{0};  // UAV descriptors for images
    uint32_t inputAttachmentCount{0};

    // Per-binding descriptor type for quick lookup
    ccstd::vector<DescriptorType> bindingTypes;

    void computeCounts(const DescriptorSetLayoutBindingList &bindings) {
        samplerCount = 0;
        textureCount = 0;
        bufferCount = 0;
        storageImageCount = 0;
        inputAttachmentCount = 0;

        for (const auto &binding : bindings) {
            switch (binding.descriptorType) {
                case DescriptorType::SAMPLER_TEXTURE:
                    samplerCount += binding.count;
                    textureCount += binding.count;
                    break;
                case DescriptorType::SAMPLER:
                    samplerCount += binding.count;
                    break;
                case DescriptorType::TEXTURE:
                    textureCount += binding.count;
                    break;
                case DescriptorType::UNIFORM_BUFFER:
                case DescriptorType::DYNAMIC_UNIFORM_BUFFER:
                    bufferCount += binding.count;
                    break;
                case DescriptorType::STORAGE_BUFFER:
                case DescriptorType::DYNAMIC_STORAGE_BUFFER:
                    bufferCount += binding.count;
                    break;
                case DescriptorType::STORAGE_IMAGE:
                    storageImageCount += binding.count;
                    break;
                case DescriptorType::INPUT_ATTACHMENT:
                    inputAttachmentCount += binding.count;
                    textureCount += binding.count;
                    break;
                default:
                    break;
            }
        }
    }
};

CCD3D12DescriptorSetLayout::CCD3D12DescriptorSetLayout()
: _impl(std::make_unique<Impl>()) {
}

CCD3D12DescriptorSetLayout::~CCD3D12DescriptorSetLayout() {
    destroy();
}

void CCD3D12DescriptorSetLayout::doInit(const DescriptorSetLayoutInfo &info) {
    _impl->computeCounts(_bindings);

    // Store binding types for quick lookup by binding index
    uint32_t maxBinding = 0;
    for (const auto &b : _bindings) {
        if (b.binding > maxBinding) maxBinding = b.binding;
    }
    _impl->bindingTypes.resize(maxBinding + 1, DescriptorType::UNKNOWN);
    for (const auto &b : _bindings) {
        _impl->bindingTypes[b.binding] = b.descriptorType;
    }

    CC_LOG_INFO("D3D12 DescriptorSetLayout initialized: %u bindings, %u total descriptors "
                "(sampler=%u, texture=%u, buffer=%u, image=%u, input=%u)",
                static_cast<uint32_t>(_bindings.size()), _descriptorCount,
                _impl->samplerCount, _impl->textureCount,
                _impl->bufferCount, _impl->storageImageCount,
                _impl->inputAttachmentCount);
}

void CCD3D12DescriptorSetLayout::doDestroy() {
    if (_impl) {
        _impl->bindingTypes.clear();
        _impl->samplerCount = 0;
        _impl->textureCount = 0;
        _impl->bufferCount = 0;
        _impl->storageImageCount = 0;
        _impl->inputAttachmentCount = 0;
    }
}

uint32_t CCD3D12DescriptorSetLayout::getSamplerCount() const {
    return _impl ? _impl->samplerCount : 0;
}

uint32_t CCD3D12DescriptorSetLayout::getTextureCount() const {
    return _impl ? _impl->textureCount : 0;
}

uint32_t CCD3D12DescriptorSetLayout::getBufferCount() const {
    return _impl ? _impl->bufferCount : 0;
}

uint32_t CCD3D12DescriptorSetLayout::getStorageImageCount() const {
    return _impl ? _impl->storageImageCount : 0;
}

uint32_t CCD3D12DescriptorSetLayout::getInputAttachmentCount() const {
    return _impl ? _impl->inputAttachmentCount : 0;
}

uint32_t CCD3D12DescriptorSetLayout::getDescriptorCountByType(DescriptorType type) const {
    if (!_impl) return 0;
    switch (type) {
        case DescriptorType::SAMPLER_TEXTURE:
            return _impl->samplerCount > _impl->textureCount
                       ? _impl->samplerCount : _impl->textureCount;
        case DescriptorType::SAMPLER:
            return _impl->samplerCount;
        case DescriptorType::TEXTURE:
            return _impl->textureCount;
        case DescriptorType::UNIFORM_BUFFER:
        case DescriptorType::DYNAMIC_UNIFORM_BUFFER:
        case DescriptorType::STORAGE_BUFFER:
        case DescriptorType::DYNAMIC_STORAGE_BUFFER:
            return _impl->bufferCount;
        case DescriptorType::STORAGE_IMAGE:
            return _impl->storageImageCount;
        case DescriptorType::INPUT_ATTACHMENT:
            return _impl->inputAttachmentCount;
        default:
            return 0;
    }
}

} // namespace gfx
} // namespace cc
