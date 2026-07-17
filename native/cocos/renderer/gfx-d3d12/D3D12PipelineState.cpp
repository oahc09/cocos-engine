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

#include "D3D12PipelineState.h"
#include "D3D12Device.h"
#include "D3D12Shader.h"
#include "D3D12RenderPass.h"
#include "D3D12Texture.h"
#include "D3D12PipelineLayout.h"
#include "D3D12DescriptorSetLayout.h"
#include "base/Log.h"
#include "gfx-base/GFXDef.h"
#include "platform/FileUtils.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <chrono>
    #include <cstdio>
    #include <d3d12.h>
    #include <cctype>
    #include <cstring>
    #include <cstdlib>
    #include <dxgiformat.h>
    #include <d3dcompiler.h>
    #include <map>
    #include <tuple>
    #include <vector>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

namespace {
using D3D12PerfClock = std::chrono::steady_clock;

constexpr uint32_t CC_D3D12_PSO_CACHE_VERSION = 1;
constexpr uint64_t FNV1A64_OFFSET = 14695981039346656037ULL;
constexpr uint64_t FNV1A64_PRIME = 1099511628211ULL;

uint64_t elapsedMs(D3D12PerfClock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(D3D12PerfClock::now() - start).count());
}

void appendHashBytes(uint64_t &hash, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= FNV1A64_PRIME;
    }
}

template <class T>
void appendHashValue(uint64_t &hash, const T &value) {
    appendHashBytes(hash, &value, sizeof(T));
}

void appendHashString(uint64_t &hash, const char *value) {
    if (!value) {
        const uint8_t terminator = 0;
        appendHashBytes(hash, &terminator, 1);
        return;
    }
    appendHashBytes(hash, value, std::strlen(value));
    const uint8_t terminator = 0;
    appendHashBytes(hash, &terminator, 1);
}

ccstd::string toHex(uint64_t value) {
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

ccstd::string joinCachePath(const ccstd::string &base, const ccstd::string &child) {
    if (base.empty()) {
        return child;
    }
    const char last = base[base.size() - 1];
    if (last == '/' || last == '\\') {
        return base + child;
    }
    return base + "/" + child;
}

ccstd::string getPSOCacheDirectory(cc::FileUtils *fileUtils) {
    if (!fileUtils) {
        return "";
    }
    ccstd::string root = fileUtils->getWritablePath();
    if (root.empty()) {
        return "";
    }
    root = joinCachePath(root, "d3d12-pso-cache");
    return joinCachePath(root, "v" + std::to_string(CC_D3D12_PSO_CACHE_VERSION));
}

void appendShaderBytecodeHash(uint64_t &hash, const D3D12_SHADER_BYTECODE &bytecode) {
    appendHashValue(hash, bytecode.BytecodeLength);
    if (bytecode.pShaderBytecode && bytecode.BytecodeLength > 0) {
        appendHashBytes(hash, bytecode.pShaderBytecode, bytecode.BytecodeLength);
    }
}

void appendRenderTargetBlendHash(uint64_t &hash, const D3D12_RENDER_TARGET_BLEND_DESC &desc) {
    appendHashValue(hash, desc.BlendEnable);
    appendHashValue(hash, desc.LogicOpEnable);
    appendHashValue(hash, desc.SrcBlend);
    appendHashValue(hash, desc.DestBlend);
    appendHashValue(hash, desc.BlendOp);
    appendHashValue(hash, desc.SrcBlendAlpha);
    appendHashValue(hash, desc.DestBlendAlpha);
    appendHashValue(hash, desc.BlendOpAlpha);
    appendHashValue(hash, desc.LogicOp);
    appendHashValue(hash, desc.RenderTargetWriteMask);
}

void appendDepthStencilOpHash(uint64_t &hash, const D3D12_DEPTH_STENCILOP_DESC &desc) {
    appendHashValue(hash, desc.StencilFailOp);
    appendHashValue(hash, desc.StencilDepthFailOp);
    appendHashValue(hash, desc.StencilPassOp);
    appendHashValue(hash, desc.StencilFunc);
}

ccstd::string makeGraphicsPSOCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc,
                                      uint64_t rootSignatureHash) {
    uint64_t hashA = FNV1A64_OFFSET;
    uint64_t hashB = FNV1A64_OFFSET ^ 0x9e3779b97f4a7c15ULL;
    appendHashValue(hashA, CC_D3D12_PSO_CACHE_VERSION);
    appendHashValue(hashA, rootSignatureHash);

    appendShaderBytecodeHash(hashA, desc.VS);
    appendShaderBytecodeHash(hashA, desc.PS);
    appendShaderBytecodeHash(hashA, desc.DS);
    appendShaderBytecodeHash(hashA, desc.HS);
    appendShaderBytecodeHash(hashA, desc.GS);

    appendHashValue(hashA, desc.BlendState.AlphaToCoverageEnable);
    appendHashValue(hashA, desc.BlendState.IndependentBlendEnable);
    for (const auto &rt : desc.BlendState.RenderTarget) {
        appendRenderTargetBlendHash(hashA, rt);
    }

    appendHashValue(hashA, desc.SampleMask);

    appendHashValue(hashA, desc.RasterizerState.FillMode);
    appendHashValue(hashA, desc.RasterizerState.CullMode);
    appendHashValue(hashA, desc.RasterizerState.FrontCounterClockwise);
    appendHashValue(hashA, desc.RasterizerState.DepthBias);
    appendHashValue(hashA, desc.RasterizerState.DepthBiasClamp);
    appendHashValue(hashA, desc.RasterizerState.SlopeScaledDepthBias);
    appendHashValue(hashA, desc.RasterizerState.DepthClipEnable);
    appendHashValue(hashA, desc.RasterizerState.MultisampleEnable);
    appendHashValue(hashA, desc.RasterizerState.AntialiasedLineEnable);
    appendHashValue(hashA, desc.RasterizerState.ForcedSampleCount);
    appendHashValue(hashA, desc.RasterizerState.ConservativeRaster);

    appendHashValue(hashA, desc.DepthStencilState.DepthEnable);
    appendHashValue(hashA, desc.DepthStencilState.DepthWriteMask);
    appendHashValue(hashA, desc.DepthStencilState.DepthFunc);
    appendHashValue(hashA, desc.DepthStencilState.StencilEnable);
    appendHashValue(hashA, desc.DepthStencilState.StencilReadMask);
    appendHashValue(hashA, desc.DepthStencilState.StencilWriteMask);
    appendDepthStencilOpHash(hashA, desc.DepthStencilState.FrontFace);
    appendDepthStencilOpHash(hashA, desc.DepthStencilState.BackFace);

    appendHashValue(hashA, desc.InputLayout.NumElements);
    for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
        const auto &elem = desc.InputLayout.pInputElementDescs[i];
        appendHashString(hashA, elem.SemanticName);
        appendHashValue(hashA, elem.SemanticIndex);
        appendHashValue(hashA, elem.Format);
        appendHashValue(hashA, elem.InputSlot);
        appendHashValue(hashA, elem.AlignedByteOffset);
        appendHashValue(hashA, elem.InputSlotClass);
        appendHashValue(hashA, elem.InstanceDataStepRate);
    }

    appendHashValue(hashA, desc.PrimitiveTopologyType);
    appendHashValue(hashA, desc.NumRenderTargets);
    for (auto format : desc.RTVFormats) {
        appendHashValue(hashA, format);
    }
    appendHashValue(hashA, desc.DSVFormat);
    appendHashValue(hashA, desc.SampleDesc.Count);
    appendHashValue(hashA, desc.SampleDesc.Quality);
    appendHashValue(hashA, desc.NodeMask);
    appendHashValue(hashA, desc.Flags);

    appendHashValue(hashB, desc.Flags);
    appendHashValue(hashB, desc.NodeMask);
    appendHashValue(hashB, desc.SampleDesc.Quality);
    appendHashValue(hashB, desc.SampleDesc.Count);
    appendHashValue(hashB, desc.DSVFormat);
    for (auto format : desc.RTVFormats) {
        appendHashValue(hashB, format);
    }
    appendHashValue(hashB, desc.NumRenderTargets);
    appendHashValue(hashB, desc.PrimitiveTopologyType);
    appendShaderBytecodeHash(hashB, desc.GS);
    appendShaderBytecodeHash(hashB, desc.HS);
    appendShaderBytecodeHash(hashB, desc.DS);
    appendShaderBytecodeHash(hashB, desc.PS);
    appendShaderBytecodeHash(hashB, desc.VS);
    appendHashValue(hashB, CC_D3D12_PSO_CACHE_VERSION);
    appendHashValue(hashB, rootSignatureHash);

    return toHex(hashA) + toHex(hashB);
}

