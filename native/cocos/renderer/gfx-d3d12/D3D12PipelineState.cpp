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
#include "base/Log.h"
#include "gfx-base/GFXDef.h"

// File diagnostic
#include <cstdio>
#include <cstdarg>
namespace {
void psoDiagLog(const char *fmt, ...) {
    static FILE *s_file = nullptr;
    if (!s_file) {
        s_file = fopen("C:\\temp\\d3d12-render-diag.log", "a");
        if (!s_file) return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(s_file, fmt, args);
    fflush(s_file);
    va_end(args);
}
} // anonymous namespace

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <dxgiformat.h>
    #include <wrl/client.h>
    #include <d3dcompiler.h>
#endif

namespace cc {
namespace gfx {

namespace {
#if defined(_WIN32)

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

#endif
} // namespace

struct CCD3D12PipelineState::Impl {
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
    D3D12_PRIMITIVE_TOPOLOGY primitiveTopology{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST};
#endif
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

#if defined(_WIN32)
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

    // If no VS bytecode, try runtime compilation of a built-in triangle shader
    Microsoft::WRL::ComPtr<ID3DBlob> compiledVS;
    Microsoft::WRL::ComPtr<ID3DBlob> compiledPS;

    if (!vsBlob.data || vsBlob.size == 0) {
        // Built-in HLSL triangle shader (fallback when GLSL->DXIL not available)
        // Uses SV_VertexID so no vertex buffer input is required.
        // vid % 3 ensures the triangle repeats safely for any vertex count.
        static const char *s_builtinHLSL =
            "float4 VSTriangle(uint vid : SV_VertexID) : SV_POSITION {\n"
            "    float2 positions[3] = { float2(0.0, 0.5), float2(0.5, -0.5), float2(-0.5, -0.5) };\n"
            "    return float4(positions[vid % 3], 0.0, 1.0);\n"
            "}\n"
            "float4 PSTriangle(float4 pos : SV_POSITION) : SV_TARGET {\n"
            "    return float4(1.0, 0.5, 0.2, 1.0);\n" // orange
            "}\n";

        Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
        HRESULT vsHR = D3DCompile(s_builtinHLSL, strlen(s_builtinHLSL),
                                   "builtin_triangle", nullptr, nullptr,
                                   "VSTriangle", "vs_5_0", 0, 0, &compiledVS, &errBlob);
        if (FAILED(vsHR)) {
            CC_LOG_ERROR("D3D12PipelineState: built-in VS compile failed: %s",
                         errBlob ? static_cast<const char *>(errBlob->GetBufferPointer()) : "unknown");
            return;
        }

        HRESULT psHR = D3DCompile(s_builtinHLSL, strlen(s_builtinHLSL),
                                   "builtin_triangle", nullptr, nullptr,
                                   "PSTriangle", "ps_5_0", 0, 0, &compiledPS, &errBlob);
        if (FAILED(psHR)) {
            CC_LOG_ERROR("D3D12PipelineState: built-in PS compile failed: %s",
                         errBlob ? static_cast<const char *>(errBlob->GetBufferPointer()) : "unknown");
            return;
        }

        vsBlob.data = compiledVS->GetBufferPointer();
        vsBlob.size = compiledVS->GetBufferSize();
        psBlob.data = compiledPS->GetBufferPointer();
        psBlob.size = compiledPS->GetBufferSize();
        psoDiagLog("[PSO] Using built-in HLSL fallback shader (VS=%zu bytes, PS=%zu bytes)\n",
                    vsBlob.size, psBlob.size);
        CC_LOG_INFO("D3D12PipelineState: using built-in triangle shader (runtime compiled).");
    }

    // Build D3D12_GRAPHICS_PIPELINE_STATE_DESC
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = nullptr;

    // Get root signature from PipelineLayout
    if (_pipelineLayout) {
        auto *d3d12Layout = static_cast<CCD3D12PipelineLayout *>(_pipelineLayout);
        psoDesc.pRootSignature = static_cast<ID3D12RootSignature *>(d3d12Layout->getID3D12RootSignature());
    }

