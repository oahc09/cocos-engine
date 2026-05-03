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
#include "D3D12PipelineLayout.h"
#include "D3D12DescriptorSetLayout.h"
#include "base/Log.h"
#include "gfx-base/GFXDef.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <dxgiformat.h>
    #include <wrl/client.h>
    #include <d3dcompiler.h>

namespace cc {
namespace gfx {

namespace {

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

} // namespace

struct CCD3D12PipelineState::Impl {
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
    ID3D12RootSignature *rootSignature{nullptr};
    D3D12_PRIMITIVE_TOPOLOGY primitiveTopology{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST};
    bool usesPipelineLayoutRootSignature{false};
    bool diagnosticFallback{false};
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
    if (!_impl) return;

    _impl->pipelineState.Reset();
    _impl->rootSignature = nullptr;
    _impl->usesPipelineLayoutRootSignature = false;
    _impl->diagnosticFallback = false;

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

    // Diagnostic: log blend state for this PSO
    {
        const char *shaderName = _shader ? _shader->getName().c_str() : "<null>";
        bool anyBlendEnabled = false;
        for (size_t i = 0; i < blend.targets.size(); ++i) {
            if (blend.targets[i].blend) { anyBlendEnabled = true; break; }
        }
        CC_LOG_INFO("[D3D12-PSO] BlendState: shader='%s' isA2C=%u isIndepend=%u targets=%zu anyBlend=%s",
                     shaderName, blend.isA2C, blend.isIndepend,
                     static_cast<unsigned>(blend.targets.size()),
                     anyBlendEnabled ? "YES" : "NO");
        for (size_t i = 0; i < blend.targets.size() && i < 4; ++i) {
            const auto &t = blend.targets[i];
            CC_LOG_INFO("[D3D12-PSO]   target[%zu]: blend=%u src=%u dst=%u op=%u srcA=%u dstA=%u opA=%u mask=0x%x",
                         i, t.blend, static_cast<unsigned>(t.blendSrc), static_cast<unsigned>(t.blendDst),
                         static_cast<unsigned>(t.blendEq), static_cast<unsigned>(t.blendSrcAlpha),
                         static_cast<unsigned>(t.blendDstAlpha), static_cast<unsigned>(t.blendAlphaEq),
                         static_cast<unsigned>(t.blendColorMask));
        }
    }

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
    psoDesc.DepthStencilState.StencilEnable = ds.stencilTestFront ? TRUE : FALSE;
    psoDesc.DepthStencilState.StencilReadMask = static_cast<UINT8>(ds.stencilReadMaskFront);
    psoDesc.DepthStencilState.StencilWriteMask = static_cast<UINT8>(ds.stencilWriteMaskFront);

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
        const auto &rtvFormats = d3d12RenderPass->getRTVFormats();
        numRenderTargets = static_cast<UINT>(rtvFormats.size());
        psoDesc.NumRenderTargets = numRenderTargets;
        for (UINT i = 0; i < numRenderTargets && i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            psoDesc.RTVFormats[i] = static_cast<DXGI_FORMAT>(rtvFormats[i]);
        }
        psoDesc.DSVFormat = static_cast<DXGI_FORMAT>(d3d12RenderPass->getDSVFormat());
    } else {
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; // default
    }

    // Input layout. SPIRV-Cross emits GLSL vertex inputs as TEXCOORD+location in
    // HLSL, so build the D3D12 layout from the shader's active attributes and
    // match them back to the IA attributes by name. Some effects (particles,
    // skinned variants, etc.) conditionally remove attributes via macros; using
    // the raw IA order as TEXCOORD0..N makes later inputs shift and corrupts VS
    // data.
    ccstd::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    const AttributeList &shaderAttributes = _shader ? _shader->getAttributes() : _inputState.attributes;
    if (!shaderAttributes.empty()) {
        inputElements.reserve(shaderAttributes.size());
        _impl->semanticNames.clear();
        _impl->semanticNames.reserve(shaderAttributes.size());

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
                    elem.InputSlotClass = attr.isInstanced ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                            : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                    elem.InstanceDataStepRate = attr.isInstanced ? 1 : 0;
                    attributeFound = true;
                    break;
                }
                offsets[attr.stream] += GFX_FORMAT_INFOS[static_cast<uint32_t>(attr.format)].size;
            }

            if (!attributeFound) {
                // Keep PSO creation valid if a shader declares an attribute that
                // the IA does not provide. This mirrors the fallback used by the
                // Vulkan/WGPU backends: read dummy data from the beginning of
                // stream 0 instead of shifting all following attributes.
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
        uint32_t sampleCount = d3d12RenderPass->getSampleCount();
        psoDesc.SampleDesc.Count = (sampleCount > 0) ? sampleCount : 1;
        psoDesc.SampleDesc.Quality = 0;
    } else {
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleDesc.Quality = 0;
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

    HRESULT hr = d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_impl->pipelineState));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12PipelineState: CreateGraphicsPipelineState failed. HRESULT=0x%08x. "
                     "Retrying with cleared InputLayout.",
                     static_cast<unsigned>(hr));

        // Retry: keep the real shader bytecode but clear InputLayout.
        // The primary PSO may fail because the engine's vertex attribute format/stride
        // doesn't exactly match the HLSL shader's input signature (e.g., RG32F vs float3).
        // By clearing InputLayout, the shader runs with vertex ID only semantics and
        // descriptors (cbuffer/textures) still bind correctly through the root signature.
        {
            psoDesc.InputLayout.pInputElementDescs = nullptr;
            psoDesc.InputLayout.NumElements = 0;

            hr = d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_impl->pipelineState));
            if (SUCCEEDED(hr)) {
                _impl->rootSignature = psoDesc.pRootSignature;
                _impl->diagnosticFallback = true;
                CC_LOG_INFO("D3D12PipelineState: retry PSO created successfully with cleared InputLayout.");
            } else {
                // Try with empty root signature to isolate: is the issue RootSig or shader/PSO-desc?
                auto *emptyRootSig = getOrCreateEmptyRootSignature(d3dDevice);
                if (emptyRootSig) {
                    psoDesc.pRootSignature = emptyRootSig;
                    hr = d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_impl->pipelineState));
                    if (SUCCEEDED(hr)) {
                        _impl->rootSignature = psoDesc.pRootSignature;
                        _impl->usesPipelineLayoutRootSignature = false;
                        _impl->diagnosticFallback = true;
                        CC_LOG_INFO("D3D12PipelineState: PSO created with empty root signature (root sig mismatch).");
                    }
                }

                if (FAILED(hr)) {
                    CC_LOG_ERROR("D3D12PipelineState: all PSO creation attempts failed. HRESULT=0x%08x",
                                 static_cast<unsigned>(hr));
                }
            }
        }
    } else {
        _impl->rootSignature = psoDesc.pRootSignature;
        CC_LOG_INFO("D3D12PipelineState created successfully.");
    }
}

void CCD3D12PipelineState::doDestroy() {
    if (_impl) {
        _impl->pipelineState.Reset();
        _impl->rootSignature = nullptr;
        _impl->primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        _impl->usesPipelineLayoutRootSignature = false;
        _impl->diagnosticFallback = false;
    }
}

void *CCD3D12PipelineState::getID3D12PipelineState() const {
    return _impl ? _impl->pipelineState.Get() : nullptr;
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

bool CCD3D12PipelineState::isDiagnosticFallback() const {
    return _impl ? _impl->diagnosticFallback : false;
}

} // namespace gfx
} // namespace cc