bool loadFileCachedPSO(const ccstd::string &cacheKey, std::vector<uint8_t> &outBlob) {
    auto *fileUtils = cc::FileUtils::getInstance();
    const ccstd::string cacheDir = getPSOCacheDirectory(fileUtils);
    if (cacheDir.empty()) {
        return false;
    }

    const ccstd::string cachePath = joinCachePath(cacheDir, cacheKey + ".pso");
    cc::Data cachedData = fileUtils->getDataFromFile(cachePath);
    if (cachedData.isNull() || cachedData.getSize() == 0) {
        return false;
    }

    const uint8_t *bytes = cachedData.getBytes();
    outBlob.assign(bytes, bytes + cachedData.getSize());
    return !outBlob.empty();
}

bool storeFileCachedPSO(const ccstd::string &cacheKey, ID3D12PipelineState *pipelineState) {
    if (!pipelineState) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> cachedBlob;
    if (FAILED(pipelineState->GetCachedBlob(&cachedBlob)) || !cachedBlob || cachedBlob->GetBufferSize() == 0) {
        return false;
    }

    auto *fileUtils = cc::FileUtils::getInstance();
    const ccstd::string cacheDir = getPSOCacheDirectory(fileUtils);
    if (cacheDir.empty()) {
        return false;
    }
    if (!fileUtils->createDirectory(cacheDir)) {
        CC_LOG_WARNING("D3D12PipelineState: failed to create PSO cache directory '%s'.", cacheDir.c_str());
        return false;
    }

    const ccstd::string cachePath = joinCachePath(cacheDir, cacheKey + ".pso");
    cc::Data data;
    data.copy(static_cast<const uint8_t *>(cachedBlob->GetBufferPointer()),
              static_cast<uint32_t>(cachedBlob->GetBufferSize()));
    if (!fileUtils->writeDataToFile(data, cachePath)) {
        CC_LOG_WARNING("D3D12PipelineState: failed to write PSO cache entry '%s'.", cachePath.c_str());
        return false;
    }
    return true;
}

