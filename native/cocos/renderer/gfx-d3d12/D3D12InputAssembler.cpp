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

#include "D3D12InputAssembler.h"
#include "D3D12Buffer.h"
#include "D3D12Device.h"
#include "base/Log.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <cstring>

namespace cc {
namespace gfx {

static DXGI_FORMAT gfxFormatToDXGI(Format format) {
    switch (format) {
        case Format::R8:      return DXGI_FORMAT_R8_UNORM;
        case Format::R8SN:    return DXGI_FORMAT_R8_SNORM;
        case Format::R8UI:    return DXGI_FORMAT_R8_UINT;
        case Format::R8I:     return DXGI_FORMAT_R8_SINT;
        case Format::R16F:    return DXGI_FORMAT_R16_FLOAT;
        case Format::R16UI:   return DXGI_FORMAT_R16_UINT;
        case Format::R16I:    return DXGI_FORMAT_R16_SINT;
        case Format::R32F:    return DXGI_FORMAT_R32_FLOAT;
        case Format::R32UI:   return DXGI_FORMAT_R32_UINT;
        case Format::R32I:    return DXGI_FORMAT_R32_SINT;
        case Format::RG8:     return DXGI_FORMAT_R8G8_UNORM;
        case Format::RG8SN:   return DXGI_FORMAT_R8G8_SNORM;
        case Format::RG8UI:   return DXGI_FORMAT_R8G8_UINT;
        case Format::RG8I:    return DXGI_FORMAT_R8G8_SINT;
        case Format::RG16F:   return DXGI_FORMAT_R16G16_FLOAT;
        case Format::RG16UI:  return DXGI_FORMAT_R16G16_UINT;
        case Format::RG16I:   return DXGI_FORMAT_R16G16_SINT;
        case Format::RG32F:   return DXGI_FORMAT_R32G32_FLOAT;
        case Format::RG32UI:  return DXGI_FORMAT_R32G32_UINT;
        case Format::RG32I:   return DXGI_FORMAT_R32G32_SINT;
        case Format::RGB32F:  return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::RGB32UI: return DXGI_FORMAT_R32G32B32_UINT;
        case Format::RGB32I:  return DXGI_FORMAT_R32G32B32_SINT;
        case Format::RGBA8:   return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::BGRA8:   return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::SRGB8_A8: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::RGBA8SN: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case Format::RGBA8UI: return DXGI_FORMAT_R8G8B8A8_UINT;
        case Format::RGBA8I:  return DXGI_FORMAT_R8G8B8A8_SINT;
        case Format::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::RGBA16UI: return DXGI_FORMAT_R16G16B16A16_UINT;
        case Format::RGBA16I: return DXGI_FORMAT_R16G16B16A16_SINT;
        case Format::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::RGBA32UI: return DXGI_FORMAT_R32G32B32A32_UINT;
        case Format::RGBA32I: return DXGI_FORMAT_R32G32B32A32_SINT;
        case Format::RGB10A2:   return DXGI_FORMAT_R10G10B10A2_UNORM;
        case Format::R11G11B10F:  return DXGI_FORMAT_R11G11B10_FLOAT;
        default:               return DXGI_FORMAT_UNKNOWN;
    }
}

// Extract semantic name and index from attribute name
// e.g. "a_position" → "POSITION", 0
// e.g. "a_texcoord1" → "TEXCOORD", 1
// e.g. "a_normal" → "NORMAL", 0
static void extractSemantic(const ccstd::string &attrName, char *semanticName, uint32_t &semanticIndex) {
    semanticIndex = 0;

    // Map common cocos attribute names to HLSL semantics
    if (attrName == "a_position" || attrName == "POSITION") {
        std::strcpy(semanticName, "POSITION");
    } else if (attrName == "a_normal" || attrName == "NORMAL") {
        std::strcpy(semanticName, "NORMAL");
    } else if (attrName == "a_tangent" || attrName == "TANGENT") {
        std::strcpy(semanticName, "TANGENT");
    } else if (attrName == "a_bitangent" || attrName == "BITANGENT") {
        std::strcpy(semanticName, "BITANGENT");
    } else if (attrName == "a_color" || attrName == "COLOR") {
        std::strcpy(semanticName, "COLOR");
        semanticIndex = 0;
    } else if (attrName == "a_weights" || attrName == "BLENDWEIGHT") {
        std::strcpy(semanticName, "BLENDWEIGHT");
    } else if (attrName == "a_joints" || attrName == "BLENDINDICES") {
        std::strcpy(semanticName, "BLENDINDICES");
    } else {
        // Try to match TEXCOORD pattern: a_texcoord, a_texcoord0, a_texcoord1, TEXCOORD, TEXCOORD0, etc.
        const char *p = attrName.c_str();
        if (std::strstr(p, "texcoord") != nullptr || std::strstr(p, "uv") != nullptr ||
            std::strstr(p, "TEXCOORD") != nullptr) {
            std::strcpy(semanticName, "TEXCOORD");
            // Extract trailing number
            const char *end = p + attrName.size();
            const char *digitStart = end;
            while (digitStart > p && std::isdigit(static_cast<unsigned char>(*(digitStart - 1)))) {
                --digitStart;
            }
            if (digitStart < end) {
                semanticIndex = static_cast<uint32_t>(std::atoi(digitStart));
            }
        } else if (std::strstr(p, "color") != nullptr || std::strstr(p, "COLOR") != nullptr) {
            std::strcpy(semanticName, "COLOR");
            const char *end = p + attrName.size();
            const char *digitStart = end;
            while (digitStart > p && std::isdigit(static_cast<unsigned char>(*(digitStart - 1)))) {
                --digitStart;
            }
            if (digitStart < end) {
                semanticIndex = static_cast<uint32_t>(std::atoi(digitStart));
            }
        } else {
            // Fallback: use the attribute name directly (strip "a_" prefix)
            if (attrName.size() > 2 && attrName[0] == 'a' && attrName[1] == '_') {
                // Convert to uppercase
                size_t len = attrName.size() - 2;
                for (size_t i = 0; i < len && i < 63; ++i) {
                    semanticName[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(attrName[i + 2])));
                }
                semanticName[len] = '\0';
            } else {
                std::strncpy(semanticName, attrName.c_str(), 63);
                semanticName[63] = '\0';
            }
        }
    }
}

struct CCD3D12InputAssembler::Impl {
    ccstd::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    ccstd::vector<ccstd::string> semanticNames; // persistent storage for SemanticName pointers
    // Cached vertex buffer views
    ccstd::vector<D3D12_VERTEX_BUFFER_VIEW> vbViews;
    ccstd::vector<uint64_t> vbResourceVersions;
    D3D12_INDEX_BUFFER_VIEW ibView{};
    uint64_t indexBufferResourceVersion{0};
    bool hasIndexBuffer{false};
    uint32_t indexFormat{0}; // DXGI_FORMAT as uint32_t
};

CCD3D12InputAssembler::CCD3D12InputAssembler()
: _impl(std::make_unique<Impl>()) {
}

CCD3D12InputAssembler::~CCD3D12InputAssembler() = default;

void CCD3D12InputAssembler::doInit(const InputAssemblerInfo &info) {
    auto *device = CCD3D12Device::getInstance();
    if (!device) {
        CC_LOG_ERROR("D3D12InputAssembler: device not available.");
        return;
    }

    // Build D3D12_INPUT_ELEMENT_DESC array
    _impl->inputElements.resize(info.attributes.size());
    _impl->semanticNames.clear();
    _impl->semanticNames.reserve(info.attributes.size());
    for (size_t i = 0; i < info.attributes.size(); ++i) {
        const auto &attr = info.attributes[i];
        auto &element = _impl->inputElements[i];

        char semanticName[64] = {};
        uint32_t semanticIndex = 0;
        extractSemantic(attr.name, semanticName, semanticIndex);

        _impl->semanticNames.emplace_back(semanticName);
        element.SemanticName = _impl->semanticNames.back().c_str();
        element.SemanticIndex = semanticIndex;
        element.Format = gfxFormatToDXGI(attr.format);
        element.InputSlot = attr.stream;
        element.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
        element.InputSlotClass = attr.isInstanced
            ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
            : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        element.InstanceDataStepRate = attr.isInstanced ? 1 : 0;

        if (element.Format == DXGI_FORMAT_UNKNOWN) {
            CC_LOG_WARNING("D3D12InputAssembler: unsupported vertex format %u for attribute '%s'",
                           static_cast<unsigned>(attr.format), attr.name.c_str());
        }

    }

    _impl->vbViews.resize(info.vertexBuffers.size());
    _impl->vbResourceVersions.assign(info.vertexBuffers.size(), 0);
    _impl->hasIndexBuffer = (info.indexBuffer != nullptr);
    _impl->indexBufferResourceVersion = 0;
    refreshBufferViews();

    CC_LOG_DEBUG("D3D12InputAssembler initialized with %u attributes, %u vertex buffers.",
                 static_cast<unsigned>(info.attributes.size()),
                 static_cast<unsigned>(info.vertexBuffers.size()));
}

void CCD3D12InputAssembler::doDestroy() {
    _impl->inputElements.clear();
    _impl->semanticNames.clear();
    _impl->vbViews.clear();
    _impl->vbResourceVersions.clear();
    _impl->hasIndexBuffer = false;
    _impl->indexBufferResourceVersion = 0;
}

void *CCD3D12InputAssembler::getInputElementDescs() const {
    return _impl ? _impl->inputElements.data() : nullptr;
}

uint32_t CCD3D12InputAssembler::getInputElementDescCount() const {
    return _impl ? static_cast<uint32_t>(_impl->inputElements.size()) : 0;
}

uint32_t CCD3D12InputAssembler::getVertexBufferCount() const {
    return _impl ? static_cast<uint32_t>(_impl->vbViews.size()) : 0;
}

bool CCD3D12InputAssembler::refreshBufferViews() {
    if (!_impl) {
        return false;
    }

    bool changed = false;
    if (_impl->vbViews.size() != _vertexBuffers.size()) {
        _impl->vbViews.resize(_vertexBuffers.size());
        _impl->vbResourceVersions.assign(_vertexBuffers.size(), 0);
        changed = true;
    }

    for (size_t i = 0; i < _vertexBuffers.size(); ++i) {
        auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(_vertexBuffers[i]);
        const uint64_t resourceVersion = d3d12Buffer ? d3d12Buffer->getD3D12ResourceVersion() : 0;
        if (_impl->vbResourceVersions[i] == resourceVersion) {
            continue;
        }

        D3D12_VERTEX_BUFFER_VIEW view{};
        if (d3d12Buffer) {
            view.BufferLocation = d3d12Buffer->getD3D12GPUVirtualAddress();
            view.SizeInBytes = d3d12Buffer->getSize();
            view.StrideInBytes = d3d12Buffer->getStride();
        }
        _impl->vbViews[i] = view;
        _impl->vbResourceVersions[i] = resourceVersion;
        changed = true;
    }

    if (!_impl->hasIndexBuffer) {
        return changed;
    }

    auto *d3d12IndexBuffer = static_cast<CCD3D12Buffer *>(_indexBuffer);
    const uint64_t indexResourceVersion = d3d12IndexBuffer
                                              ? d3d12IndexBuffer->getD3D12ResourceVersion()
                                              : 0;
    if (_impl->indexBufferResourceVersion == indexResourceVersion) {
        return changed;
    }

    D3D12_INDEX_BUFFER_VIEW view{};
    if (d3d12IndexBuffer) {
        view.BufferLocation = d3d12IndexBuffer->getD3D12GPUVirtualAddress();
        view.SizeInBytes = d3d12IndexBuffer->getSize();
        view.Format = d3d12IndexBuffer->getStride() == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        _impl->indexFormat = static_cast<uint32_t>(view.Format);
    }
    _impl->ibView = view;
    _impl->indexBufferResourceVersion = indexResourceVersion;
    return true;
}

void CCD3D12InputAssembler::fillVertexBufferViews(void *views) const {
    if (!views || !_impl || _impl->vbViews.empty()) return;
    auto *dst = static_cast<D3D12_VERTEX_BUFFER_VIEW *>(views);
    std::memcpy(dst, _impl->vbViews.data(), sizeof(D3D12_VERTEX_BUFFER_VIEW) * _impl->vbViews.size());
}

bool CCD3D12InputAssembler::hasIndexBuffer() const {
    return _impl ? _impl->hasIndexBuffer : false;
}

void CCD3D12InputAssembler::fillIndexBufferView(void *view) const {
    if (!view || !_impl || !_impl->hasIndexBuffer) return;
    *static_cast<D3D12_INDEX_BUFFER_VIEW *>(view) = _impl->ibView;
}

uint32_t CCD3D12InputAssembler::getIndexFormat() const {
    if (!_indexBuffer) {
        return 0;
    }
    return _indexBuffer->getStride() == 4 ? static_cast<uint32_t>(DXGI_FORMAT_R32_UINT)
                                          : static_cast<uint32_t>(DXGI_FORMAT_R16_UINT);
}

} // namespace gfx
} // namespace cc