    // If no root signature from PipelineLayout, create an empty one
    if (!psoDesc.pRootSignature) {
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
        if (SUCCEEDED(serHR)) {
            Microsoft::WRL::ComPtr<ID3D12RootSignature> emptyRootSig;
            HRESULT createHR = d3dDevice->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                                               sigBlob->GetBufferSize(),
                                                               IID_PPV_ARGS(&emptyRootSig));
            if (SUCCEEDED(createHR)) {
                // Store as a member for PSO creation. We use a static map to cache empty root signatures.
                // For simplicity, store on the Impl.
                _impl->pipelineState = nullptr; // ensure clean state
                psoDesc.pRootSignature = emptyRootSig.Get();
                // Keep the empty root signature alive by storing it in a static cache
                static Microsoft::WRL::ComPtr<ID3D12RootSignature> s_emptyRootSig;
                if (!s_emptyRootSig) {
                    s_emptyRootSig = emptyRootSig;
                }
                psoDesc.pRootSignature = s_emptyRootSig.Get();
                CC_LOG_INFO("D3D12PipelineState: created empty root signature for PSO.");
            } else {
                CC_LOG_WARNING("D3D12PipelineState: failed to create empty root signature. HRESULT=0x%08x",
                               static_cast<unsigned>(createHR));
            }
        }
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
        if (i < blend.targets.size()) {
            const auto &target = blend.targets[i];
            rtBlend.BlendEnable = target.blend ? TRUE : FALSE;
            rtBlend.SrcBlend = toD3D12Blend(target.blendSrc);
            rtBlend.DestBlend = toD3D12Blend(target.blendDst);
            rtBlend.BlendOp = toD3D12BlendOp(target.blendEq);
            rtBlend.SrcBlendAlpha = toD3D12Blend(target.blendSrcAlpha);
            rtBlend.DestBlendAlpha = toD3D12Blend(target.blendDstAlpha);
            rtBlend.BlendOpAlpha = toD3D12BlendOp(target.blendAlphaEq);
            rtBlend.RenderTargetWriteMask = toD3D12ColorWriteMask(target.blendColorMask);
        } else {
            rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
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

    // Input layout — build from _inputState attributes
    ccstd::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    if (!_inputState.attributes.empty()) {
        inputElements.reserve(_inputState.attributes.size());
        for (const auto &attr : _inputState.attributes) {
            D3D12_INPUT_ELEMENT_DESC elem{};
            // SemanticName: use location-based semantics for HLSL (TEXCOORDN)
            // If name starts with "a_", strip prefix; otherwise use as-is
            static thread_local char semanticName[64];
            if (attr.location < 10) {
                snprintf(semanticName, sizeof(semanticName), "TEXCOORD%u", attr.location);
            } else {
                snprintf(semanticName, sizeof(semanticName), "TEXCOORD%u", attr.location);
            }
            elem.SemanticName = nullptr; // Will set below with persistent storage
            elem.SemanticIndex = 0;
            elem.Format = toD3D12VertexFormat(attr.format);
            elem.InputSlot = attr.stream;
            elem.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
            elem.InputSlotClass = attr.isInstanced ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                    : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            elem.InstanceDataStepRate = attr.isInstanced ? 1 : 0;
            inputElements.push_back(elem);
        }

        // Store semantic names persistently for the lifetime of inputElements
        // (We need the string data to stay alive past this scope)
        static ccstd::vector<ccstd::string> s_semanticNames;
        s_semanticNames.clear();
        s_semanticNames.reserve(_inputState.attributes.size());
        for (const auto &attr : _inputState.attributes) {
            char buf[64];
            snprintf(buf, sizeof(buf), "TEXCOORD%u", attr.location);
            s_semanticNames.push_back(buf);
        }
        for (size_t i = 0; i < inputElements.size(); ++i) {
            inputElements[i].SemanticName = s_semanticNames[i].c_str();
        }

        psoDesc.InputLayout.pInputElementDescs = inputElements.data();
        psoDesc.InputLayout.NumElements = static_cast<UINT>(inputElements.size());
    } else {
        psoDesc.InputLayout.pInputElementDescs = nullptr;
        psoDesc.InputLayout.NumElements = 0;
    }

    // Primitive topology
    psoDesc.PrimitiveTopologyType = toD3D12PrimitiveTopologyType(_primitive);

    // Sample description
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;
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
        psoDiagLog("[PSO] CreateGraphicsPipelineState FAILED hr=0x%08x\n", static_cast<unsigned>(hr));
        CC_LOG_ERROR("D3D12PipelineState: CreateGraphicsPipelineState failed. HRESULT=0x%08x. "
                     "Attempting built-in fallback shader.",
                     static_cast<unsigned>(hr));

        // Retry: clear InputLayout and use fallback shader (SV_VertexID, no vertex buffer).
        // The primary PSO may fail because the inputLayout (from engine's vertex attributes)
        // doesn't match the fallback HLSL shader which only uses SV_VertexID.
        // Always attempt this retry — compiledVS/compiledPS may already be populated
        // from the first fallback path (lines above), so we must not guard on their nullity.
        {
            // Ensure we have fallback shader bytecode compiled
            if (!compiledVS || !compiledPS) {
                Microsoft::WRL::ComPtr<ID3DBlob> fbErr;
                static const char *s_fallbackHLSL =
                    "float4 VSTriangle(uint vid : SV_VertexID) : SV_POSITION {\n"
                    "    float2 pos[3] = { float2(0.0, 0.5), float2(0.5, -0.5), float2(-0.5, -0.5) };\n"
                    "    return float4(pos[vid % 3], 0.0, 1.0);\n"
                    "}\n"
                    "float4 PSTriangle(float4 p : SV_POSITION) : SV_TARGET {\n"
                    "    return float4(1.0, 0.5, 0.2, 1.0);\n"
                    "}\n";
                HRESULT vsH = D3DCompile(s_fallbackHLSL, strlen(s_fallbackHLSL),
                                          "fallback_triangle", nullptr, nullptr,
                                          "VSTriangle", "vs_5_0", 0, 0, &compiledVS, &fbErr);
                HRESULT psH = D3DCompile(s_fallbackHLSL, strlen(s_fallbackHLSL),
                                          "fallback_triangle", nullptr, nullptr,
                                          "PSTriangle", "ps_5_0", 0, 0, &compiledPS, &fbErr);
                if (FAILED(vsH) || FAILED(psH)) {
                    CC_LOG_ERROR("D3D12PipelineState: retry fallback shader compile failed.");
                }
            }

            if (compiledVS && compiledPS) {
                psoDesc.VS.pShaderBytecode = compiledVS->GetBufferPointer();
                psoDesc.VS.BytecodeLength = compiledVS->GetBufferSize();
                psoDesc.PS.pShaderBytecode = compiledPS->GetBufferPointer();
                psoDesc.PS.BytecodeLength = compiledPS->GetBufferSize();
                // Clear input layout — fallback shader uses SV_VertexID (no vertex input)
                psoDesc.InputLayout.pInputElementDescs = nullptr;
                psoDesc.InputLayout.NumElements = 0;

                psoDiagLog("[PSO] Retrying with cleared InputLayout (SV_VertexID shader)...\n");
                hr = d3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_impl->pipelineState));
                if (SUCCEEDED(hr)) {
                    psoDiagLog("[PSO] Retry PSO created successfully!\n");
                    CC_LOG_INFO("D3D12PipelineState: retry PSO (no input layout) created successfully.");
                } else {
                    psoDiagLog("[PSO] Retry PSO ALSO FAILED hr=0x%08x\n", static_cast<unsigned>(hr));
                    CC_LOG_ERROR("D3D12PipelineState: retry PSO also failed. HRESULT=0x%08x",
                                 static_cast<unsigned>(hr));
                }
            }
        }
    } else {
        psoDiagLog("[PSO] Created successfully (primary path).\n");
        CC_LOG_INFO("D3D12PipelineState created successfully.");
    }
#endif
}

void CCD3D12PipelineState::doDestroy() {
#if defined(_WIN32)
    if (_impl) {
        _impl->pipelineState.Reset();
        _impl->primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
#endif
}

void *CCD3D12PipelineState::getID3D12PipelineState() const {
#if defined(_WIN32)
    return _impl ? _impl->pipelineState.Get() : nullptr;
#else
    return nullptr;
#endif
}

uint32_t CCD3D12PipelineState::getD3D12PrimitiveTopology() const {
#if defined(_WIN32)
    return _impl ? static_cast<uint32_t>(_impl->primitiveTopology) : 0;
#else
    return 0;
#endif
}

} // namespace gfx
} // namespace cc
