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

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <cstring>
#endif

namespace cc {
namespace gfx {

#if defined(_WIN32)
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
        case Format::RG32F:   return DXGI_FORMAT_R32G32_FLOAT;
        case Format::RGB32F:  return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::RGBA8:   return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::BGRA8:   return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::RGBA8SN: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case Format::RGBA8UI: return DXGI_FORMAT_R8G8B8A8_UINT;
        case Format::RGBA8I:  return DXGI_FORMAT_R8G8B8A8_SINT;
        case Format::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
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
#endif

struct CCD3D12InputAssembler::Impl {
#if defined(_WIN32)
    ccstd::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    ccstd::vector<ccstd::string> semanticNames; // persistent storage for SemanticName pointers
    // Cached vertex buffer views
    ccstd::vector<D3D12_VERTEX_BUFFER_VIEW> vbViews;
    D3D12_INDEX_BUFFER_VIEW ibView{};
    bool hasIndexBuffer{false};
    uint32_t indexFormat{0}; // DXGI_FORMAT as uint32_t
#endif
};

CCD3D12InputAssembler::CCD3D12InputAssembler()
: _impl(std::make_unique<Impl>()) {
}

CCD3D12InputAssembler::~CCD3D12InputAssembler() = default;

void CCD3D12InputAssembler::doInit(const InputAssemblerInfo &info) {
#if defined(_WIN32)
    auto *device = CCD3D12Device::getInstance();
    if (!device) {
        CC_LOG_ERROR("D3D12InputAssembler: device not available.");
        return;
    }

    // Build D3D12_INPUT_ELEMENT_DESC array
    _impl->inputElements.resize(info.attributes.size());
    for (size_t i = 0; i < info.attributes.size(); ++i) {
        const auto &attr = info.attributes[i];
        auto &element = _impl->inputElements[i];

        char semanticName[64] = {};
        uint32_t semanticIndex = 0;
        extractSemantic(attr.name, semanticName, semanticIndex);

        // We need to store the semantic name string persistently
        // D3D12_INPUT_ELEMENT_DESC.SemanticName is a const char* that must remain valid
        // We store them in a separate vector - but since inputElements may reallocate,
        // we use a stable storage approach: allocate strings on heap
        // Actually, let's use a different approach - store strings in the Impl struct
        // For simplicity, we'll use static strings for common semantics and heap for others

        element.SemanticName = nullptr; // will be set below
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

        // Allocate persistent semantic name string
        // Use a simple approach: store in a vector<string> in Impl
        // We'll add a semanticNames vector
        (void)semanticName; // Will fix below
    }

    // Store semantic names persistently
    // We need to add semanticNames vector to Impl... Let me restructure.
    // Actually we need to do this differently. Let me store the names separately.

    // Store semantic names persistently in Impl (D3D12_INPUT_ELEMENT_DESC.SemanticName must remain valid)
    _impl->semanticNames.resize(info.attributes.size());
    for (size_t i = 0; i < info.attributes.size(); ++i) {
        char semanticName[64] = {};
        uint32_t semanticIndex = 0;
        extractSemantic(info.attributes[i].name, semanticName, semanticIndex);
        _impl->semanticNames[i] = semanticName;
        _impl->inputElements[i].SemanticName = _impl->semanticNames[i].c_str();
        _impl->inputElements[i].SemanticIndex = semanticIndex;
    }

    // Build vertex buffer views
    _impl->vbViews.resize(info.vertexBuffers.size());
    for (size_t i = 0; i < info.vertexBuffers.size(); ++i) {
        auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(info.vertexBuffers[i]);
        if (d3d12Buffer) {
            // getD3D12GPUVirtualAddress() already includes resourceOffset internally,
            // so we must NOT add getD3D12ResourceOffset() again.
            _impl->vbViews[i].BufferLocation = d3d12Buffer->getD3D12GPUVirtualAddress();
            _impl->vbViews[i].SizeInBytes = d3d12Buffer->getSize();
            _impl->vbViews[i].StrideInBytes = d3d12Buffer->getStride();
        }
    }

    // Build index buffer view
    _impl->hasIndexBuffer = (info.indexBuffer != nullptr);
    if (_impl->hasIndexBuffer) {
        auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(info.indexBuffer);
        if (d3d12Buffer) {
            // getD3D12GPUVirtualAddress() already includes resourceOffset internally.
            _impl->ibView.BufferLocation = d3d12Buffer->getD3D12GPUVirtualAddress();
            _impl->ibView.SizeInBytes = d3d12Buffer->getSize();

            // Determine index format from buffer stride
            const uint32_t stride = d3d12Buffer->getStride();
            if (stride == 4) {
                _impl->ibView.Format = DXGI_FORMAT_R32_UINT;
                _impl->indexFormat = static_cast<uint32_t>(DXGI_FORMAT_R32_UINT);
            } else {
                _impl->ibView.Format = DXGI_FORMAT_R16_UINT;
                _impl->indexFormat = static_cast<uint32_t>(DXGI_FORMAT_R16_UINT);
            }
        }
    }

    CC_LOG_INFO("D3D12InputAssembler initialized with %u attributes, %u vertex buffers.",
                static_cast<unsigned>(info.attributes.size()),
                static_cast<unsigned>(info.vertexBuffers.size()));
#else
    (void)info;
#endif
}

void CCD3D12InputAssembler::doDestroy() {
#if defined(_WIN32)
    _impl->inputElements.clear();
    _impl->semanticNames.clear();
    _impl->vbViews.clear();
    _impl->hasIndexBuffer = false;
#endif
}

void *CCD3D12InputAssembler::getInputElementDescs() const {
#if defined(_WIN32)
    return _impl ? _impl->inputElements.data() : nullptr;
#else
    return nullptr;
#endif
}

uint32_t CCD3D12InputAssembler::getInputElementDescCount() const {
#if defined(_WIN32)
    return _impl ? static_cast<uint32_t>(_impl->inputElements.size()) : 0;
#else
    return 0;
#endif
}

uint32_t CCD3D12InputAssembler::getVertexBufferCount() const {
#if defined(_WIN32)
    return _impl ? static_cast<uint32_t>(_impl->vbViews.size()) : 0;
#else
    return 0;
#endif
}

void CCD3D12InputAssembler::fillVertexBufferViews(void *views) const {
#if defined(_WIN32)
    if (!_impl || _impl->vbViews.empty()) return;
    auto *dst = static_cast<D3D12_VERTEX_BUFFER_VIEW *>(views);
    for (size_t i = 0; i < _impl->vbViews.size(); ++i) {
        dst[i] = _impl->vbViews[i];
    }
#else
    (void)views;
#endif
}

bool CCD3D12InputAssembler::hasIndexBuffer() const {
#if defined(_WIN32)
    return _impl ? _impl->hasIndexBuffer : false;
#else
    return false;
#endif
}

void CCD3D12InputAssembler::fillIndexBufferView(void *view) const {
#if defined(_WIN32)
    if (!_impl || !_impl->hasIndexBuffer) return;
    *static_cast<D3D12_INDEX_BUFFER_VIEW *>(view) = _impl->ibView;
#else
    (void)view;
#endif
}

uint32_t CCD3D12InputAssembler::getIndexFormat() const {
#if defined(_WIN32)
    return _impl ? _impl->indexFormat : 0;
#else
    return 0;
#endif
}

} // namespace gfx
} // namespace cc