ID3D12RootSignature *getOrCreateEmptyRootSignature(ID3D12Device *device) {
    static Microsoft::WRL::ComPtr<ID3D12RootSignature> s_emptyRootSig;
    if (s_emptyRootSig || !device) {
        return s_emptyRootSig.Get();
    }

    D3D12_ROOT_SIGNATURE_DESC emptyRootSigDesc{};
    emptyRootSigDesc.NumParameters = 0;
    emptyRootSigDesc.pParameters = nullptr;
    emptyRootSigDesc.NumStaticSamplers = 0;
    emptyRootSigDesc.pStaticSamplers = nullptr;
    emptyRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
    HRESULT serHR = D3D12SerializeRootSignature(&emptyRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                &sigBlob, &errBlob);
    if (FAILED(serHR)) {
        CC_LOG_WARNING("D3D12PipelineState: failed to serialize empty root signature. HRESULT=0x%08x",
                       static_cast<unsigned>(serHR));
        return nullptr;
    }

    HRESULT createHR = device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                                   sigBlob->GetBufferSize(),
                                                   IID_PPV_ARGS(&s_emptyRootSig));
    if (FAILED(createHR)) {
        CC_LOG_WARNING("D3D12PipelineState: failed to create empty root signature. HRESULT=0x%08x",
                       static_cast<unsigned>(createHR));
        return nullptr;
    }

    CC_LOG_INFO("D3D12PipelineState: created empty root signature.");
    return s_emptyRootSig.Get();
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE toD3D12PrimitiveTopologyType(PrimitiveMode mode) {
    switch (mode) {
        case PrimitiveMode::POINT_LIST:                  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case PrimitiveMode::LINE_LIST:                   return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case PrimitiveMode::LINE_STRIP:                  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case PrimitiveMode::LINE_LOOP:                   return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; // approx
        case PrimitiveMode::LINE_LIST_ADJACENCY:         return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case PrimitiveMode::LINE_STRIP_ADJACENCY:        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case PrimitiveMode::ISO_LINE_LIST:               return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; // approx
        case PrimitiveMode::TRIANGLE_LIST:               return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case PrimitiveMode::TRIANGLE_STRIP:              return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case PrimitiveMode::TRIANGLE_FAN:                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case PrimitiveMode::TRIANGLE_LIST_ADJACENCY:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case PrimitiveMode::TRIANGLE_STRIP_ADJACENCY:    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case PrimitiveMode::TRIANGLE_PATCH_ADJACENCY:    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        case PrimitiveMode::QUAD_PATCH_LIST:             return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        default:                                         return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

D3D12_PRIMITIVE_TOPOLOGY toD3D12PrimitiveTopology(PrimitiveMode mode) {
    switch (mode) {
        case PrimitiveMode::POINT_LIST:                  return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case PrimitiveMode::LINE_LIST:                   return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case PrimitiveMode::LINE_STRIP:                  return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case PrimitiveMode::LINE_LOOP:                   return D3D_PRIMITIVE_TOPOLOGY_LINELIST; // approx
        case PrimitiveMode::TRIANGLE_LIST:               return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case PrimitiveMode::TRIANGLE_STRIP:              return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case PrimitiveMode::TRIANGLE_FAN:                return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; // approx
        case PrimitiveMode::LINE_LIST_ADJACENCY:         return D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        case PrimitiveMode::LINE_STRIP_ADJACENCY:        return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
        case PrimitiveMode::TRIANGLE_LIST_ADJACENCY:     return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
        case PrimitiveMode::TRIANGLE_STRIP_ADJACENCY:    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
        case PrimitiveMode::TRIANGLE_PATCH_ADJACENCY:    return D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST;
        case PrimitiveMode::QUAD_PATCH_LIST:             return D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
        case PrimitiveMode::ISO_LINE_LIST:               return D3D_PRIMITIVE_TOPOLOGY_LINELIST; // approx
        default:                                         return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

D3D12_BLEND toD3D12Blend(BlendFactor factor) {
    switch (factor) {
        case BlendFactor::ZERO:                    return D3D12_BLEND_ZERO;
        case BlendFactor::ONE:                     return D3D12_BLEND_ONE;
        case BlendFactor::SRC_ALPHA:               return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::DST_ALPHA:               return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::ONE_MINUS_SRC_ALPHA:     return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::ONE_MINUS_DST_ALPHA:     return D3D12_BLEND_INV_DEST_ALPHA;
        case BlendFactor::SRC_COLOR:               return D3D12_BLEND_SRC_COLOR;
        case BlendFactor::DST_COLOR:               return D3D12_BLEND_DEST_COLOR;
        case BlendFactor::ONE_MINUS_SRC_COLOR:     return D3D12_BLEND_INV_SRC_COLOR;
        case BlendFactor::ONE_MINUS_DST_COLOR:     return D3D12_BLEND_INV_DEST_COLOR;
        case BlendFactor::SRC_ALPHA_SATURATE:      return D3D12_BLEND_SRC_ALPHA_SAT;
        case BlendFactor::CONSTANT_COLOR:          return D3D12_BLEND_BLEND_FACTOR;
        case BlendFactor::ONE_MINUS_CONSTANT_COLOR:return D3D12_BLEND_INV_BLEND_FACTOR;
        case BlendFactor::CONSTANT_ALPHA:          return D3D12_BLEND_BLEND_FACTOR;
        case BlendFactor::ONE_MINUS_CONSTANT_ALPHA:return D3D12_BLEND_INV_BLEND_FACTOR;
        default:                                   return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP toD3D12BlendOp(BlendOp op) {
    switch (op) {
        case BlendOp::ADD:     return D3D12_BLEND_OP_ADD;
        case BlendOp::SUB:     return D3D12_BLEND_OP_SUBTRACT;
        case BlendOp::REV_SUB: return D3D12_BLEND_OP_REV_SUBTRACT;
        case BlendOp::MIN:     return D3D12_BLEND_OP_MIN;
        case BlendOp::MAX:     return D3D12_BLEND_OP_MAX;
        default:               return D3D12_BLEND_OP_ADD;
    }
}

D3D12_COMPARISON_FUNC toD3D12ComparisonFunc(ComparisonFunc func) {
    switch (func) {
        case ComparisonFunc::NEVER:        return D3D12_COMPARISON_FUNC_NEVER;
        case ComparisonFunc::LESS:         return D3D12_COMPARISON_FUNC_LESS;
        case ComparisonFunc::EQUAL:        return D3D12_COMPARISON_FUNC_EQUAL;
        case ComparisonFunc::LESS_EQUAL:   return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case ComparisonFunc::GREATER:      return D3D12_COMPARISON_FUNC_GREATER;
        case ComparisonFunc::NOT_EQUAL:    return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case ComparisonFunc::GREATER_EQUAL:return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case ComparisonFunc::ALWAYS:       return D3D12_COMPARISON_FUNC_ALWAYS;
        default:                           return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

D3D12_FILL_MODE toD3D12FillMode(PolygonMode mode) {
    switch (mode) {
        case PolygonMode::FILL:  return D3D12_FILL_MODE_SOLID;
        case PolygonMode::POINT: return D3D12_FILL_MODE_WIREFRAME; // D3D12 has no point mode
        case PolygonMode::LINE:  return D3D12_FILL_MODE_WIREFRAME;
        default:                 return D3D12_FILL_MODE_SOLID;
    }
}

D3D12_CULL_MODE toD3D12CullMode(CullMode mode) {
    switch (mode) {
        case CullMode::NONE:  return D3D12_CULL_MODE_NONE;
        case CullMode::FRONT: return D3D12_CULL_MODE_FRONT;
        case CullMode::BACK:  return D3D12_CULL_MODE_BACK;
        default:              return D3D12_CULL_MODE_NONE;
    }
}

D3D12_STENCIL_OP toD3D12StencilOp(StencilOp op) {
    switch (op) {
        case StencilOp::ZERO:       return D3D12_STENCIL_OP_ZERO;
        case StencilOp::KEEP:       return D3D12_STENCIL_OP_KEEP;
        case StencilOp::REPLACE:    return D3D12_STENCIL_OP_REPLACE;
        case StencilOp::INCR:       return D3D12_STENCIL_OP_INCR;
        case StencilOp::DECR:       return D3D12_STENCIL_OP_DECR;
        case StencilOp::INVERT:     return D3D12_STENCIL_OP_INVERT;
        case StencilOp::INCR_WRAP:  return D3D12_STENCIL_OP_INCR_SAT;
        case StencilOp::DECR_WRAP:  return D3D12_STENCIL_OP_DECR_SAT;
        default:                    return D3D12_STENCIL_OP_KEEP;
    }
}

UINT8 toD3D12ColorWriteMask(ColorMask mask) {
    UINT8 result = 0;
    if (hasFlag(mask, ColorMask::R)) result |= D3D12_COLOR_WRITE_ENABLE_RED;
    if (hasFlag(mask, ColorMask::G)) result |= D3D12_COLOR_WRITE_ENABLE_GREEN;
    if (hasFlag(mask, ColorMask::B)) result |= D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (hasFlag(mask, ColorMask::A)) result |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
    return result;
}

D3D12_RENDER_TARGET_BLEND_DESC makeDefaultRenderTargetBlendDesc() {
    D3D12_RENDER_TARGET_BLEND_DESC desc{};
    desc.BlendEnable = FALSE;
    desc.LogicOpEnable = FALSE;
    desc.SrcBlend = D3D12_BLEND_ONE;
    desc.DestBlend = D3D12_BLEND_ZERO;
    desc.BlendOp = D3D12_BLEND_OP_ADD;
    desc.SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return desc;
}

DXGI_FORMAT toD3D12VertexFormat(Format fmt) {
    switch (fmt) {
        case Format::RGB32F:    return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::RG32F:     return DXGI_FORMAT_R32G32_FLOAT;
        case Format::R32F:      return DXGI_FORMAT_R32_FLOAT;
        case Format::RGBA32F:   return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::RGBA8:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8SN:   return DXGI_FORMAT_R8G8B8A8_SNORM;
        case Format::RG16F:     return DXGI_FORMAT_R16G16_FLOAT;
        case Format::RGBA16F:   return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::RG32I:     return DXGI_FORMAT_R32G32_SINT;
        case Format::R32I:      return DXGI_FORMAT_R32_SINT;
        case Format::R32UI:     return DXGI_FORMAT_R32_UINT;
        case Format::RG8:       return DXGI_FORMAT_R8G8_UNORM;
        case Format::R16F:      return DXGI_FORMAT_R16_FLOAT;
        case Format::RGBA16UI:  return DXGI_FORMAT_R16G16B16A16_UINT;
        case Format::RGB8:      return DXGI_FORMAT_R8G8B8A8_UNORM; // D3D12 has no 24-bit vertex format
        case Format::BGRA8:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::RGB10A2:   return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:                return DXGI_FORMAT_R32G32B32_FLOAT; // safe default
    }
}

struct AttributeSemantic {
    const char *name{"TEXCOORD"};
    uint32_t index{0};
};

uint32_t trailingNumber(const ccstd::string &value) {
    const char *begin = value.c_str();
    const char *end = begin + value.size();
    const char *digitStart = end;
    while (digitStart > begin && std::isdigit(static_cast<unsigned char>(*(digitStart - 1)))) {
        --digitStart;
    }
    return digitStart < end ? static_cast<uint32_t>(std::atoi(digitStart)) : 0;
}

AttributeSemantic getAttributeSemantic(const ccstd::string &attributeName, uint32_t fallbackLocation) {
    if (attributeName == "a_position" || attributeName == "POSITION") {
        return {"POSITION", 0};
    }
    if (attributeName == "a_normal" || attributeName == "NORMAL") {
        return {"NORMAL", 0};
    }
    if (attributeName == "a_tangent" || attributeName == "TANGENT") {
        return {"TANGENT", 0};
    }
    if (attributeName == "a_bitangent" || attributeName == "BITANGENT") {
        return {"BITANGENT", 0};
    }
    if (attributeName == "a_color" || attributeName == "COLOR") {
        return {"COLOR", 0};
    }
    if (attributeName.find("color") != ccstd::string::npos || attributeName.find("COLOR") != ccstd::string::npos) {
        return {"COLOR", trailingNumber(attributeName)};
    }
    if (attributeName == "a_weights" || attributeName == "BLENDWEIGHT") {
        return {"BLENDWEIGHT", 0};
    }
    if (attributeName == "a_joints" || attributeName == "BLENDINDICES") {
        return {"BLENDINDICES", 0};
    }
    if (attributeName.find("texCoord") != ccstd::string::npos ||
        attributeName.find("texcoord") != ccstd::string::npos ||
        attributeName.find("TEXCOORD") != ccstd::string::npos ||
        attributeName.find("uv") != ccstd::string::npos) {
        return {"TEXCOORD", trailingNumber(attributeName)};
    }
    return {"TEXCOORD", fallbackLocation};
}

const Attribute *findShaderAttributeForReflectedInput(const CCD3D12Shader::VertexInputSignature &input,
                                                      const AttributeList &shaderAttributes) {
    // SPIRV-Cross maps GLSL vertex input layout(location = N) to TEXCOORDN in
    // HLSL. Use the reflected semantic index to recover the engine attribute
    // name from the shader template, then match that name to the IA stream.
    if (_stricmp(input.semanticName.c_str(), "TEXCOORD") == 0) {
        for (const auto &attr : shaderAttributes) {
            if (attr.location == input.semanticIndex) {
                return &attr;
            }
        }
    }
    for (const auto &attr : shaderAttributes) {
        const auto semantic = getAttributeSemantic(attr.name, attr.location);
        if (_stricmp(input.semanticName.c_str(), semantic.name) == 0 &&
            input.semanticIndex == semantic.index) {
            return &attr;
        }
    }
    return nullptr;
}

} // namespace

struct CCD3D12PipelineState::Impl {
    using DynamicPipelineKey = std::tuple<INT, uint32_t, uint32_t, UINT8, UINT8>;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
    ID3D12RootSignature *rootSignature{nullptr};
    D3D12_PRIMITIVE_TOPOLOGY primitiveTopology{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST};
    bool usesPipelineLayoutRootSignature{false};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC baseDesc{};
    ccstd::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    std::map<DynamicPipelineKey, Microsoft::WRL::ComPtr<ID3D12PipelineState>> dynamicPipelineStates;
    // Per-PSO persistent storage for InputLayout semantic names.
    // Must outlive the PSO because D3D12_INPUT_ELEMENT_DESC::SemanticName is a raw pointer.
    ccstd::vector<ccstd::string> semanticNames;
};

CCD3D12PipelineState::CCD3D12PipelineState() {
    _impl = std::make_unique<Impl>();
}

CCD3D12PipelineState::~CCD3D12PipelineState() {
    destroy();
}

void CCD3D12PipelineState::doInit(const PipelineStateInfo &info) {
    (void)info;
    const auto initStart = D3D12PerfClock::now();
    if (!_impl) return;

    _impl->pipelineState.Reset();
    _impl->rootSignature = nullptr;
    _impl->usesPipelineLayoutRootSignature = false;
    _impl->baseDesc = {};
    _impl->inputElements.clear();
    _impl->dynamicPipelineStates.clear();

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12PipelineState: device unavailable.");
        return;
    }

    // Store primitive topology for IA setup
    _impl->primitiveTopology = toD3D12PrimitiveTopology(_primitive);

    // Get shader bytecode
    auto *d3d12Shader = static_cast<CCD3D12Shader *>(_shader);
    if (!d3d12Shader) {
        CC_LOG_ERROR("D3D12PipelineState: shader is null.");
        return;
    }

    auto vsBlob = d3d12Shader->getVertexBytecode();
    auto psBlob = d3d12Shader->getFragmentBytecode();
    auto gsBlob = d3d12Shader->getGeometryBytecode();

    if (!vsBlob.data || vsBlob.size == 0) {
        CC_LOG_ERROR("D3D12PipelineState: vertex shader bytecode is empty.");
        return;
    }

    // Build D3D12_GRAPHICS_PIPELINE_STATE_DESC
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = nullptr;

    // Get root signature from PipelineLayout
    if (_pipelineLayout) {
        auto *d3d12Layout = static_cast<CCD3D12PipelineLayout *>(_pipelineLayout);
        psoDesc.pRootSignature = static_cast<ID3D12RootSignature *>(d3d12Layout->getID3D12RootSignature());
        _impl->usesPipelineLayoutRootSignature = (psoDesc.pRootSignature != nullptr);
    }

    // If no root signature from PipelineLayout, create an empty one
    if (!psoDesc.pRootSignature) {
        psoDesc.pRootSignature = getOrCreateEmptyRootSignature(d3dDevice);
        _impl->usesPipelineLayoutRootSignature = false;
    }

    // Shader stages
    psoDesc.VS.pShaderBytecode = vsBlob.data;
    psoDesc.VS.BytecodeLength = vsBlob.size;

    if (psBlob.data && psBlob.size > 0) {
        psoDesc.PS.pShaderBytecode = psBlob.data;
        psoDesc.PS.BytecodeLength = psBlob.size;
    }

    if (gsBlob.data && gsBlob.size > 0) {
        psoDesc.GS.pShaderBytecode = gsBlob.data;
        psoDesc.GS.BytecodeLength = gsBlob.size;
    }

    // Blend state
    const auto &blend = _blendState;
    psoDesc.BlendState.AlphaToCoverageEnable = blend.isA2C ? TRUE : FALSE;
    psoDesc.BlendState.IndependentBlendEnable = blend.isIndepend ? TRUE : FALSE;

    UINT numRenderTargets = 0;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        auto &rtBlend = psoDesc.BlendState.RenderTarget[i];
        rtBlend = makeDefaultRenderTargetBlendDesc();
        if (i < blend.targets.size()) {
            const auto &target = blend.targets[i];
            rtBlend.BlendEnable = target.blend ? TRUE : FALSE;
            rtBlend.LogicOpEnable = FALSE;
            rtBlend.SrcBlend = toD3D12Blend(target.blendSrc);
            rtBlend.DestBlend = toD3D12Blend(target.blendDst);
            rtBlend.BlendOp = toD3D12BlendOp(target.blendEq);
            rtBlend.SrcBlendAlpha = toD3D12Blend(target.blendSrcAlpha);
            rtBlend.DestBlendAlpha = toD3D12Blend(target.blendDstAlpha);
            rtBlend.BlendOpAlpha = toD3D12BlendOp(target.blendAlphaEq);
            rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
            rtBlend.RenderTargetWriteMask = toD3D12ColorWriteMask(target.blendColorMask);
        }
    }

    // Rasterizer state
    const auto &raster = _rasterizerState;
    psoDesc.RasterizerState.FillMode = toD3D12FillMode(raster.polygonMode);
    psoDesc.RasterizerState.CullMode = toD3D12CullMode(raster.cullMode);
    psoDesc.RasterizerState.FrontCounterClockwise = raster.isFrontFaceCCW ? TRUE : FALSE;
    psoDesc.RasterizerState.DepthBias = static_cast<INT>(raster.depthBias);
    psoDesc.RasterizerState.DepthBiasClamp = raster.depthBiasClamp;
    psoDesc.RasterizerState.SlopeScaledDepthBias = raster.depthBiasSlop;
    psoDesc.RasterizerState.DepthClipEnable = raster.isDepthClip ? TRUE : FALSE;
    psoDesc.RasterizerState.MultisampleEnable = raster.isMultisample ? TRUE : FALSE;
    psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    psoDesc.RasterizerState.ForcedSampleCount = 0;
    psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // Depth-stencil state
    const auto &ds = _depthStencilState;
    psoDesc.DepthStencilState.DepthEnable = ds.depthTest ? TRUE : FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = ds.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = toD3D12ComparisonFunc(ds.depthFunc);
    psoDesc.DepthStencilState.StencilEnable = (ds.stencilTestFront || ds.stencilTestBack) ? TRUE : FALSE;
    const bool useFrontStencilMask = ds.stencilTestFront || !ds.stencilTestBack;
    psoDesc.DepthStencilState.StencilReadMask = static_cast<UINT8>(
        useFrontStencilMask ? ds.stencilReadMaskFront : ds.stencilReadMaskBack);
    psoDesc.DepthStencilState.StencilWriteMask = static_cast<UINT8>(
        useFrontStencilMask ? ds.stencilWriteMaskFront : ds.stencilWriteMaskBack);
    if (ds.stencilTestFront && ds.stencilTestBack &&
        (ds.stencilReadMaskFront != ds.stencilReadMaskBack ||
         ds.stencilWriteMaskFront != ds.stencilWriteMaskBack)) {
        CC_LOG_WARNING("D3D12PipelineState: front/back stencil masks differ; D3D12 uses one shared mask.");
    }

    // Front face stencil
    psoDesc.DepthStencilState.FrontFace.StencilFailOp = toD3D12StencilOp(ds.stencilFailOpFront);
    psoDesc.DepthStencilState.FrontFace.StencilDepthFailOp = toD3D12StencilOp(ds.stencilZFailOpFront);
    psoDesc.DepthStencilState.FrontFace.StencilPassOp = toD3D12StencilOp(ds.stencilPassOpFront);
    psoDesc.DepthStencilState.FrontFace.StencilFunc = toD3D12ComparisonFunc(ds.stencilFuncFront);

    // Back face stencil
    psoDesc.DepthStencilState.BackFace.StencilFailOp = toD3D12StencilOp(ds.stencilFailOpBack);
    psoDesc.DepthStencilState.BackFace.StencilDepthFailOp = toD3D12StencilOp(ds.stencilZFailOpBack);
    psoDesc.DepthStencilState.BackFace.StencilPassOp = toD3D12StencilOp(ds.stencilPassOpBack);
    psoDesc.DepthStencilState.BackFace.StencilFunc = toD3D12ComparisonFunc(ds.stencilFuncBack);

    // Render pass formats
    if (_renderPass) {
        auto *d3d12RenderPass = static_cast<CCD3D12RenderPass *>(const_cast<RenderPass *>(_renderPass));
        const auto rtvFormats = d3d12RenderPass->getRTVFormats(_subpass);
        numRenderTargets = static_cast<UINT>(rtvFormats.size());
        psoDesc.NumRenderTargets = numRenderTargets;
        for (UINT i = 0; i < numRenderTargets && i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            psoDesc.RTVFormats[i] = static_cast<DXGI_FORMAT>(rtvFormats[i]);
        }
        psoDesc.DSVFormat = static_cast<DXGI_FORMAT>(d3d12RenderPass->getDSVFormat(_subpass));
    } else {
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // default
    }

    // Input layout. Reflect the compiled vertex shader so D3D12 follows the
    // actual DXBC input signature. Program templates can conservatively list
    // conditionally active attributes (for example particle render modes), but
    // D3D12 PSOs need the exact shader signature.
    auto &inputElements = _impl->inputElements;
    inputElements.clear();
    const AttributeList &shaderAttributes = _shader ? _shader->getAttributes() : _inputState.attributes;
    const auto &reflectedInputs = d3d12Shader->getVertexInputSignature();
    if (!reflectedInputs.empty()) {
        inputElements.reserve(reflectedInputs.size());
        _impl->semanticNames.clear();
        _impl->semanticNames.reserve(reflectedInputs.size());

        // D3D12 classifies vertex input by slot, not by attribute. Keep all
        // elements in the same slot on a single step mode; instanced streams
        // win when conditional attributes leave mixed metadata in the IA.
        bool slotIsInstanced[256] = {};
        for (const auto &shaderAttr : shaderAttributes) {
            for (const auto &attr : _inputState.attributes) {
                if (attr.name == shaderAttr.name) {
                    slotIsInstanced[attr.stream] = slotIsInstanced[attr.stream] || attr.isInstanced;
                    break;
                }
            }
        }

        for (const auto &reflectedInput : reflectedInputs) {
            const Attribute *shaderAttr = findShaderAttributeForReflectedInput(reflectedInput, shaderAttributes);
            D3D12_INPUT_ELEMENT_DESC elem{};
            _impl->semanticNames.push_back(reflectedInput.semanticName);
            elem.SemanticName = _impl->semanticNames.back().c_str();
            elem.SemanticIndex = reflectedInput.semanticIndex;

            bool attributeFound = false;
            uint32_t offsets[256] = {};
            if (shaderAttr) {
                for (const auto &attr : _inputState.attributes) {
                    if (attr.name == shaderAttr->name) {
                        elem.Format = toD3D12VertexFormat(attr.format);
                        elem.InputSlot = attr.stream;
                        elem.AlignedByteOffset = offsets[attr.stream];
                        elem.InputSlotClass = slotIsInstanced[attr.stream] ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                                            : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                        elem.InstanceDataStepRate = slotIsInstanced[attr.stream] ? 1 : 0;
                        attributeFound = true;
                        break;
                    }
                    offsets[attr.stream] += GFX_FORMAT_INFOS[static_cast<uint32_t>(attr.format)].size;
                }
            }

            if (!attributeFound) {
                // Keep PSO creation valid if a shader declares an attribute that
                // the IA does not provide. This mirrors the fallback used by the
                // Vulkan/WGPU backends: read dummy data from the beginning of
                // stream 0 instead of shifting all following attributes.
                elem.Format = shaderAttr ? toD3D12VertexFormat(shaderAttr->format) : DXGI_FORMAT_R32G32B32A32_FLOAT;
                elem.InputSlot = 0;
                elem.AlignedByteOffset = 0;
                elem.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                elem.InstanceDataStepRate = 0;
            }
            inputElements.push_back(elem);
        }

        psoDesc.InputLayout.pInputElementDescs = inputElements.data();
        psoDesc.InputLayout.NumElements = static_cast<UINT>(inputElements.size());
    } else if (!shaderAttributes.empty()) {
        inputElements.reserve(shaderAttributes.size());
        _impl->semanticNames.clear();
        _impl->semanticNames.reserve(shaderAttributes.size());

        bool slotIsInstanced[256] = {};
        for (const auto &shaderAttr : shaderAttributes) {
            for (const auto &attr : _inputState.attributes) {
                if (attr.name == shaderAttr.name) {
                    slotIsInstanced[attr.stream] = slotIsInstanced[attr.stream] || attr.isInstanced;
                    break;
                }
            }
        }

        for (const auto &shaderAttr : shaderAttributes) {
            D3D12_INPUT_ELEMENT_DESC elem{};
            _impl->semanticNames.push_back("TEXCOORD");
            elem.SemanticName = _impl->semanticNames.back().c_str();
            elem.SemanticIndex = shaderAttr.location;

            bool attributeFound = false;
            uint32_t offsets[256] = {};
            for (const auto &attr : _inputState.attributes) {
                if (attr.name == shaderAttr.name) {
                    elem.Format = toD3D12VertexFormat(attr.format);
                    elem.InputSlot = attr.stream;
                    elem.AlignedByteOffset = offsets[attr.stream];
                    elem.InputSlotClass = slotIsInstanced[attr.stream] ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                                        : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                    elem.InstanceDataStepRate = slotIsInstanced[attr.stream] ? 1 : 0;
                    attributeFound = true;
                    break;
                }
                offsets[attr.stream] += GFX_FORMAT_INFOS[static_cast<uint32_t>(attr.format)].size;
            }

            if (!attributeFound) {
                elem.Format = toD3D12VertexFormat(shaderAttr.format);
                elem.InputSlot = 0;
                elem.AlignedByteOffset = 0;
                elem.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                elem.InstanceDataStepRate = 0;
            }
            inputElements.push_back(elem);
        }

        psoDesc.InputLayout.pInputElementDescs = inputElements.data();
        psoDesc.InputLayout.NumElements = static_cast<UINT>(inputElements.size());
    } else {
        psoDesc.InputLayout.pInputElementDescs = nullptr;
        psoDesc.InputLayout.NumElements = 0;
    }

    // Primitive topology
    psoDesc.PrimitiveTopologyType = toD3D12PrimitiveTopologyType(_primitive);

    // Sample description — derive from RenderPass if available
    if (_renderPass) {
        auto *d3d12RenderPass = static_cast<CCD3D12RenderPass *>(const_cast<RenderPass *>(_renderPass));
        const auto requestedSampleCount = static_cast<SampleCount>(d3d12RenderPass->getSampleCount(_subpass));
        psoDesc.SampleDesc.Count = static_cast<UINT>(getD3D12EffectiveSampleCount(requestedSampleCount));
        psoDesc.SampleDesc.Quality = 0;
    } else {
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleDesc.Quality = 0;
    }
    if (psoDesc.SampleDesc.Count > 1) {
        psoDesc.RasterizerState.MultisampleEnable = TRUE;
    }
    psoDesc.SampleMask = 0xFFFFFFFF;

    // Node mask
    psoDesc.NodeMask = 0;
    psoDesc.CachedPSO = {};
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    if (!psoDesc.pRootSignature) {
        CC_LOG_WARNING("D3D12PipelineState: no root signature available. Skipping PSO creation.");
        return;
    }

    uint64_t rootSignatureHash = 0;
    if (_pipelineLayout) {
        auto *d3d12Layout = static_cast<CCD3D12PipelineLayout *>(_pipelineLayout);
        rootSignatureHash = d3d12Layout->getRootSignatureHash();
    }
    const auto psoKeyStart = D3D12PerfClock::now();
    const ccstd::string psoCacheKey = makeGraphicsPSOCacheKey(psoDesc, rootSignatureHash);
    const uint64_t psoKeyMs = elapsedMs(psoKeyStart);
    std::vector<uint8_t> cachedPsoBlob;
    const auto psoCacheReadStart = D3D12PerfClock::now();
    const bool psoCacheHit = loadFileCachedPSO(psoCacheKey, cachedPsoBlob);
    const uint64_t psoCacheReadMs = elapsedMs(psoCacheReadStart);
    if (psoCacheHit) {
        psoDesc.CachedPSO.pCachedBlob = cachedPsoBlob.data();
        psoDesc.CachedPSO.CachedBlobSizeInBytes = cachedPsoBlob.size();
    }

    const auto createPsoStart = D3D12PerfClock::now();
    HRESULT hr = d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_impl->pipelineState));
    const HRESULT initialCreateHR = hr;
    const uint64_t initialCreatePsoMs = elapsedMs(createPsoStart);
    bool psoCacheRejected = false;
    uint64_t fallbackCreatePsoMs = 0;
    if (FAILED(hr) && psoCacheHit) {
        psoCacheRejected = true;
        CC_LOG_WARNING("D3D12PipelineState: cached PSO rejected, retrying without cache. HRESULT=0x%08x key=%s",
                       static_cast<unsigned>(hr), psoCacheKey.c_str());
        psoDesc.CachedPSO = {};
        const auto fallbackCreatePsoStart = D3D12PerfClock::now();
        hr = d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_impl->pipelineState));
        fallbackCreatePsoMs = elapsedMs(fallbackCreatePsoStart);
    }
    const auto createPsoMs = elapsedMs(createPsoStart);
    CC_LOG_INFO("[D3D12-PERF] PipelineStateCacheDecision shader='%s' key=%s rootSignature=%p keyMs=%llu cacheHit=%u cacheReadMs=%llu cacheBytes=%u initialHr=0x%08x initialCreateMs=%llu cacheRejected=%u fallbackCreateMs=%llu finalHr=0x%08x",
                _shader ? _shader->getName().c_str() : "", psoCacheKey.c_str(), psoDesc.pRootSignature,
                static_cast<unsigned long long>(psoKeyMs), psoCacheHit ? 1U : 0U,
                static_cast<unsigned long long>(psoCacheReadMs), static_cast<unsigned>(cachedPsoBlob.size()),
                static_cast<unsigned>(initialCreateHR), static_cast<unsigned long long>(initialCreatePsoMs),
                psoCacheRejected ? 1U : 0U, static_cast<unsigned long long>(fallbackCreatePsoMs),
                static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12PipelineState: CreateGraphicsPipelineState failed. HRESULT=0x%08x",
                     static_cast<unsigned>(hr));
    } else {
        _impl->rootSignature = psoDesc.pRootSignature;
        _impl->baseDesc = psoDesc;
        _impl->baseDesc.CachedPSO = {};
        const auto psoCacheStoreStart = D3D12PerfClock::now();
        const bool psoCacheStored = psoCacheHit ? false : storeFileCachedPSO(psoCacheKey, _impl->pipelineState.Get());
        const uint64_t psoCacheStoreMs = elapsedMs(psoCacheStoreStart);
        if (psoCacheHit) {
            CC_LOG_INFO("[D3D12-PERF] PipelineStateCacheHit key=%s bytes=%u",
                        psoCacheKey.c_str(), static_cast<unsigned>(cachedPsoBlob.size()));
        } else if (psoCacheStored) {
            CC_LOG_INFO("[D3D12-PERF] PipelineStateCacheStore key=%s", psoCacheKey.c_str());
        }
        CC_LOG_INFO("D3D12PipelineState created successfully.");
        CC_LOG_INFO("[D3D12-PERF] PipelineStateInit shader='%s' renderTargets=%u inputElements=%u sampleCount=%u psoCache=%s psoKeyMs=%llu cacheReadMs=%llu cacheStoreMs=%llu createGraphicsPsoMs=%llu totalMs=%llu",
                    _shader ? _shader->getName().c_str() : "",
                    static_cast<unsigned>(psoDesc.NumRenderTargets),
                    static_cast<unsigned>(psoDesc.InputLayout.NumElements),
                    static_cast<unsigned>(psoDesc.SampleDesc.Count),
                    psoCacheHit ? "hit" : (psoCacheStored ? "store" : "miss"),
                    static_cast<unsigned long long>(psoKeyMs),
                    static_cast<unsigned long long>(psoCacheReadMs),
                    static_cast<unsigned long long>(psoCacheStoreMs),
                    static_cast<unsigned long long>(createPsoMs),
                    static_cast<unsigned long long>(elapsedMs(initStart)));
    }
}

void CCD3D12PipelineState::doDestroy() {
    if (_impl) {
        _impl->pipelineState.Reset();
        _impl->dynamicPipelineStates.clear();
        _impl->inputElements.clear();
        _impl->baseDesc = {};
        _impl->rootSignature = nullptr;
        _impl->primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        _impl->usesPipelineLayoutRootSignature = false;
    }
}

void *CCD3D12PipelineState::getID3D12PipelineState() const {
    return _impl ? _impl->pipelineState.Get() : nullptr;
}

void *CCD3D12PipelineState::getDynamicID3D12PipelineState(float depthBias, float depthBiasClamp, float slopeScaledDepthBias,
                                                         uint32_t stencilReadMask, uint32_t stencilWriteMask) {
    if (!_impl || !_impl->pipelineState) {
        return nullptr;
    }

    uint32_t depthBiasClampBits = 0;
    uint32_t slopeScaledDepthBiasBits = 0;
    static_assert(sizeof(depthBiasClampBits) == sizeof(depthBiasClamp), "float bit size mismatch");
    std::memcpy(&depthBiasClampBits, &depthBiasClamp, sizeof(depthBiasClampBits));
    std::memcpy(&slopeScaledDepthBiasBits, &slopeScaledDepthBias, sizeof(slopeScaledDepthBiasBits));

    Impl::DynamicPipelineKey key{
        static_cast<INT>(depthBias),
        depthBiasClampBits,
        slopeScaledDepthBiasBits,
        static_cast<UINT8>(stencilReadMask),
        static_cast<UINT8>(stencilWriteMask),
    };
    auto cached = _impl->dynamicPipelineStates.find(key);
    if (cached != _impl->dynamicPipelineStates.end()) {
        return cached->second.Get();
    }

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        return nullptr;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC variantDesc = _impl->baseDesc;
    variantDesc.RasterizerState.DepthBias = static_cast<INT>(depthBias);
    variantDesc.RasterizerState.DepthBiasClamp = depthBiasClamp;
    variantDesc.RasterizerState.SlopeScaledDepthBias = slopeScaledDepthBias;
    variantDesc.DepthStencilState.StencilReadMask = static_cast<UINT8>(stencilReadMask);
    variantDesc.DepthStencilState.StencilWriteMask = static_cast<UINT8>(stencilWriteMask);
    variantDesc.InputLayout.pInputElementDescs = _impl->inputElements.empty() ? nullptr : _impl->inputElements.data();
    variantDesc.InputLayout.NumElements = static_cast<UINT>(_impl->inputElements.size());

    Microsoft::WRL::ComPtr<ID3D12PipelineState> variant;
    const auto createVariantStart = D3D12PerfClock::now();
    HRESULT hr = d3dDevice->CreateGraphicsPipelineState(&variantDesc, IID_PPV_ARGS(&variant));
    const auto createVariantMs = elapsedMs(createVariantStart);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12PipelineState: dynamic PSO variant creation failed. HRESULT=0x%08x",
                     static_cast<unsigned>(hr));
        return _impl->pipelineState.Get();
    }

    auto *result = variant.Get();
    _impl->dynamicPipelineStates.emplace(key, std::move(variant));
    CC_LOG_INFO("[D3D12-PERF] DynamicPipelineStateInit shader='%s' depthBias=%d stencilReadMask=%u stencilWriteMask=%u createGraphicsPsoMs=%llu variantCount=%u",
                _shader ? _shader->getName().c_str() : "",
                static_cast<int>(depthBias),
                static_cast<unsigned>(stencilReadMask),
                static_cast<unsigned>(stencilWriteMask),
                static_cast<unsigned long long>(createVariantMs),
                static_cast<unsigned>(_impl->dynamicPipelineStates.size()));
    return result;
}

uint32_t CCD3D12PipelineState::getD3D12PrimitiveTopology() const {
    return _impl ? static_cast<uint32_t>(_impl->primitiveTopology) : 0;
}

void *CCD3D12PipelineState::getID3D12RootSignature() const {
    return _impl ? _impl->rootSignature : nullptr;
}

bool CCD3D12PipelineState::usesPipelineLayoutRootSignature() const {
    return _impl ? _impl->usesPipelineLayoutRootSignature : false;
}

} // namespace gfx
} // namespace cc
