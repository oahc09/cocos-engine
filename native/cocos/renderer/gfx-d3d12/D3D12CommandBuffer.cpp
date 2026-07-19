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
#include "D3D12Buffer.h"
#include "D3D12DescriptorSet.h"
#include "D3D12DescriptorSetLayout.h"
#include "D3D12DescriptorHeapPool.h"
#include "D3D12Device.h"
#include "D3D12Framebuffer.h"
#include "D3D12InputAssembler.h"
#include "D3D12PipelineLayout.h"
#include "D3D12PipelineState.h"
#include "D3D12QueryPool.h"
#include "D3D12RenderPass.h"
#include "D3D12Swapchain.h"
#include "D3D12Texture.h"
#include "base/Log.h"
#include "gfx-base/GFXDef.h"


    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <algorithm>
    #include <cstring>
    #include <d3d12.h>
    #include <d3dcompiler.h>
    #include <limits>
    #include <unordered_map>
    #include <unordered_set>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

// Maximum number of descriptor sets that can be bound simultaneously
static constexpr uint32_t D3D12_MAX_BOUND_SETS = 4;
static constexpr uint32_t D3D12_MAX_ROOT_PARAMETERS = 64;
static constexpr uint32_t D3D12_MAX_VERTEX_BUFFERS = 16;
// Cocos' standard binding ABI is GLOBAL=0, MATERIAL=1, LOCAL=2. Keep this
// diagnostic backend-local; it must not affect generic descriptor semantics.
static constexpr uint32_t D3D12_LOCAL_DESCRIPTOR_SET_INDEX = 2;

// Maximum resource barriers per render pass transition (swapchain + color attachments + depth)
static constexpr uint32_t MAX_PASS_BARRIERS = 16;

namespace {
uint64_t hashSamplerTableKey(const ccstd::vector<uint32_t> &key) {
    uint64_t hash = 1469598103934665603ULL;
    hash ^= static_cast<uint64_t>(key.size());
    hash *= 1099511628211ULL;
    for (const uint32_t value : key) {
        hash ^= static_cast<uint64_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}


void retainCommandListResource(ccstd::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> &resources,
                               std::unordered_set<ID3D12Resource *> &retainedResources,
                               ID3D12Resource *resource) {
    if (!resource) {
        return;
    }
    if (auto *device = CCD3D12Device::getInstance(); device && device->isSwapchainBackBuffer(resource)) {
        return;
    }
    if (!retainedResources.emplace(resource).second) {
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> retained;
    retained = resource;
    resources.push_back(std::move(retained));
}

D3D12_RECT makeSafeRenderAreaRect(const Rect &renderArea, uint32_t framebufferWidth, uint32_t framebufferHeight) {
    const int32_t fbWidth = static_cast<int32_t>(framebufferWidth);
    const int32_t fbHeight = static_cast<int32_t>(framebufferHeight);

    int32_t left = renderArea.x;
    int32_t top = renderArea.y;
    int32_t right = left + static_cast<int32_t>(renderArea.width > 0 ? renderArea.width : framebufferWidth);
    int32_t bottom = top + static_cast<int32_t>(renderArea.height > 0 ? renderArea.height : framebufferHeight);

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > fbWidth) right = fbWidth;
    if (bottom > fbHeight) bottom = fbHeight;
    if (right < left) right = left;
    if (bottom < top) bottom = top;

    return D3D12_RECT{
        static_cast<LONG>(left),
        static_cast<LONG>(top),
        static_cast<LONG>(right),
        static_cast<LONG>(bottom),
    };
}

bool hasDepthComponent(Format format) {
    if (format == Format::UNKNOWN) {
        return true;
    }
    return GFX_FORMAT_INFOS[toNumber(format)].hasDepth;
}

bool hasStencilComponent(Format format) {
    if (format == Format::UNKNOWN) {
        return false;
    }
    return GFX_FORMAT_INFOS[toNumber(format)].hasStencil;
}

D3D12_RESOURCE_STATES getPostTransferTextureState(const TextureInfo &info) {
    if (hasFlag(info.usage, TextureUsageBit::SAMPLED)) {
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (hasFlag(info.usage, TextureUsageBit::DEPTH_STENCIL_ATTACHMENT)) {
        return D3D12_RESOURCE_STATE_DEPTH_READ;
    }
    if (hasFlag(info.usage, TextureUsageBit::COLOR_ATTACHMENT)) {
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

DXGI_FORMAT toBlitDXGIFormat(Format format) {
    switch (format) {
        case Format::R8:          return DXGI_FORMAT_R8_UNORM;
        case Format::R8SN:        return DXGI_FORMAT_R8_SNORM;
        case Format::RG8:         return DXGI_FORMAT_R8G8_UNORM;
        case Format::RG8SN:       return DXGI_FORMAT_R8G8_SNORM;
        case Format::RGBA8:       return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::BGRA8:       return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::SRGB8_A8:    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::RGBA8SN:     return DXGI_FORMAT_R8G8B8A8_SNORM;
        case Format::R16F:        return DXGI_FORMAT_R16_FLOAT;
        case Format::RG16F:       return DXGI_FORMAT_R16G16_FLOAT;
        case Format::RGBA16F:     return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R32F:        return DXGI_FORMAT_R32_FLOAT;
        case Format::RG32F:       return DXGI_FORMAT_R32G32_FLOAT;
        case Format::RGBA32F:     return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::RGB10A2:     return DXGI_FORMAT_R10G10B10A2_UNORM;
        case Format::R11G11B10F:  return DXGI_FORMAT_R11G11B10_FLOAT;
        case Format::RGBA4:       return DXGI_FORMAT_B4G4R4A4_UNORM;
        case Format::RGB5A1:      return DXGI_FORMAT_B5G5R5A1_UNORM;
        default:                  return DXGI_FORMAT_UNKNOWN;
    }
}

bool supportsShaderBlit(const TextureInfo &srcInfo, const TextureInfo &dstInfo) {
    const auto &srcFormatInfo = GFX_FORMAT_INFOS[toNumber(srcInfo.format)];
    const auto &dstFormatInfo = GFX_FORMAT_INFOS[toNumber(dstInfo.format)];
    if (srcFormatInfo.hasDepth || srcFormatInfo.hasStencil || dstFormatInfo.hasDepth || dstFormatInfo.hasStencil) return false;
    if (srcFormatInfo.isCompressed || dstFormatInfo.isCompressed) return false;
    if (srcFormatInfo.type == FormatType::UINT || srcFormatInfo.type == FormatType::INT) return false;
    if (dstFormatInfo.type == FormatType::UINT || dstFormatInfo.type == FormatType::INT) return false;
    if (srcInfo.samples != SampleCount::X1 || dstInfo.samples != SampleCount::X1) return false;
    if (srcInfo.type == TextureType::TEX3D || dstInfo.type == TextureType::TEX3D) return false;
    return toBlitDXGIFormat(srcInfo.format) != DXGI_FORMAT_UNKNOWN &&
           toBlitDXGIFormat(dstInfo.format) != DXGI_FORMAT_UNKNOWN;
}

struct D3D12BlitPipeline {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
};

struct D3D12MipDescriptorSet {
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> samplerHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT srvDescriptorSize{0};
    UINT rtvDescriptorSize{0};
    uint32_t nextSlot{0};
};

bool createMipDescriptorSet(ID3D12Device *device, uint32_t passCount, D3D12MipDescriptorSet &descriptorSet) {
    if (!device || passCount == 0) {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = passCount;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&descriptorSet.srvHeap));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 mip SRV descriptor heap creation failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
    samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samplerHeapDesc.NumDescriptors = 1;
    samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&descriptorSet.samplerHeap));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 mip sampler descriptor heap creation failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = passCount;
    hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&descriptorSet.rtvHeap));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 mip RTV descriptor heap creation failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    descriptorSet.srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    descriptorSet.rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    descriptorSet.nextSlot = 0;
    return true;
}

D3D12BlitPipeline *getOrCreateBlitPipeline(ID3D12Device *device, DXGI_FORMAT rtvFormat) {
    static std::unordered_map<uint32_t, D3D12BlitPipeline> pipelines;
    const uint32_t key = static_cast<uint32_t>(rtvFormat);
    auto iter = pipelines.find(key);
    if (iter != pipelines.end()) {
        return &iter->second;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[3]{};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[2].Constants.ShaderRegister = 0;
    rootParameters[2].Constants.RegisterSpace = 0;
    rootParameters[2].Constants.Num32BitValues = 12;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
    rootSigDesc.NumParameters = 3;
    rootSigDesc.pParameters = rootParameters;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> rootBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errorBlob);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 blit root signature serialization failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return nullptr;
    }

    D3D12BlitPipeline pipeline;
    hr = device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&pipeline.rootSignature));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 blit root signature creation failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return nullptr;
    }

    static constexpr const char *BLIT_HLSL = R"(
Texture2DArray<float4> SourceTexture : register(t0);
SamplerState SourceSampler : register(s0);

cbuffer BlitConstants : register(b0) {
    float4 SrcRect;
    float4 DstRect;
    float4 SrcInfo;
};

struct VSOut {
    float4 position : SV_Position;
};

VSOut VSMain(uint vertexId : SV_VertexID) {
    float2 pos = float2((vertexId == 2) ? 3.0 : -1.0, (vertexId == 1) ? 3.0 : -1.0);
    VSOut output;
    output.position = float4(pos, 0.0, 1.0);
    return output;
}

float4 PSMain(VSOut input) : SV_Target {
    float2 dstUV = (input.position.xy - DstRect.xy) / DstRect.zw;
    float2 srcPixel = SrcRect.xy + dstUV * SrcRect.zw;
    float2 uv = srcPixel * SrcInfo.xy;
    return SourceTexture.SampleLevel(SourceSampler, float3(uv, SrcInfo.z), 0.0);
}
)";

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
    hr = D3DCompile(BLIT_HLSL, std::strlen(BLIT_HLSL), "D3D12Blit", nullptr, nullptr, "VSMain", "vs_5_1", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 blit VS compile failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return nullptr;
    }
    hr = D3DCompile(BLIT_HLSL, std::strlen(BLIT_HLSL), "D3D12Blit", nullptr, nullptr, "PSMain", "ps_5_1", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 blit PS compile failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return nullptr;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = pipeline.rootSignature.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = rtvFormat;
    psoDesc.SampleDesc.Count = 1;

    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipeline.pipelineState));
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12 blit PSO creation failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return nullptr;
    }

    auto inserted = pipelines.emplace(key, std::move(pipeline));
    return &inserted.first->second;
}

bool shaderBlitRegion(ID3D12Device *device,
                      ID3D12GraphicsCommandList *commandList,
                      ID3D12Resource *srcResource,
                      ID3D12Resource *dstResource,
                      const TextureInfo &srcInfo,
                      const TextureInfo &dstInfo,
                      const TextureBlit &region,
                      uint32_t srcLayer,
                      uint32_t dstLayer,
                      Filter filter,
                      ccstd::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> &pendingDescriptorHeaps,
                      D3D12MipDescriptorSet *reusableDescriptors = nullptr) {
    const DXGI_FORMAT srvFormat = toBlitDXGIFormat(srcInfo.format);
    const DXGI_FORMAT rtvFormat = toBlitDXGIFormat(dstInfo.format);
    auto *pipeline = getOrCreateBlitPipeline(device, rtvFormat);
    if (!pipeline) return false;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> samplerHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ID3D12DescriptorHeap *srvHeapPtr = nullptr;
    ID3D12DescriptorHeap *samplerHeapPtr = nullptr;
    ID3D12DescriptorHeap *rtvHeapPtr = nullptr;
    uint32_t descriptorSlot = 0;
    UINT srvDescriptorSize = 0;
    UINT rtvDescriptorSize = 0;

    if (reusableDescriptors) {
        if (!reusableDescriptors->srvHeap || !reusableDescriptors->samplerHeap || !reusableDescriptors->rtvHeap) {
            return false;
        }
        srvHeapPtr = reusableDescriptors->srvHeap.Get();
        samplerHeapPtr = reusableDescriptors->samplerHeap.Get();
        rtvHeapPtr = reusableDescriptors->rtvHeap.Get();
        descriptorSlot = reusableDescriptors->nextSlot++;
        srvDescriptorSize = reusableDescriptors->srvDescriptorSize;
        rtvDescriptorSize = reusableDescriptors->rtvDescriptorSize;
    } else {
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.NumDescriptors = 1;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hr = device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap));
        if (FAILED(hr)) return false;

        D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc{};
        samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeapDesc.NumDescriptors = 1;
        samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&samplerHeap));
        if (FAILED(hr)) return false;

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
        if (FAILED(hr)) return false;

        srvHeapPtr = srvHeap.Get();
        samplerHeapPtr = samplerHeap.Get();
        rtvHeapPtr = rtvHeap.Get();
        srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = srvFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MostDetailedMip = region.srcSubres.mipLevel;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = srcLayer;
    srvDesc.Texture2DArray.ArraySize = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle = srvHeapPtr->GetCPUDescriptorHandleForHeapStart();
    srvCpuHandle.ptr += static_cast<SIZE_T>(descriptorSlot) * srvDescriptorSize;
    device->CreateShaderResourceView(srcResource, &srvDesc, srvCpuHandle);

    D3D12_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = filter == Filter::POINT ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    if (!reusableDescriptors || descriptorSlot == 0) {
        device->CreateSampler(&samplerDesc, samplerHeapPtr->GetCPUDescriptorHandleForHeapStart());
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = rtvFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtvDesc.Texture2DArray.MipSlice = region.dstSubres.mipLevel;
    rtvDesc.Texture2DArray.FirstArraySlice = dstLayer;
    rtvDesc.Texture2DArray.ArraySize = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpuHandle = rtvHeapPtr->GetCPUDescriptorHandleForHeapStart();
    rtvCpuHandle.ptr += static_cast<SIZE_T>(descriptorSlot) * rtvDescriptorSize;
    device->CreateRenderTargetView(dstResource, &rtvDesc, rtvCpuHandle);

    const bool bindStaticBlitState = !reusableDescriptors || descriptorSlot == 0;
    if (bindStaticBlitState) {
        ID3D12DescriptorHeap *heaps[] = {srvHeapPtr, samplerHeapPtr};
        commandList->SetDescriptorHeaps(2, heaps);
        commandList->SetGraphicsRootSignature(pipeline->rootSignature.Get());
    }
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle = srvHeapPtr->GetGPUDescriptorHandleForHeapStart();
    srvGpuHandle.ptr += static_cast<UINT64>(descriptorSlot) * srvDescriptorSize;
    commandList->SetGraphicsRootDescriptorTable(0, srvGpuHandle);
    if (bindStaticBlitState) {
        commandList->SetGraphicsRootDescriptorTable(1, samplerHeapPtr->GetGPUDescriptorHandleForHeapStart());
    }

    const uint32_t srcMipWidth = std::max<uint32_t>(srcInfo.width >> region.srcSubres.mipLevel, 1);
    const uint32_t srcMipHeight = std::max<uint32_t>(srcInfo.height >> region.srcSubres.mipLevel, 1);
    struct BlitConstants {
        float srcRect[4];
        float dstRect[4];
        float srcInfo[4];
    } constants{
        {static_cast<float>(region.srcOffset.x), static_cast<float>(region.srcOffset.y),
         static_cast<float>(region.srcExtent.width), static_cast<float>(region.srcExtent.height)},
        {static_cast<float>(region.dstOffset.x), static_cast<float>(region.dstOffset.y),
         static_cast<float>(region.dstExtent.width), static_cast<float>(region.dstExtent.height)},
        {1.0F / static_cast<float>(srcMipWidth), 1.0F / static_cast<float>(srcMipHeight),
         0.0F, 0.0F},
    };
    commandList->SetGraphicsRoot32BitConstants(2, 12, &constants, 0);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvCpuHandle;
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = static_cast<float>(region.dstOffset.x);
    viewport.TopLeftY = static_cast<float>(region.dstOffset.y);
    viewport.Width = static_cast<float>(region.dstExtent.width);
    viewport.Height = static_cast<float>(region.dstExtent.height);
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;
    commandList->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{
        static_cast<LONG>(region.dstOffset.x),
        static_cast<LONG>(region.dstOffset.y),
        static_cast<LONG>(region.dstOffset.x + region.dstExtent.width),
        static_cast<LONG>(region.dstOffset.y + region.dstExtent.height),
    };
    commandList->RSSetScissorRects(1, &scissor);
    if (bindStaticBlitState) {
        commandList->SetPipelineState(pipeline->pipelineState.Get());
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    commandList->DrawInstanced(3, 1, 0, 0);
    if (!reusableDescriptors) {
        pendingDescriptorHeaps.push_back(std::move(srvHeap));
        pendingDescriptorHeaps.push_back(std::move(samplerHeap));
        pendingDescriptorHeaps.push_back(std::move(rtvHeap));
    }
    return true;
}

bool canGenerateMipmaps(const TextureInfo &textureInfo, ID3D12Resource *resource) {
    if (!resource || !hasFlag(textureInfo.flags, TextureFlagBit::GEN_MIPMAP) ||
        textureInfo.levelCount <= 1 || textureInfo.samples != SampleCount::X1 ||
        textureInfo.type == TextureType::TEX3D) {
        return false;
    }

    const auto &formatInfo = GFX_FORMAT_INFOS[toNumber(textureInfo.format)];
    if (formatInfo.hasDepth || formatInfo.hasStencil || formatInfo.isCompressed ||
        formatInfo.type == FormatType::UINT || formatInfo.type == FormatType::INT) {
        return false;
    }
    return (resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 &&
           toBlitDXGIFormat(textureInfo.format) != DXGI_FORMAT_UNKNOWN;
}

bool generateMipmaps(ID3D12Device *device,
                     ID3D12GraphicsCommandList *commandList,
                     ID3D12Resource *resource,
                     const TextureInfo &textureInfo,
                     ccstd::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> &pendingDescriptorHeaps) {
    if (!canGenerateMipmaps(textureInfo, resource)) {
#ifndef NDEBUG
        if (hasFlag(textureInfo.flags, TextureFlagBit::GEN_MIPMAP) && textureInfo.levelCount > 1) {
            CC_LOG_WARNING("[D3D12-MIP-DIAG] generation rejected resource=%p size=%ux%u levels=%u format=%u flags=0x%x",
                           resource, textureInfo.width, textureInfo.height, textureInfo.levelCount,
                           static_cast<unsigned>(textureInfo.format),
                           resource ? static_cast<unsigned>(resource->GetDesc().Flags) : 0U);
        }
#endif
        return false;
    }

    const uint32_t mipCount = textureInfo.levelCount;
    const uint32_t layerCount = std::max<uint32_t>(textureInfo.layerCount, 1);
    D3D12MipDescriptorSet mipDescriptors;
    const uint32_t mipPassCount = (mipCount - 1) * layerCount;
    if (!createMipDescriptorSet(device, mipPassCount, mipDescriptors)) {
        return false;
    }

    for (uint32_t mip = 1; mip < mipCount; ++mip) {
        const uint32_t srcWidth = std::max<uint32_t>(textureInfo.width >> (mip - 1), 1);
        const uint32_t srcHeight = std::max<uint32_t>(textureInfo.height >> (mip - 1), 1);
        const uint32_t dstWidth = std::max<uint32_t>(textureInfo.width >> mip, 1);
        const uint32_t dstHeight = std::max<uint32_t>(textureInfo.height >> mip, 1);

        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            const uint32_t srcSubresource = (mip - 1) + layer * mipCount;
            const uint32_t dstSubresource = mip + layer * mipCount;

            D3D12_RESOURCE_BARRIER preBarriers[2]{};
            preBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            preBarriers[0].Transition.pResource = resource;
            preBarriers[0].Transition.Subresource = srcSubresource;
            preBarriers[0].Transition.StateBefore =
                mip == 1 ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            preBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

            preBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            preBarriers[1].Transition.pResource = resource;
            preBarriers[1].Transition.Subresource = dstSubresource;
            preBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            preBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

            const UINT preBarrierCount = mip == 1 ? 2U : 1U;
            commandList->ResourceBarrier(preBarrierCount, mip == 1 ? preBarriers : &preBarriers[1]);

            TextureBlit region{};
            region.srcSubres.mipLevel = mip - 1;
            region.srcSubres.baseArrayLayer = layer;
            region.srcExtent = {srcWidth, srcHeight, 1};
            region.dstSubres.mipLevel = mip;
            region.dstSubres.baseArrayLayer = layer;
            region.dstExtent = {dstWidth, dstHeight, 1};
            if (!shaderBlitRegion(device, commandList, resource, resource,
                                  textureInfo, textureInfo, region, layer, layer,
                                  Filter::LINEAR, pendingDescriptorHeaps, &mipDescriptors)) {
                return false;
            }

            D3D12_RESOURCE_BARRIER toShaderRead{};
            toShaderRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toShaderRead.Transition.pResource = resource;
            toShaderRead.Transition.Subresource = dstSubresource;
            toShaderRead.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toShaderRead.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            commandList->ResourceBarrier(1, &toShaderRead);
        }
    }

    pendingDescriptorHeaps.push_back(std::move(mipDescriptors.srvHeap));
    pendingDescriptorHeaps.push_back(std::move(mipDescriptors.samplerHeap));
    pendingDescriptorHeaps.push_back(std::move(mipDescriptors.rtvHeap));
    return true;
}
} // namespace

bool generateD3D12Mipmaps(
    ID3D12Device *device,
    ID3D12GraphicsCommandList *commandList,
    ID3D12Resource *resource,
    const TextureInfo &textureInfo,
    ccstd::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> &pendingDescriptorHeaps) {
    return generateMipmaps(device, commandList, resource, textureInfo, pendingDescriptorHeaps);
}

struct CCD3D12CommandBuffer::Impl {
    struct PendingDefaultBufferCopy {
        CCD3D12Buffer *buffer{nullptr};
        ID3D12Resource *destination{nullptr};
        uint64_t destinationOffset{0};
        ID3D12Resource *source{nullptr};
        uint64_t sourceOffset{0};
        uint32_t size{0};
        uint32_t transitionIndex{0};
    };
    struct PendingDefaultBufferTransition {
        ID3D12Resource *resource{nullptr};
        D3D12_RESOURCE_STATES previousState{D3D12_RESOURCE_STATE_COMMON};
        uint32_t copyCount{1};
    };

    struct CommandRecordingContext {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
        Microsoft::WRL::ComPtr<ID3D12Fence> lastSubmittedFence;
        uint64_t lastSubmittedFenceValue{0};
        ccstd::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> pendingUploadResources;
        std::unordered_set<ID3D12Resource *> pendingUploadResourceSet;
        ccstd::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> pendingDescriptorHeaps;
    };
    CommandRecordingContext recordingContexts[D3D12_MAX_FRAMES_IN_FLIGHT];
    uint32_t activeRecordingContext{1};

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice; // cached ref, not owning

    // Track state for the current recording
    PipelineState *boundPipelineState{nullptr};
    PipelineLayout *boundPipelineLayout{nullptr};
    InputAssembler *boundIA{nullptr};
    ID3D12PipelineState *boundNativePipelineState{nullptr};
    ID3D12RootSignature *boundRootSignature{nullptr};
    D3D12_PRIMITIVE_TOPOLOGY boundPrimitiveTopology{D3D_PRIMITIVE_TOPOLOGY_UNDEFINED};
    bool primitiveTopologyValid{false};
    float boundBlendFactor[4]{};
    bool blendFactorValid{false};
    uint32_t boundStencilRef{0};
    bool stencilRefValid{false};
    D3D12_VERTEX_BUFFER_VIEW boundVertexBufferViews[D3D12_MAX_VERTEX_BUFFERS]{};
    uint32_t boundVertexBufferCount{0};
    bool vertexBufferStateValid{false};
    D3D12_INDEX_BUFFER_VIEW boundIndexBufferView{};
    bool boundHasIndexBuffer{false};
    bool indexBufferStateValid{false};
    D3D12_VIEWPORT boundViewport{};
    bool viewportValid{false};
    D3D12_RECT boundScissor{};
    bool scissorValid{false};
    bool isRecording{false};

    // Track swapchain for resource barriers during render pass
    CCD3D12Swapchain *activeSwapchain{nullptr};
    RenderPass *activeRenderPass{nullptr};
    CCD3D12Framebuffer *activeFramebuffer{nullptr};
    uint32_t currentSubpass{0};
    ID3D12Resource *activeSwapchainBackBuffer{nullptr};
    ID3D12Resource *activeDepthStencil{nullptr};
    CCD3D12Texture *activeDepthTexture{nullptr};
    struct ActiveColorTarget {
        ID3D12Resource *resource{nullptr};
        CCD3D12Texture *texture{nullptr};
        bool hasTextureState{false};
    };
    ccstd::vector<ActiveColorTarget> activeColorTargets;
    bool inRenderPass{false};

    // Deferred descriptor binding state — collected during bindDescriptorSet,
    // flushed to GPU during draw/dispatch to avoid multiple SetDescriptorHeaps calls.
    struct PendingDescriptorSet {
        DescriptorSet *set{nullptr};
        uint32_t setIndex{0};
        ccstd::vector<uint32_t> dynamicOffsets;
        uint64_t version{0};
        bool valid{false};
    };
    struct CachedGpuDescriptorRange {
        uint64_t version{0};
        uint64_t gpuHandle{0};
        uint32_t descriptorCount{0};
        uint32_t heapIndex{0};
        ID3D12DescriptorHeap *heap{nullptr};
        bool valid{false};
    };
    struct CachedGpuDescriptorSet {
        CCD3D12DescriptorSet *owner{nullptr};
        CachedGpuDescriptorRange cbvSrvUav;
        CachedGpuDescriptorRange sampler;
        ccstd::vector<uint32_t> cbvDynamicOffsets;

        void reset(CCD3D12DescriptorSet *newOwner = nullptr) {
            owner = newOwner;
            cbvSrvUav = {};
            sampler = {};
            cbvDynamicOffsets.clear();
        }
    };
    struct CachedSamplerTable {
        ccstd::vector<uint32_t> key;
        uint64_t gpuHandle{0};
        uint32_t descriptorCount{0};
        uint32_t heapIndex{0};
        ID3D12DescriptorHeap *heap{nullptr};
    };
    // The command-buffer half of the local root-table split uses the same
    // heap-epoch lifetime rule as samplerTableCache.  It remains inert until
    // both split root parameters are emitted by the active PipelineLayout.
    struct CachedLocalStaticCbvSrvUavTable {
        CCD3D12DescriptorSet *owner{nullptr};
        uint64_t signature{0};
        uint64_t gpuHandle{0};
        uint32_t descriptorCount{0};
        uint32_t heapIndex{0};
        ID3D12DescriptorHeap *heap{nullptr};
    };
    // All-null dynamic slots produce the same explicit null CBV/SRV
    // descriptors. Reuse their GPU table only for the same layout and heap
    // epoch; any real buffer binding takes the normal per-set path.
    struct CachedLocalNullDynamicCbvSrvUavTable {
        const DescriptorSetLayout *layout{nullptr};
        uint64_t gpuHandle{0};
        uint32_t descriptorCount{0};
        uint32_t heapIndex{0};
        ID3D12DescriptorHeap *heap{nullptr};
    };
    struct BoundRootTable {
        uint64_t gpuHandle{0};
        bool valid{false};
    };
    PendingDescriptorSet pendingSets[D3D12_MAX_BOUND_SETS]{};
    BoundRootTable boundRootTables[D3D12_MAX_ROOT_PARAMETERS]{};
    CachedGpuDescriptorSet gpuDescriptorCache[D3D12_MAX_BOUND_SETS]{};
    std::unordered_map<uint64_t, ccstd::vector<CachedSamplerTable>> samplerTableCache;
    std::unordered_map<uint64_t, ccstd::vector<CachedLocalStaticCbvSrvUavTable>> localStaticCbvSrvUavTableCache;
    CachedLocalNullDynamicCbvSrvUavTable localNullDynamicCbvSrvUavTable;
    ID3D12DescriptorHeap *samplerTableCacheHeap{nullptr};
    uint32_t samplerTableCacheHeapIndex{std::numeric_limits<uint32_t>::max()};
    uint32_t pendingSetCount{0};
    bool descriptorSetsDirty{false};
    bool localDescriptorSetOnlyDirty{false};
    ID3D12DescriptorHeap *boundCbvSrvUavHeap{nullptr};
    ID3D12DescriptorHeap *boundSamplerHeap{nullptr};
    struct LocalRootCbvIndirectDraw {
        uint64_t rootCbvGpuAddress{0};
        D3D12_DRAW_ARGUMENTS arguments{};
    };
    struct LocalRootCbvIndexedIndirectDraw {
        uint64_t rootCbvGpuAddress{0};
        D3D12_DRAW_INDEXED_ARGUMENTS arguments{};
        uint32_t padding{0};
    };
    static_assert(sizeof(LocalRootCbvIndirectDraw) == 24, "Unexpected D3D12 indirect draw stride");
    static_assert(sizeof(LocalRootCbvIndexedIndirectDraw) == 32, "Unexpected D3D12 indexed indirect draw stride");
    bool localRootCbvBatchActive{false};
    bool localRootCbvBatchReady{false};
    bool localRootCbvBatchIndexed{false};
    uint32_t localRootCbvBatchRootParameterIndex{0};
    ID3D12RootSignature *localRootCbvBatchRootSignature{nullptr};
    CCD3D12DescriptorSet *localRootCbvBatchDescriptorSet{nullptr};
    uint64_t localRootCbvBatchStaticSignature{0};
    ccstd::vector<uint32_t> localRootCbvBatchSamplerKey;
    ccstd::vector<LocalRootCbvIndirectDraw> localRootCbvBatchDraws;
    ccstd::vector<LocalRootCbvIndexedIndirectDraw> localRootCbvBatchIndexedDraws;
    float dynamicDepthBias{0.F};
    float dynamicDepthBiasClamp{0.F};
    float dynamicDepthBiasSlope{0.F};
    uint32_t dynamicStencilReadMask{0xFFFFFFFFU};
    uint32_t dynamicStencilWriteMask{0xFFFFFFFFU};
    bool hasDynamicDepthBias{false};
    bool hasDynamicStencilReadMask{false};
    bool hasDynamicStencilWriteMask{false};
    PipelineState *lastDynamicPipelineStateOwner{nullptr};
    float lastDynamicDepthBias{0.F};
    float lastDynamicDepthBiasClamp{0.F};
    float lastDynamicDepthBiasSlope{0.F};
    uint32_t lastDynamicStencilReadMask{0xFFFFFFFFU};
    uint32_t lastDynamicStencilWriteMask{0xFFFFFFFFU};
    bool dynamicPipelineStateValid{false};


    // Resources replaced while recording stay alive until this command buffer
    // can be safely reused. Upload heap pages are owned by the device ring.
    ccstd::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> pendingUploadResources;
    std::unordered_set<ID3D12Resource *> pendingUploadResourceSet;
    ccstd::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> pendingDescriptorHeaps;
    ccstd::vector<PendingDefaultBufferCopy> pendingDefaultBufferCopies;
    ccstd::vector<PendingDefaultBufferTransition> pendingDefaultBufferTransitions;
    std::unordered_map<ID3D12Resource *, uint32_t> pendingDefaultBufferTransitionIndices;
    ccstd::vector<D3D12_RESOURCE_BARRIER> bufferUpdatePreCopyBarriers;
    ccstd::vector<D3D12_RESOURCE_BARRIER> bufferUpdatePostCopyBarriers;
    bool bufferUpdateBatchActive{false};
    bool bufferUpdateBatchDestinationsAreUnique{false};
    // Kept after the existing hot recording state so Bundle lifetime tracking
    // does not shift fields accessed by every draw in primary-only scenes.
    ccstd::vector<IntrusivePtr<CCD3D12CommandBuffer>> executedBundles[D3D12_MAX_FRAMES_IN_FLIGHT];
};

CCD3D12CommandBuffer::CCD3D12CommandBuffer()
: _impl(std::make_unique<Impl>()) {
}

CCD3D12CommandBuffer::~CCD3D12CommandBuffer() = default;

void CCD3D12CommandBuffer::doInit(const CommandBufferInfo &info) {
    auto *device = CCD3D12Device::getInstance();
    if (!device) {
        CC_LOG_ERROR("D3D12CommandBuffer: device not available.");
        return;
    }

    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12CommandBuffer: D3D12 device handle is null.");
        return;
    }

    _impl->d3dDevice = d3dDevice;
    const D3D12_COMMAND_LIST_TYPE commandListType = info.type == CommandBufferType::SECONDARY
                                                       ? D3D12_COMMAND_LIST_TYPE_BUNDLE
                                                       : D3D12_COMMAND_LIST_TYPE_DIRECT;

    for (auto &context : _impl->recordingContexts) {
        HRESULT hr = d3dDevice->CreateCommandAllocator(
            commandListType,
            IID_PPV_ARGS(&context.commandAllocator));
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12CommandBuffer: CreateCommandAllocator failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return;
        }

        hr = d3dDevice->CreateCommandList(
            0,
            commandListType,
            context.commandAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&context.commandList));
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12CommandBuffer: CreateCommandList failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return;
        }

        // D3D12 command lists are created in open state, close them initially.
        hr = context.commandList->Close();
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12CommandBuffer: initial Close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return;
        }
    }
    _impl->commandAllocator = _impl->recordingContexts[0].commandAllocator;
    _impl->commandList = _impl->recordingContexts[0].commandList;

    CC_LOG_INFO("D3D12CommandBuffer initialized as %s.",
                info.type == CommandBufferType::SECONDARY ? "bundle" : "direct list");
}

void CCD3D12CommandBuffer::doDestroy() {
    for (auto &context : _impl->recordingContexts) {
        context.pendingDescriptorHeaps.clear();
        context.pendingUploadResources.clear();
        context.pendingUploadResourceSet.clear();
        context.lastSubmittedFence.Reset();
        context.commandList.Reset();
        context.commandAllocator.Reset();
    }
    for (auto &executedBundles : _impl->executedBundles) {
        executedBundles.clear();
    }
    _impl->commandList.Reset();
    _impl->commandAllocator.Reset();
    _impl->d3dDevice.Reset();
    _impl->boundPipelineState = nullptr;
    _impl->boundPipelineLayout = nullptr;
    for (auto &cachedSet : _impl->gpuDescriptorCache) {
        cachedSet.reset();
    }
    _impl->samplerTableCache.clear();
    _impl->localStaticCbvSrvUavTableCache.clear();
    _impl->localNullDynamicCbvSrvUavTable = {};
    _impl->samplerTableCacheHeap = nullptr;
    _impl->samplerTableCacheHeapIndex = std::numeric_limits<uint32_t>::max();
    _impl->pendingDefaultBufferCopies.clear();
    _impl->pendingDefaultBufferTransitions.clear();
    _impl->pendingDefaultBufferTransitionIndices.clear();
    _impl->bufferUpdatePreCopyBarriers.clear();
    _impl->bufferUpdatePostCopyBarriers.clear();
    _impl->bufferUpdateBatchActive = false;
    _impl->bufferUpdateBatchDestinationsAreUnique = false;
}

void CCD3D12CommandBuffer::notifySubmitted(void *fence, uint64_t fenceValue) {
    if (!_impl) {
        return;
    }
    auto &context = _impl->recordingContexts[_impl->activeRecordingContext];
    context.lastSubmittedFence = static_cast<ID3D12Fence *>(fence);
    context.lastSubmittedFenceValue = fenceValue;
    for (const auto &bundleCommandBuffer : _impl->executedBundles[_impl->activeRecordingContext]) {
        bundleCommandBuffer->notifySubmitted(fence, fenceValue);
    }
}

void CCD3D12CommandBuffer::waitForFenceValue() {
    if (!_impl) {
        return;
    }
    auto &context = _impl->recordingContexts[_impl->activeRecordingContext];
    if (!context.lastSubmittedFence || context.lastSubmittedFenceValue == 0) {
        return;
    }
    if (context.lastSubmittedFence->GetCompletedValue() >= context.lastSubmittedFenceValue) {
        context.lastSubmittedFence.Reset();
        context.lastSubmittedFenceValue = 0;
        return;
    }

    HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent) {
        CC_LOG_ERROR("D3D12CommandBuffer::begin - CreateEvent failed while waiting for allocator reuse.");
        return;
    }
    HRESULT hr = context.lastSubmittedFence->SetEventOnCompletion(context.lastSubmittedFenceValue, fenceEvent);
    if (SUCCEEDED(hr)) {
        WaitForSingleObject(fenceEvent, INFINITE);
        context.lastSubmittedFence.Reset();
        context.lastSubmittedFenceValue = 0;
    } else {
        CC_LOG_ERROR("D3D12CommandBuffer::begin - SetEventOnCompletion failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
    }
    CloseHandle(fenceEvent);
}

void CCD3D12CommandBuffer::startBufferUpdateBatch(bool destinationsAreUnique) {
    if (!_impl || _impl->bufferUpdateBatchActive) {
        return;
    }
    _impl->pendingDefaultBufferCopies.clear();
    _impl->pendingDefaultBufferTransitions.clear();
    _impl->pendingDefaultBufferTransitionIndices.clear();
    _impl->bufferUpdatePreCopyBarriers.clear();
    _impl->bufferUpdatePostCopyBarriers.clear();
    _impl->bufferUpdateBatchDestinationsAreUnique = destinationsAreUnique;
    _impl->bufferUpdateBatchActive = true;
}

void CCD3D12CommandBuffer::finishBufferUpdateBatch() {
    if (!_impl || !_impl->bufferUpdateBatchActive) {
        return;
    }
    _impl->bufferUpdateBatchActive = false;
    _impl->bufferUpdateBatchDestinationsAreUnique = false;

    if (_impl->pendingDefaultBufferCopies.empty()) {
        _impl->pendingDefaultBufferTransitions.clear();
        _impl->pendingDefaultBufferTransitionIndices.clear();
        return;
    }


    auto &preCopyBarriers = _impl->bufferUpdatePreCopyBarriers;
    auto &postCopyBarriers = _impl->bufferUpdatePostCopyBarriers;
    preCopyBarriers.clear();
    postCopyBarriers.clear();
    preCopyBarriers.reserve(_impl->pendingDefaultBufferTransitions.size());
    postCopyBarriers.reserve(_impl->pendingDefaultBufferTransitions.size());
    for (const auto &transition : _impl->pendingDefaultBufferTransitions) {
        if (!transition.resource || transition.copyCount != 1) {
            continue;
        }
        if (transition.previousState != D3D12_RESOURCE_STATE_COPY_DEST &&
            transition.previousState != D3D12_RESOURCE_STATE_COMMON) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = transition.resource;
            barrier.Transition.StateBefore = transition.previousState;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            preCopyBarriers.push_back(barrier);
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = transition.resource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postCopyBarriers.push_back(barrier);
    }

    if (!preCopyBarriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(preCopyBarriers.size()), preCopyBarriers.data());
    }
    for (const auto &copy : _impl->pendingDefaultBufferCopies) {
        if (copy.transitionIndex >= _impl->pendingDefaultBufferTransitions.size() ||
            _impl->pendingDefaultBufferTransitions[copy.transitionIndex].copyCount != 1) {
            continue;
        }
        _impl->commandList->CopyBufferRegion(
            copy.destination, copy.destinationOffset,
            copy.source, copy.sourceOffset, copy.size);
    }
    if (!postCopyBarriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(postCopyBarriers.size()), postCopyBarriers.data());
    }
    for (const auto &copy : _impl->pendingDefaultBufferCopies) {
        if (copy.buffer && copy.transitionIndex < _impl->pendingDefaultBufferTransitions.size() &&
            _impl->pendingDefaultBufferTransitions[copy.transitionIndex].copyCount == 1) {
            copy.buffer->setCurrentState(D3D12_RESOURCE_STATE_GENERIC_READ);
        }
    }

    // A repeated destination may represent multiple writes or aliased buffer
    // wrappers. Preserve the original per-copy barrier ordering for that rare
    // case instead of incorrectly coalescing COPY_DEST transitions.
    for (const auto &copy : _impl->pendingDefaultBufferCopies) {
        if (!copy.buffer || copy.transitionIndex >= _impl->pendingDefaultBufferTransitions.size()) {
            continue;
        }
        const auto &transition = _impl->pendingDefaultBufferTransitions[copy.transitionIndex];
        if (transition.copyCount == 1) {
            continue;
        }

        const auto previousState = copy.buffer->getCurrentState();
        if (previousState != D3D12_RESOURCE_STATE_COPY_DEST &&
            previousState != D3D12_RESOURCE_STATE_COMMON) {
            D3D12_RESOURCE_BARRIER toCopyDest{};
            toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopyDest.Transition.pResource = copy.destination;
            toCopyDest.Transition.StateBefore = previousState;
            toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            _impl->commandList->ResourceBarrier(1, &toCopyDest);
        }
        _impl->commandList->CopyBufferRegion(
            copy.destination, copy.destinationOffset,
            copy.source, copy.sourceOffset, copy.size);
        D3D12_RESOURCE_BARRIER toRead{};
        toRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRead.Transition.pResource = copy.destination;
        toRead.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toRead.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
        toRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        _impl->commandList->ResourceBarrier(1, &toRead);
        copy.buffer->setCurrentState(D3D12_RESOURCE_STATE_GENERIC_READ);
    }

    _impl->pendingDefaultBufferCopies.clear();
    _impl->pendingDefaultBufferTransitions.clear();
    _impl->pendingDefaultBufferTransitionIndices.clear();
}

void CCD3D12CommandBuffer::invalidateDescriptorTables() {
    for (auto &table : _impl->boundRootTables) {
        table = {};
    }
    _impl->localDescriptorSetOnlyDirty = false;
    if (_impl->pendingSetCount > 0) {
        _impl->descriptorSetsDirty = true;
    }
}

void CCD3D12CommandBuffer::invalidateGraphicsState() {
    _impl->boundPipelineState = nullptr;
    _impl->boundPipelineLayout = nullptr;
    _impl->boundIA = nullptr;
    _impl->boundNativePipelineState = nullptr;
    _impl->boundRootSignature = nullptr;
    _impl->boundCbvSrvUavHeap = nullptr;
    _impl->boundSamplerHeap = nullptr;
    _impl->primitiveTopologyValid = false;
    _impl->blendFactorValid = false;
    _impl->stencilRefValid = false;
    _impl->vertexBufferStateValid = false;
    _impl->indexBufferStateValid = false;
    _impl->viewportValid = false;
    _impl->scissorValid = false;
    _impl->dynamicPipelineStateValid = false;
    _impl->lastDynamicPipelineStateOwner = nullptr;
    invalidateDescriptorTables();
}

void CCD3D12CommandBuffer::begin(RenderPass *renderPass, uint32_t subpass, Framebuffer *frameBuffer) {
    (void)renderPass;
    (void)subpass;
    (void)frameBuffer;
    if (!_impl->commandAllocator || !_impl->commandList) {
        CC_LOG_ERROR("D3D12CommandBuffer::begin - allocator or command list is null.");
        return;
    }

    auto &previousContext = _impl->recordingContexts[_impl->activeRecordingContext];
    previousContext.pendingUploadResources = std::move(_impl->pendingUploadResources);
    previousContext.pendingUploadResourceSet = std::move(_impl->pendingUploadResourceSet);
    previousContext.pendingDescriptorHeaps = std::move(_impl->pendingDescriptorHeaps);
    _impl->activeRecordingContext = (_impl->activeRecordingContext + 1) % D3D12_MAX_FRAMES_IN_FLIGHT;
    auto &activeContext = _impl->recordingContexts[_impl->activeRecordingContext];
    _impl->commandAllocator = activeContext.commandAllocator;
    _impl->commandList = activeContext.commandList;

    waitForFenceValue();


    // The selected context's fence has completed, so its retained resources can
    // be released before recording a new command list into the same allocator.
    activeContext.pendingUploadResources.clear();
    activeContext.pendingUploadResourceSet.clear();
    activeContext.pendingDescriptorHeaps.clear();
    auto &executedBundles = _impl->executedBundles[_impl->activeRecordingContext];
    if (!executedBundles.empty()) {
        executedBundles.clear();
    }

    HRESULT hr = _impl->commandAllocator->Reset();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12CommandBuffer::begin - allocator reset failed. HRESULT=0x%08x. "
                      "This typically means the GPU is still executing the previous command list. "
                      "The Queue::submit fence wait should prevent this.", static_cast<unsigned>(hr));
        // Do NOT continue recording commands with a stale allocator — it would corrupt GPU state.
        return;
    }

    hr = _impl->commandList->Reset(_impl->commandAllocator.Get(), nullptr);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12CommandBuffer::begin - command list reset failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        return;
    }

    _impl->isRecording = true;
    _impl->activeSwapchain = nullptr;
    _impl->activeRenderPass = nullptr;
    _impl->activeFramebuffer = nullptr;
    _impl->currentSubpass = 0;
    _impl->activeSwapchainBackBuffer = nullptr;
    _impl->activeDepthStencil = nullptr;
    _impl->activeDepthTexture = nullptr;
    _impl->activeColorTargets.clear();
    _impl->inRenderPass = false;
    // waitForFenceValue() above guarantees resources referenced by the previous
    // submission are no longer in flight.
    _impl->pendingUploadResources.clear();
    _impl->pendingUploadResourceSet.clear();
    _impl->pendingDescriptorHeaps.clear();
    // Clear pending descriptor sets
    for (uint32_t i = 0; i < D3D12_MAX_BOUND_SETS; ++i) {
        _impl->pendingSets[i] = {};
    }
    _impl->pendingSetCount = 0;
    _impl->descriptorSetsDirty = false;
    _impl->localDescriptorSetOnlyDirty = false;
    for (auto &cachedSet : _impl->gpuDescriptorCache) {
        cachedSet.reset();
    }
    _impl->samplerTableCache.clear();
    _impl->localStaticCbvSrvUavTableCache.clear();
    _impl->localNullDynamicCbvSrvUavTable = {};
    _impl->samplerTableCacheHeap = nullptr;
    _impl->samplerTableCacheHeapIndex = std::numeric_limits<uint32_t>::max();
    _impl->boundCbvSrvUavHeap = nullptr;
    _impl->boundSamplerHeap = nullptr;
    invalidateGraphicsState();
    _impl->dynamicDepthBias = 0.F;
    _impl->dynamicDepthBiasClamp = 0.F;
    _impl->dynamicDepthBiasSlope = 0.F;
    _impl->dynamicStencilReadMask = 0xFFFFFFFFU;
    _impl->dynamicStencilWriteMask = 0xFFFFFFFFU;
    _impl->hasDynamicDepthBias = false;
    _impl->hasDynamicStencilReadMask = false;
    _impl->hasDynamicStencilWriteMask = false;
    _impl->lastDynamicPipelineStateOwner = nullptr;
    _impl->dynamicPipelineStateValid = false;
    _numDrawCalls = 0;
    _numInstances = 0;
    _numTriangles = 0;
}

void CCD3D12CommandBuffer::end() {
    if (!_impl->commandList) return;

    if (_type == CommandBufferType::PRIMARY) {
        CCD3D12Device::getInstance()->flushPendingBufferUpdates(this);
    }

    HRESULT hr = _impl->commandList->Close();
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12CommandBuffer::end - Close failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        if (_impl->d3dDevice) {
            Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
            if (SUCCEEDED(_impl->d3dDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
                const UINT64 msgCount = infoQueue->GetNumStoredMessages();
                CC_LOG_ERROR("[DIAG-CLOSE] InfoQueue pending messages: %llu",
                             static_cast<unsigned long long>(msgCount));
                for (UINT64 i = 0; i < msgCount; ++i) {
                    SIZE_T msgSize = 0;
                    infoQueue->GetMessage(i, nullptr, &msgSize);
                    if (msgSize == 0) {
                        continue;
                    }
                    auto *msgData = static_cast<D3D12_MESSAGE *>(malloc(msgSize));
                    if (!msgData) {
                        continue;
                    }
                    if (SUCCEEDED(infoQueue->GetMessage(i, msgData, &msgSize))) {
                        CC_LOG_ERROR("[DIAG-CLOSE] ID=%u Severity=%u: %.*s",
                                     static_cast<unsigned>(msgData->ID),
                                     static_cast<unsigned>(msgData->Severity),
                                     static_cast<int>(msgData->DescriptionByteLength),
                                     msgData->pDescription);
                    }
                    free(msgData);
                }
                infoQueue->ClearStoredMessages();
            }
        }
    }
    _impl->isRecording = false;
}

void CCD3D12CommandBuffer::transitionColorAttachment(uint32_t attachment, D3D12_RESOURCE_STATES state) {
    auto *framebuffer = _impl->activeFramebuffer;
    if (!framebuffer || attachment >= framebuffer->getColorTextureCount()) {
        CC_LOG_WARNING("D3D12 render pass: color attachment index %u is outside framebuffer color count %u.",
                       attachment, framebuffer ? framebuffer->getColorTextureCount() : 0);
        return;
    }

    auto *resource = static_cast<ID3D12Resource *>(framebuffer->getColorResource(attachment));
    if (!resource) {
        return;
    }

    auto *texture = framebuffer->getColorTexture(attachment);
    const bool hasTextureState = texture && framebuffer->hasColorTextureState(attachment);
    const D3D12_RESOURCE_STATES fallback =
        texture && texture->isSwapchainColorTexture()
            ? D3D12_RESOURCE_STATE_RENDER_TARGET
            : D3D12_RESOURCE_STATE_COMMON;
    const D3D12_RESOURCE_STATES previousState =
        hasTextureState
            ? texture->getCurrentState()
            : CCD3D12Texture::getTrackedResourceState(resource, fallback);
    if (previousState == state) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = previousState;
    barrier.Transition.StateAfter = state;
    _impl->commandList->ResourceBarrier(1, &barrier);

    if (hasTextureState) {
        texture->setCurrentState(state);
    } else {
        CCD3D12Texture::setTrackedResourceState(resource, state);
    }
}

void CCD3D12CommandBuffer::bindSubpassRenderTargets(uint32_t subpassIndex) {
    auto *framebuffer = _impl->activeFramebuffer;
    if (!framebuffer) {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[MAX_ATTACHMENTS]{};
    uint32_t rtvCount = 0;
    auto appendColor = [&](uint32_t attachment) {
        if (rtvCount >= MAX_ATTACHMENTS) {
            CC_LOG_WARNING("D3D12 render pass: too many color attachments; truncating to %u.", MAX_ATTACHMENTS);
            return;
        }
        transitionColorAttachment(attachment, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto handle = framebuffer->getRTVHandle(attachment);
        if (handle.ptr == 0) {
            CC_LOG_WARNING("D3D12 render pass: RTV handle[%u] is NULL.", attachment);
            return;
        }
        rtvHandles[rtvCount++].ptr = handle.ptr;
    };

    bool usesDepthStencil = framebuffer->getDSVHandle().ptr != 0;
    const auto &subpasses = _impl->activeRenderPass->getSubpasses();
    if (subpasses.empty()) {
        for (uint32_t attachment = 0; attachment < framebuffer->getColorTextureCount(); ++attachment) {
            appendColor(attachment);
        }
    } else if (subpassIndex < subpasses.size()) {
        const auto &subpass = subpasses[subpassIndex];
        for (uint32_t attachment : subpass.colors) {
            appendColor(attachment);
        }
        usesDepthStencil = usesDepthStencil && subpass.depthStencil != INVALID_BINDING;
    } else {
        CC_LOG_WARNING("D3D12 render pass: subpass %u is outside subpass count %zu.",
                       subpassIndex, subpasses.size());
        usesDepthStencil = false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    dsvHandle.ptr = framebuffer->getDSVHandle().ptr;
    _impl->commandList->OMSetRenderTargets(
        rtvCount,
        rtvCount > 0 ? rtvHandles : nullptr,
        FALSE,
        usesDepthStencil ? &dsvHandle : nullptr);
}

void CCD3D12CommandBuffer::resolveSubpass(uint32_t subpassIndex) {
    if (!_impl->activeRenderPass || !_impl->activeFramebuffer) {
        return;
    }

    const auto &subpasses = _impl->activeRenderPass->getSubpasses();
    if (subpasses.empty() || subpassIndex >= subpasses.size()) {
        return;
    }

    const auto &subpass = subpasses[subpassIndex];
    if (subpass.resolves.empty()) {
        return;
    }

    _impl->commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    auto *framebuffer = _impl->activeFramebuffer;
    for (uint32_t colorIndex = 0; colorIndex < subpass.colors.size(); ++colorIndex) {
        const uint32_t sourceAttachment = subpass.colors[colorIndex];
        uint32_t destinationAttachment = INVALID_BINDING;
        if (sourceAttachment < subpass.resolves.size()) {
            destinationAttachment = subpass.resolves[sourceAttachment];
        } else if (colorIndex < subpass.resolves.size()) {
            destinationAttachment = subpass.resolves[colorIndex];
        }
        if (destinationAttachment == INVALID_BINDING) {
            continue;
        }
        if (sourceAttachment >= framebuffer->getColorTextureCount() ||
            destinationAttachment >= framebuffer->getColorTextureCount()) {
            CC_LOG_WARNING("D3D12 resolve: attachment pair %u -> %u is outside framebuffer color count %u.",
                           sourceAttachment, destinationAttachment, framebuffer->getColorTextureCount());
            continue;
        }

        auto *source = static_cast<ID3D12Resource *>(framebuffer->getColorResource(sourceAttachment));
        auto *destination = static_cast<ID3D12Resource *>(framebuffer->getColorResource(destinationAttachment));
        if (!source || !destination || source == destination) {
            continue;
        }
        retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, source);
        retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, destination);

        const auto sourceDesc = source->GetDesc();
        const auto destinationDesc = destination->GetDesc();
        if (sourceDesc.SampleDesc.Count <= 1 || destinationDesc.SampleDesc.Count != 1) {
            CC_LOG_WARNING("D3D12 resolve skipped invalid sample counts: attachment %u has %u samples, "
                           "attachment %u has %u samples.",
                           sourceAttachment, sourceDesc.SampleDesc.Count,
                           destinationAttachment, destinationDesc.SampleDesc.Count);
            continue;
        }
        if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            destinationDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            CC_LOG_WARNING("D3D12 resolve only supports Texture2D render-pass attachments.");
            continue;
        }

        transitionColorAttachment(sourceAttachment, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        transitionColorAttachment(destinationAttachment, D3D12_RESOURCE_STATE_RESOLVE_DEST);

        const uint32_t layerCount = std::min<uint32_t>(sourceDesc.DepthOrArraySize, destinationDesc.DepthOrArraySize);
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            _impl->commandList->ResolveSubresource(
                destination, layer,
                source, layer,
                sourceDesc.Format);
        }

        transitionColorAttachment(sourceAttachment, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        auto *destinationTexture = framebuffer->getColorTexture(destinationAttachment);
        const D3D12_RESOURCE_STATES destinationState =
            destinationTexture && destinationTexture->isSwapchainColorTexture()
                ? D3D12_RESOURCE_STATE_RENDER_TARGET
                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        transitionColorAttachment(destinationAttachment, destinationState);
    }
}

void CCD3D12CommandBuffer::beginRenderPass(RenderPass *renderPass, Framebuffer *fbo, const Rect &renderArea, const Color *colors, float depth, uint32_t stencil, CommandBuffer *const *secondaryCBs, uint32_t secondaryCBCount) {
    (void)secondaryCBs;
    (void)secondaryCBCount;
    if (!_impl->commandList) return;
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot begin a render pass.");
        return;
    }

    auto *d3d12Fbo = static_cast<CCD3D12Framebuffer *>(fbo);
    if (!d3d12Fbo) {
        CC_LOG_WARNING("D3D12CommandBuffer::beginRenderPass - framebuffer is null.");
        return;
    }

    // Fixed-size stack array for pre-pass barriers: swapchain(1) + colors(8) + depth(1) ≤ 10
    D3D12_RESOURCE_BARRIER prePassBarriers[MAX_PASS_BARRIERS];
    uint32_t prePassBarrierCount = 0;

    // If this framebuffer renders to a swapchain, insert PRESENT → RENDER_TARGET barrier
    CCD3D12Swapchain *swapchain = d3d12Fbo->getSwapchain();
    if (swapchain) {
        auto *backBuffer = static_cast<ID3D12Resource *>(swapchain->getCurrentBackBufferHandle());
        if (backBuffer) {
            retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, backBuffer);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = backBuffer;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (prePassBarrierCount < MAX_PASS_BARRIERS) {
                prePassBarriers[prePassBarrierCount++] = barrier;
            }

            _impl->activeSwapchain = swapchain;
            _impl->activeSwapchainBackBuffer = backBuffer;
        }
    }

    const uint32_t fboWidth = d3d12Fbo->getWidth();
    const uint32_t fboHeight = d3d12Fbo->getHeight();
    _impl->activeRenderPass = renderPass;
    _impl->activeFramebuffer = d3d12Fbo;
    _impl->currentSubpass = 0;

    // Count color attachments from the D3D12 framebuffer cache. Repaired
    // offscreen targets may only have a cached ID3D12Resource, not a live
    // CCD3D12Texture actor.
    const uint32_t colorCount = d3d12Fbo->getColorTextureCount();

    // Transition non-swapchain color attachments to RENDER_TARGET
    _impl->activeColorTargets.clear();
    for (uint32_t i = 0; i < colorCount; ++i) {
        auto *d3d12Tex = d3d12Fbo->getColorTexture(i);
        auto *resource = static_cast<ID3D12Resource *>(d3d12Fbo->getColorResource(i));
        if (!resource) continue;
        retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, resource);

        const bool hasTextureState = d3d12Tex && d3d12Fbo->hasColorTextureState(i);
        _impl->activeColorTargets.push_back({resource, d3d12Tex, hasTextureState});

        if (d3d12Tex && d3d12Tex->isSwapchainColorTexture()) continue;

        D3D12_RESOURCE_STATES prevState = hasTextureState
                                               ? d3d12Tex->getCurrentState()
                                               : CCD3D12Texture::getTrackedResourceState(resource, D3D12_RESOURCE_STATE_COMMON);
        if (prevState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = prevState;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (prePassBarrierCount < MAX_PASS_BARRIERS) {
                prePassBarriers[prePassBarrierCount++] = barrier;
            }
            if (hasTextureState) {
                d3d12Tex->setCurrentState(D3D12_RESOURCE_STATE_RENDER_TARGET);
            } else {
                CCD3D12Texture::setTrackedResourceState(resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
    }

    // Collect RTV handles
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[MAX_ATTACHMENTS]{};
    for (uint32_t i = 0; i < colorCount && i < MAX_ATTACHMENTS; ++i) {
        auto handle = d3d12Fbo->getRTVHandle(i);
        rtvHandles[i].ptr = handle.ptr;
        if (handle.ptr == 0) {
            CC_LOG_WARNING("D3D12 beginRenderPass: RTV handle[%u] is NULL (swapchain=%s, colorCount=%u). "
                           "Off-screen RT will not be bound!",
                           i, swapchain ? "yes" : "no", colorCount);
        }
    }

    // Get DSV handle
    auto dsvPair = d3d12Fbo->getDSVHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    dsvHandle.ptr = dsvPair.ptr;
    bool hasDSV = (dsvPair.ptr != 0);
    auto *depthStencilTexture = d3d12Fbo->getDepthStencilTexture();
    auto *depthStencilResource = static_cast<ID3D12Resource *>(d3d12Fbo->getDepthStencilResource());
    if (hasDSV && depthStencilResource) {
        retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, depthStencilResource);
        D3D12_RESOURCE_STATES dsPrevState = depthStencilTexture ? depthStencilTexture->getCurrentState() : D3D12_RESOURCE_STATE_COMMON;
        if (dsPrevState != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            D3D12_RESOURCE_BARRIER depthBarrier{};
            depthBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            depthBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            depthBarrier.Transition.pResource = depthStencilResource;
            depthBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            depthBarrier.Transition.StateBefore = dsPrevState;
            depthBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            if (prePassBarrierCount < MAX_PASS_BARRIERS) {
                prePassBarriers[prePassBarrierCount++] = depthBarrier;
            }
        }
        if (depthStencilTexture) {
            depthStencilTexture->setCurrentState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
        _impl->activeDepthStencil = depthStencilResource;
        _impl->activeDepthTexture = depthStencilTexture;
    } else {
        _impl->activeDepthStencil = nullptr;
        _impl->activeDepthTexture = nullptr;
    }

    // Submit all pre-pass barriers at once
    if (prePassBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(prePassBarrierCount, prePassBarriers);
    }

    // Resolve attachments are part of the framebuffer but are not regular MRTs.
    // Bind only the color attachments declared by the first subpass.
    bindSubpassRenderTargets(0);

    const D3D12_RECT safeRenderArea = makeSafeRenderAreaRect(renderArea, fboWidth, fboHeight);
    const bool hasSafeRenderArea = safeRenderArea.right > safeRenderArea.left &&
                                   safeRenderArea.bottom > safeRenderArea.top;
    const uint32_t clearRectCount = hasSafeRenderArea ? 1U : 0U;
    const D3D12_RECT *clearRects = hasSafeRenderArea ? &safeRenderArea : nullptr;

    // Clear render targets based on loadOp from RenderPass
    // CRITICAL: Only clear when loadOp == CLEAR. For LOAD, preserve existing content.
    // This allows multiple render passes to share the same RT (e.g. 3D scene + UI overlay).
    const auto &rpColorAttachments = renderPass ? renderPass->getColorAttachments() : ColorAttachmentList();
    for (uint32_t i = 0; i < colorCount; ++i) {
        if (rtvHandles[i].ptr == 0) continue;

        // Determine loadOp for this attachment
        LoadOp loadOp = LoadOp::CLEAR; // default: clear if no RenderPass info
        if (i < rpColorAttachments.size()) {
            loadOp = rpColorAttachments[i].loadOp;
        }

        if (loadOp == LoadOp::CLEAR && colors && hasSafeRenderArea) {
            float clearColor[4] = {colors[i].x, colors[i].y, colors[i].z, colors[i].w};
            _impl->commandList->ClearRenderTargetView(rtvHandles[i], clearColor, clearRectCount, clearRects);
        }
        // LoadOp::LOAD: do nothing, preserve existing content
        // LoadOp::DISCARD: do nothing, D3D12 DISCARD optimization could be added later
    }

    // Clear depth-stencil based on depthLoadOp/stencilLoadOp from RenderPass
    if (hasDSV) {
        LoadOp depthLoadOp = LoadOp::CLEAR;
        LoadOp stencilLoadOp = LoadOp::CLEAR;
        Format dsFormat = depthStencilTexture ? depthStencilTexture->getFormat() : Format::UNKNOWN;
        if (renderPass) {
            const auto &dsAttachment = renderPass->getDepthStencilAttachment();
            dsFormat = dsAttachment.format;
            depthLoadOp = dsAttachment.depthLoadOp;
            stencilLoadOp = dsAttachment.stencilLoadOp;
        }

        D3D12_CLEAR_FLAGS clearFlags = static_cast<D3D12_CLEAR_FLAGS>(0);
        if (depthLoadOp == LoadOp::CLEAR && hasDepthComponent(dsFormat)) {
            clearFlags |= D3D12_CLEAR_FLAG_DEPTH;
        }
        if (stencilLoadOp == LoadOp::CLEAR && hasStencilComponent(dsFormat)) {
            clearFlags |= D3D12_CLEAR_FLAG_STENCIL;
        }

        if (clearFlags != 0 && hasSafeRenderArea) {
            _impl->commandList->ClearDepthStencilView(dsvHandle, clearFlags, depth, stencil, clearRectCount, clearRects);
        }
    }

    if (renderPass && !renderPass->getSubpasses().empty()) {
        const auto &firstSubpass = renderPass->getSubpasses()[0];
        for (uint32_t input : firstSubpass.inputs) {
            const bool isColorOutput =
                std::find(firstSubpass.colors.begin(), firstSubpass.colors.end(), input) != firstSubpass.colors.end();
            if (!isColorOutput) {
                transitionColorAttachment(input, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
    }

    // Set viewport from render area
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(safeRenderArea.left);
    vp.TopLeftY = static_cast<float>(safeRenderArea.top);
    vp.Width = static_cast<float>(safeRenderArea.right - safeRenderArea.left);
    vp.Height = static_cast<float>(safeRenderArea.bottom - safeRenderArea.top);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    _impl->commandList->RSSetViewports(1, &vp);
    _impl->boundViewport = vp;
    _impl->viewportValid = true;

    _impl->commandList->RSSetScissorRects(1, &safeRenderArea);
    _impl->boundScissor = safeRenderArea;
    _impl->scissorValid = true;

    _impl->inRenderPass = true;
}

void CCD3D12CommandBuffer::endRenderPass() {
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot end a render pass.");
        return;
    }

    resolveSubpass(_impl->currentSubpass);
    _impl->commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    D3D12_RESOURCE_BARRIER postPassBarriers[MAX_PASS_BARRIERS];
    uint32_t postPassBarrierCount = 0;

    // If we transitioned a swapchain back buffer to RENDER_TARGET, transition it back to PRESENT
    if (_impl->activeSwapchainBackBuffer) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = _impl->activeSwapchainBackBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        if (postPassBarrierCount < MAX_PASS_BARRIERS) {
            postPassBarriers[postPassBarrierCount++] = barrier;
        }

        _impl->activeSwapchain = nullptr;
        _impl->activeSwapchainBackBuffer = nullptr;
    }

    // Transition non-swapchain color attachments from RENDER_TARGET to SHADER_RESOURCE
    // (the next pass will likely read them as textures)
    for (const auto &target : _impl->activeColorTargets) {
        auto *d3d12Tex = target.texture;
        if (d3d12Tex && d3d12Tex->isSwapchainColorTexture()) continue;
        auto *resource = target.resource;
        if (!resource) continue;
        const D3D12_RESOURCE_STATES trackedState =
            CCD3D12Texture::getTrackedResourceState(resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const bool shouldTransition = target.hasTextureState
                                          ? (d3d12Tex && d3d12Tex->getCurrentState() == D3D12_RESOURCE_STATE_RENDER_TARGET)
                                          : (trackedState == D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (shouldTransition) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if (postPassBarrierCount < MAX_PASS_BARRIERS) {
                postPassBarriers[postPassBarrierCount++] = barrier;
            }
            if (target.hasTextureState && d3d12Tex) {
                d3d12Tex->setCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            } else {
                CCD3D12Texture::setTrackedResourceState(resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
    }
    _impl->activeColorTargets.clear();

    // Transition depth-stencil back from DEPTH_WRITE
    // Use DEPTH_READ | PIXEL_SHADER_RESOURCE so that depth textures used as
    // shadow maps can be sampled as SRVs in subsequent passes (e.g. forward pass
    // reading the shadow map).  In Vulkan/GLES this is handled by explicit
    // pipeline barriers, but D3D12 endRenderPass must set the correct combined state.
    if (_impl->activeDepthStencil) {
        if (_impl->activeDepthTexture && _impl->activeDepthTexture->getCurrentState() == D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            D3D12_RESOURCE_BARRIER depthBarrier{};
            depthBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            depthBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            depthBarrier.Transition.pResource = _impl->activeDepthStencil;
            depthBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            depthBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            depthBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if (postPassBarrierCount < MAX_PASS_BARRIERS) {
                postPassBarriers[postPassBarrierCount++] = depthBarrier;
            }
            _impl->activeDepthTexture->setCurrentState(D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        _impl->activeDepthStencil = nullptr;
        _impl->activeDepthTexture = nullptr;
    }

    if (postPassBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(postPassBarrierCount, postPassBarriers);
    }

    _impl->inRenderPass = false;
    _impl->activeRenderPass = nullptr;
    _impl->activeFramebuffer = nullptr;
    _impl->currentSubpass = 0;
    // D3D12 has no explicit endRenderPass beyond resource barriers
}

void CCD3D12CommandBuffer::insertMarker(const MarkerInfo &marker) {
    if (!_impl->commandList || marker.name.empty()) return;
    _impl->commandList->SetMarker(0, marker.name.c_str(), static_cast<UINT>(marker.name.size() + 1));
}

void CCD3D12CommandBuffer::beginMarker(const MarkerInfo &marker) {
    if (!_impl->commandList || marker.name.empty()) return;
    _impl->commandList->BeginEvent(0, marker.name.c_str(), static_cast<UINT>(marker.name.size() + 1));
}

void CCD3D12CommandBuffer::endMarker() {
    if (!_impl->commandList) return;
    _impl->commandList->EndEvent();
}

void CCD3D12CommandBuffer::execute(CommandBuffer *const *cmdBuffs, uint32_t count) {
    if (!_impl->commandList || !cmdBuffs || count == 0) return;
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundles cannot execute nested command buffers.");
        return;
    }

    CCD3D12Device::getInstance()->flushPendingBufferUpdates(this);

    for (uint32_t i = 0; i < count; ++i) {
        if (!cmdBuffs[i]) continue;

        auto *d3d12CmdBuff = static_cast<CCD3D12CommandBuffer *>(cmdBuffs[i]);
        if (d3d12CmdBuff->getType() != CommandBufferType::SECONDARY) {
            CC_LOG_WARNING("D3D12CommandBuffer::execute skipped non-secondary command buffer at index %u.", i);
            continue;
        }

        auto *bundle = static_cast<ID3D12GraphicsCommandList *>(d3d12CmdBuff->getD3D12CommandList());
        if (!bundle) continue;

        ID3D12DescriptorHeap *bundleHeaps[2]{};
        UINT bundleHeapCount = 0;
        if (d3d12CmdBuff->_impl->boundCbvSrvUavHeap) {
            bundleHeaps[bundleHeapCount++] = d3d12CmdBuff->_impl->boundCbvSrvUavHeap;
        }
        if (d3d12CmdBuff->_impl->boundSamplerHeap) {
            bundleHeaps[bundleHeapCount++] = d3d12CmdBuff->_impl->boundSamplerHeap;
        }
        if (bundleHeapCount > 0) {
            _impl->commandList->SetDescriptorHeaps(bundleHeapCount, bundleHeaps);
            _impl->boundCbvSrvUavHeap = d3d12CmdBuff->_impl->boundCbvSrvUavHeap;
            _impl->boundSamplerHeap = d3d12CmdBuff->_impl->boundSamplerHeap;
        }

        _impl->commandList->ExecuteBundle(bundle);
        _impl->executedBundles[_impl->activeRecordingContext].emplace_back(d3d12CmdBuff);
        // A bundle may change any graphics state. Do not let the primary
        // command buffer's cache suppress the next explicit bind.
        invalidateGraphicsState();
        _numDrawCalls += d3d12CmdBuff->getNumDrawCalls();
        _numInstances += d3d12CmdBuff->getNumInstances();
        _numTriangles += d3d12CmdBuff->getNumTris();
    }
}

void CCD3D12CommandBuffer::bindPipelineState(PipelineState *pso) {
    if (!_impl->commandList || !pso) return;

    if (_impl->localRootCbvBatchActive && _impl->boundPipelineState != pso) {
        flushLocalRootCbvBatch();
    }

    const bool logicalPipelineChanged = _impl->boundPipelineState != pso;
    if (!logicalPipelineChanged) {
        // Pipeline, root signature, blend constants, stencil reference, and topology
        // remain valid for the same logical PSO. Dynamic state still needs its
        // normal update path because it may have been changed between draws.
        applyDynamicPipelineState();
        return;
    }
    auto *d3d12PSO = static_cast<CCD3D12PipelineState *>(pso);
    auto *d3d12PipelineState = static_cast<ID3D12PipelineState *>(d3d12PSO->getID3D12PipelineState());
    if (!d3d12PipelineState) {
        CC_LOG_WARNING("D3D12CommandBuffer::bindPipelineState - PSO handle is null.");
        return;
    }

    if (logicalPipelineChanged && _impl->boundNativePipelineState != d3d12PipelineState) {
        _impl->commandList->SetPipelineState(d3d12PipelineState);
        _impl->boundNativePipelineState = d3d12PipelineState;
    }

    // Apply blend constants from PSO's BlendState.
    // This matches WebGL backend behavior (gl.blendColor) and is required
    // when any target uses CONSTANT_COLOR / CONSTANT_ALPHA blend factors.
    {
        const auto &bs = pso->getBlendState();
        float blendFactor[4] = { bs.blendColor.x, bs.blendColor.y, bs.blendColor.z, bs.blendColor.w };
        if (!_impl->blendFactorValid || std::memcmp(_impl->boundBlendFactor, blendFactor, sizeof(blendFactor)) != 0) {
            _impl->commandList->OMSetBlendFactor(blendFactor);
            std::memcpy(_impl->boundBlendFactor, blendFactor, sizeof(blendFactor));
            _impl->blendFactorValid = true;
        }
    }

    // D3D12 treats stencil reference as dynamic command-list state.
    // Passes such as planar-shadow rely on stencilRefFront being active.
    {
        const auto &ds = pso->getDepthStencilState();
        const uint32_t stencilRef = ds.stencilTestFront ? ds.stencilRefFront : ds.stencilRefBack;
        if (!_impl->stencilRefValid || _impl->boundStencilRef != stencilRef) {
            _impl->commandList->OMSetStencilRef(stencilRef);
            _impl->boundStencilRef = stencilRef;
            _impl->stencilRefValid = true;
        }
    }

    // Set primitive topology from PSO
    D3D12_PRIMITIVE_TOPOLOGY topology = static_cast<D3D12_PRIMITIVE_TOPOLOGY>(d3d12PSO->getD3D12PrimitiveTopology());
    if (!_impl->primitiveTopologyValid || _impl->boundPrimitiveTopology != topology) {
        _impl->commandList->IASetPrimitiveTopology(topology);
        _impl->boundPrimitiveTopology = topology;
        _impl->primitiveTopologyValid = true;
    }

    auto *rootSig = static_cast<ID3D12RootSignature *>(d3d12PSO->getID3D12RootSignature());
    if (rootSig && _impl->boundRootSignature != rootSig) {
        _impl->commandList->SetGraphicsRootSignature(rootSig);
        _impl->boundRootSignature = rootSig;
        // Setting a graphics root signature invalidates all root arguments.
        invalidateDescriptorTables();
    }

    auto *pipelineLayout = pso->getPipelineLayout();
    PipelineLayout *newPipelineLayout = nullptr;
    if (pipelineLayout && d3d12PSO->usesPipelineLayoutRootSignature()) {
        newPipelineLayout = const_cast<PipelineLayout *>(pipelineLayout);
    }
    if (_impl->boundPipelineLayout != newPipelineLayout) {
        _impl->boundPipelineLayout = newPipelineLayout;
        if (_impl->pendingSetCount > 0) {
            _impl->descriptorSetsDirty = true;
            _impl->localDescriptorSetOnlyDirty = false;
        }
    }

    _impl->boundPipelineState = pso;
    if (logicalPipelineChanged) {
        _impl->dynamicPipelineStateValid = false;
    }
    applyDynamicPipelineState();
}

void CCD3D12CommandBuffer::applyDynamicPipelineState() {
    if (!_impl->commandList || !_impl->boundPipelineState) {
        return;
    }

    auto *d3d12PSO = static_cast<CCD3D12PipelineState *>(_impl->boundPipelineState);
    const auto dynamicStates = d3d12PSO->getDynamicStates();
    const auto &rasterizer = d3d12PSO->getRasterizerState();
    const auto &depthStencil = d3d12PSO->getDepthStencilState();

    const bool dynamicDepthBias = hasFlag(dynamicStates, DynamicStateFlagBit::DEPTH_BIAS) &&
                                  _impl->hasDynamicDepthBias;
    const bool dynamicStencilReadMask = hasFlag(dynamicStates, DynamicStateFlagBit::STENCIL_COMPARE_MASK) &&
                                        _impl->hasDynamicStencilReadMask;
    const bool dynamicStencilWriteMask = hasFlag(dynamicStates, DynamicStateFlagBit::STENCIL_WRITE_MASK) &&
                                         _impl->hasDynamicStencilWriteMask;
    if (!dynamicDepthBias && !dynamicStencilReadMask && !dynamicStencilWriteMask) {
        return;
    }

    const bool useFrontStencilMask = depthStencil.stencilTestFront || !depthStencil.stencilTestBack;
    const float depthBias = dynamicDepthBias ? _impl->dynamicDepthBias : rasterizer.depthBias;
    const float depthBiasClamp = dynamicDepthBias ? _impl->dynamicDepthBiasClamp : rasterizer.depthBiasClamp;
    const float depthBiasSlope = dynamicDepthBias ? _impl->dynamicDepthBiasSlope : rasterizer.depthBiasSlop;
    const uint32_t stencilReadMask = dynamicStencilReadMask
                                         ? _impl->dynamicStencilReadMask
                                         : (useFrontStencilMask ? depthStencil.stencilReadMaskFront : depthStencil.stencilReadMaskBack);
    const uint32_t stencilWriteMask = dynamicStencilWriteMask
                                          ? _impl->dynamicStencilWriteMask
                                          : (useFrontStencilMask ? depthStencil.stencilWriteMaskFront : depthStencil.stencilWriteMaskBack);

    if (_impl->dynamicPipelineStateValid &&
        _impl->lastDynamicPipelineStateOwner == _impl->boundPipelineState &&
        _impl->lastDynamicDepthBias == depthBias &&
        _impl->lastDynamicDepthBiasClamp == depthBiasClamp &&
        _impl->lastDynamicDepthBiasSlope == depthBiasSlope &&
        _impl->lastDynamicStencilReadMask == stencilReadMask &&
        _impl->lastDynamicStencilWriteMask == stencilWriteMask) {
        return;
    }

    auto *variant = static_cast<ID3D12PipelineState *>(
        d3d12PSO->getDynamicID3D12PipelineState(depthBias, depthBiasClamp, depthBiasSlope,
                                                stencilReadMask, stencilWriteMask));
    if (variant) {
        if (_impl->boundNativePipelineState != variant) {
            _impl->commandList->SetPipelineState(variant);
            _impl->boundNativePipelineState = variant;
        }
        _impl->lastDynamicPipelineStateOwner = _impl->boundPipelineState;
        _impl->lastDynamicDepthBias = depthBias;
        _impl->lastDynamicDepthBiasClamp = depthBiasClamp;
        _impl->lastDynamicDepthBiasSlope = depthBiasSlope;
        _impl->lastDynamicStencilReadMask = stencilReadMask;
        _impl->lastDynamicStencilWriteMask = stencilWriteMask;
        _impl->dynamicPipelineStateValid = true;
    }
}

void CCD3D12CommandBuffer::bindDescriptorSet(uint32_t set, DescriptorSet *descriptorSet, uint32_t dynamicOffsetCount, const uint32_t *dynamicOffsets) {
    if (!_impl->commandList || !descriptorSet) return;
    if (set >= D3D12_MAX_BOUND_SETS) {
        CC_LOG_ERROR("D3D12CommandBuffer::bindDescriptorSet set index %u exceeds the supported maximum %u.",
                     set, D3D12_MAX_BOUND_SETS);
        return;
    }
    if (_impl->localRootCbvBatchActive && set != D3D12_LOCAL_DESCRIPTOR_SET_INDEX) {
        flushLocalRootCbvBatch();
    }

    // Defer the actual GPU binding until draw time.
    // D3D12 only allows one CBV/SRV/UAV heap and one Sampler heap bound at a time,
    // so we must collect all sets and flush them together before each draw call.
    auto *d3d12Set = static_cast<CCD3D12DescriptorSet *>(descriptorSet);
    // The draw/dispatch path flushes pending buffer uploads before it updates
    // descriptor sets. Updating here would scan the same slots once before
    // their final transient-CBV versions are available and again at flush.
    const uint64_t version = d3d12Set->getVersion();
    const auto sameDynamicOffsets = [&](const ccstd::vector<uint32_t> &current) {
        if (current.size() != dynamicOffsetCount) {
            return false;
        }
        return dynamicOffsetCount == 0 ||
               (dynamicOffsets && std::equal(current.begin(), current.end(), dynamicOffsets));
    };

    // Store in pending list (replace if same set index already recorded)
    bool replaced = false;
    for (uint32_t i = 0; i < _impl->pendingSetCount; ++i) {
        if (_impl->pendingSets[i].valid && _impl->pendingSets[i].setIndex == set) {
            if (_impl->pendingSets[i].set == descriptorSet &&
                _impl->pendingSets[i].version == version &&
                sameDynamicOffsets(_impl->pendingSets[i].dynamicOffsets)) {
                return;
            }
            _impl->pendingSets[i].set = descriptorSet;
            _impl->pendingSets[i].version = version;
            _impl->pendingSets[i].dynamicOffsets.clear();
            if (dynamicOffsetCount > 0 && dynamicOffsets) {
                _impl->pendingSets[i].dynamicOffsets.assign(dynamicOffsets, dynamicOffsets + dynamicOffsetCount);
            }
            replaced = true;
            break;
        }
    }
    if (!replaced && _impl->pendingSetCount < D3D12_MAX_BOUND_SETS) {
        auto &pendingSet = _impl->pendingSets[_impl->pendingSetCount];
        pendingSet.set = descriptorSet;
        pendingSet.setIndex = set;
        pendingSet.version = version;
        pendingSet.dynamicOffsets.clear();
        if (dynamicOffsetCount > 0 && dynamicOffsets) {
            pendingSet.dynamicOffsets.assign(dynamicOffsets, dynamicOffsets + dynamicOffsetCount);
        }
        pendingSet.valid = true;
        ++_impl->pendingSetCount;
    }
    _impl->descriptorSetsDirty = true;
    _impl->localDescriptorSetOnlyDirty = set == D3D12_LOCAL_DESCRIPTOR_SET_INDEX;
}

bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental() {
    if (!_impl->descriptorSetsDirty || !_impl->commandList) {
        return true;
    }

    auto *device = CCD3D12Device::getInstance();
    auto *boundLayout = static_cast<CCD3D12PipelineLayout *>(_impl->boundPipelineLayout);
    if (!device || !boundLayout) {
        return false;
    }

    // Steady local draws change only b0's GPU virtual address. Once the
    // global/material tables and the local static/null-dynamic tables are
    // already bound, avoid rebuilding the complete three-set descriptor view.
    // A cache miss deliberately falls through to the conservative full path.
    auto tryFlushLocalRootCbvOnly = [&]() {
        if (!_impl->localDescriptorSetOnlyDirty) {
            return false;
        }

        auto *localPending = static_cast<Impl::PendingDescriptorSet *>(nullptr);
        for (uint32_t i = 0; i < _impl->pendingSetCount; ++i) {
            auto &pending = _impl->pendingSets[i];
            if (pending.valid && pending.set && pending.setIndex == D3D12_LOCAL_DESCRIPTOR_SET_INDEX) {
                localPending = &pending;
                break;
            }
        }
        if (!localPending) {
            return false;
        }

        auto *set = static_cast<CCD3D12DescriptorSet *>(localPending->set);
        const int32_t rootCbvIndex = boundLayout->getLocalRootCbvParameterIndex(D3D12_LOCAL_DESCRIPTOR_SET_INDEX);
        const int32_t dynamicRootIndex = boundLayout->getDynamicCbvSrvUavRootParameterIndex(D3D12_LOCAL_DESCRIPTOR_SET_INDEX);
        const int32_t staticRootIndex = boundLayout->getStaticCbvSrvUavRootParameterIndex(D3D12_LOCAL_DESCRIPTOR_SET_INDEX);
        const int32_t samplerRootIndex = boundLayout->getSamplerRootParameterIndex(D3D12_LOCAL_DESCRIPTOR_SET_INDEX);
        if (!set || rootCbvIndex < 0 || dynamicRootIndex < 0 || staticRootIndex < 0) {
            return false;
        }

        set->updateForLocalRootCbv(true);
        uint32_t rootCbvDescriptorOffset = 0;
        uint32_t staticCbvSrvUavCount = 0;
        if (!set->getCbvSrvUavPartition(rootCbvDescriptorOffset, staticCbvSrvUavCount) ||
            !set->hasOnlyNullDynamicDescriptorSources()) {
            return false;
        }
        const uint32_t dynamicDescriptorCount = set->getDynamicDescriptorSlotCount();
        const auto &cachedDynamic = _impl->localNullDynamicCbvSrvUavTable;
        if (dynamicDescriptorCount == 0 || cachedDynamic.layout != set->getLayout() ||
            cachedDynamic.descriptorCount != dynamicDescriptorCount || !cachedDynamic.heap ||
            _impl->boundCbvSrvUavHeap != cachedDynamic.heap) {
            return false;
        }

        uint32_t dynamicCbvDescriptors = 0;
        uint32_t staticTextureDescriptors = 0;
        uint32_t staticCbvSrvUavDescriptors = 0;
        uint64_t staticSignature = 0;
        set->getStaticDescriptorAnalysis(dynamicCbvDescriptors, staticTextureDescriptors,
                                         staticCbvSrvUavDescriptors, staticSignature);
        if (dynamicCbvDescriptors != 1 || staticCbvSrvUavDescriptors != staticCbvSrvUavCount ||
            staticSignature == 0 || !set->canReuseStaticCbvSrvUavResources()) {
            return false;
        }

        const Impl::CachedLocalStaticCbvSrvUavTable *cachedStatic = nullptr;
        const auto staticBucket = _impl->localStaticCbvSrvUavTableCache.find(staticSignature);
        if (staticBucket != _impl->localStaticCbvSrvUavTableCache.end()) {
            for (const auto &entry : staticBucket->second) {
                if (entry.heap == cachedDynamic.heap && entry.heapIndex == cachedDynamic.heapIndex &&
                    entry.descriptorCount == staticCbvSrvUavCount && entry.owner &&
                    entry.owner->hasMatchingStaticCbvSrvUavResources(*set)) {
                    cachedStatic = &entry;
                    break;
                }
            }
        }
        if (!cachedStatic) {
            return false;
        }

        const Impl::CachedSamplerTable *cachedSampler = nullptr;
        const uint32_t samplerDescriptorCount = set->getSamplerDescriptorCount();
        if (samplerDescriptorCount > 0) {
            if (samplerRootIndex < 0 || !_impl->samplerTableCacheHeap ||
                _impl->boundSamplerHeap != _impl->samplerTableCacheHeap) {
                return false;
            }
            const auto &samplerKey = set->getSamplerTableKey();
            const auto samplerBucket = _impl->samplerTableCache.find(hashSamplerTableKey(samplerKey));
            if (samplerBucket == _impl->samplerTableCache.end()) {
                return false;
            }
            for (const auto &entry : samplerBucket->second) {
                if (entry.heap == _impl->samplerTableCacheHeap &&
                    entry.heapIndex == _impl->samplerTableCacheHeapIndex &&
                    entry.descriptorCount == samplerDescriptorCount && entry.key == samplerKey) {
                    cachedSampler = &entry;
                    break;
                }
            }
            if (!cachedSampler) {
                return false;
            }
        }

        auto bindCachedRootTable = [&](int32_t rootIndex, uint64_t gpuHandle) {
            if (rootIndex < 0 || rootIndex >= static_cast<int32_t>(D3D12_MAX_ROOT_PARAMETERS) || gpuHandle == 0) {
                return false;
            }
            auto &bound = _impl->boundRootTables[rootIndex];
            if (!bound.valid || bound.gpuHandle != gpuHandle) {
                _impl->commandList->SetGraphicsRootDescriptorTable(static_cast<UINT>(rootIndex), {gpuHandle});
                bound = {gpuHandle, true};
            }
            return true;
        };
        if (!bindCachedRootTable(dynamicRootIndex, cachedDynamic.gpuHandle) ||
            !bindCachedRootTable(staticRootIndex, cachedStatic->gpuHandle) ||
            (cachedSampler && !bindCachedRootTable(samplerRootIndex, cachedSampler->gpuHandle))) {
            return false;
        }

        uint64_t rootCbvGpuAddress = 0;
        uint32_t rootCbvSize = 0;
        const uint32_t uniformSlotCount = set->getUniformDescriptorSlotCount();
        for (uint32_t slotIndex = 0; slotIndex < uniformSlotCount; ++slotIndex) {
            uint32_t descriptorOffset = 0;
            if (set->getUniformDescriptorSignature(slotIndex, descriptorOffset,
                                                   rootCbvGpuAddress, rootCbvSize) &&
                descriptorOffset == rootCbvDescriptorOffset) {
                break;
            }
        }
        if (rootCbvGpuAddress == 0 || rootCbvSize < 256U) {
            auto *dummyBuffer = device->getDummyBuffer();
            rootCbvGpuAddress = dummyBuffer ? dummyBuffer->getD3D12GPUVirtualAddress() : 0;
        }
        if (rootCbvGpuAddress == 0) {
            return false;
        }
        _impl->commandList->SetGraphicsRootConstantBufferView(
            static_cast<UINT>(rootCbvIndex), rootCbvGpuAddress);
        _impl->descriptorSetsDirty = false;
        _impl->localDescriptorSetOnlyDirty = false;
        return true;
    };
    _impl->localDescriptorSetOnlyDirty = false;

    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    auto *cbvPool = device->getGPUDescriptorHeapPool();
    auto *samplerPool = device->getSamplerDescriptorHeapPool();
    if (!d3dDevice || !cbvPool) {
        return false;
    }

    struct PreparedRange {
        ID3D12DescriptorHeap *heap{nullptr};
        uint64_t gpuHandle{0};
        uint32_t descriptorCount{0};
        uint32_t heapIndex{0};
        bool valid{false};
    };
    struct SetBindingInfo {
        CCD3D12DescriptorSet *set{nullptr};
        Impl::CachedGpuDescriptorSet *cachedSet{nullptr};
        const ccstd::vector<uint32_t> *dynamicOffsets{nullptr};
        uint64_t version{0};
        uint32_t cbvCount{0};
        uint32_t dynamicCbvCount{0};
        uint32_t dynamicDescriptorCount{0};
        uint32_t localRootCbvDescriptorOffset{0};
        uint32_t staticCbvSrvUavCount{0};
        uint32_t samplerCount{0};
        int32_t cbvRootIndex{-1};
        int32_t dynamicCbvRootIndex{-1};
        int32_t localRootCbvRootIndex{-1};
        int32_t staticCbvSrvUavRootIndex{-1};
        int32_t samplerRootIndex{-1};
        bool splitLocalCbvSrvUav{false};
        bool useLocalRootCbv{false};
        uint64_t localRootCbvGpuAddress{0};
        PreparedRange cbvRange;
        PreparedRange dynamicCbvSrvUavRange;
        PreparedRange staticCbvSrvUavRange;
        PreparedRange samplerRange;
    };

    SetBindingInfo bindings[D3D12_MAX_BOUND_SETS];
    uint32_t bindingCount = 0;
    for (uint32_t i = 0; i < _impl->pendingSetCount; ++i) {
        auto &pending = _impl->pendingSets[i];
        if (!pending.valid || !pending.set || pending.setIndex >= D3D12_MAX_BOUND_SETS ||
            bindingCount >= D3D12_MAX_BOUND_SETS) {
            continue;
        }
        auto *set = static_cast<CCD3D12DescriptorSet *>(pending.set);
        const bool localRootCbvLayout = pending.setIndex == D3D12_LOCAL_DESCRIPTOR_SET_INDEX &&
                                        boundLayout->getLocalRootCbvParameterIndex(pending.setIndex) >= 0;
        if (localRootCbvLayout) {
            set->updateForLocalRootCbv(true);
        } else {
            set->update();
        }
        pending.version = set->getVersion();
        auto &cachedSet = _impl->gpuDescriptorCache[pending.setIndex];
        const bool ownerChanged = cachedSet.owner != set;
        if (ownerChanged) {
            cachedSet.reset(set);
        }
        auto &binding = bindings[bindingCount++];
        binding.set = set;
        binding.cachedSet = &cachedSet;
        binding.dynamicOffsets = &pending.dynamicOffsets;
        binding.version = pending.version;
        binding.cbvCount = set->getCbvSrvUavDescriptorCount();
        binding.dynamicDescriptorCount = set->getDynamicDescriptorSlotCount();
        binding.samplerCount = set->getSamplerDescriptorCount();
        binding.cbvRootIndex = boundLayout->getCbvSrvUavRootParameterIndex(pending.setIndex);
        binding.dynamicCbvRootIndex = boundLayout->getDynamicCbvSrvUavRootParameterIndex(pending.setIndex);
        binding.localRootCbvRootIndex = boundLayout->getLocalRootCbvParameterIndex(pending.setIndex);
        binding.staticCbvSrvUavRootIndex = boundLayout->getStaticCbvSrvUavRootParameterIndex(pending.setIndex);
        binding.samplerRootIndex = boundLayout->getSamplerRootParameterIndex(pending.setIndex);
        const bool localCbvSrvUavPartition = set->getCbvSrvUavPartition(
            binding.localRootCbvDescriptorOffset, binding.staticCbvSrvUavCount);
        binding.dynamicCbvCount = localCbvSrvUavPartition ? 1U : 0U;
        binding.splitLocalCbvSrvUav = pending.setIndex == D3D12_LOCAL_DESCRIPTOR_SET_INDEX &&
                                       localCbvSrvUavPartition &&
                                       binding.localRootCbvRootIndex >= 0 &&
                                       binding.dynamicCbvCount == 1 &&
                                       binding.staticCbvSrvUavRootIndex >= 0 &&
                                       (binding.dynamicDescriptorCount == 0 ||
                                        binding.dynamicCbvRootIndex >= 0);
        binding.useLocalRootCbv = binding.splitLocalCbvSrvUav;
    }


    const uint32_t cbvDescriptorSize = cbvPool->getDescriptorSize();
    const uint32_t samplerDescriptorSize = samplerPool ? samplerPool->getDescriptorSize() : 0;
    auto copyRange = [&](SetBindingInfo &binding, bool sampler,
                          const D3D12DescriptorHeapPool::Allocation &allocation,
                          uint32_t descriptorOffset, uint32_t sourceDescriptorOffset,
                          uint32_t count) {
        const uint32_t descriptorSize = sampler ? samplerDescriptorSize : cbvDescriptorSize;
        const uint64_t sourceHandle = sampler ? binding.set->getSamplerCPUDescriptorHandle()
                                              : binding.set->getCbvSrvUavCPUDescriptorHandle();
        if (!allocation.isValid || count == 0 || descriptorSize == 0 || sourceHandle == 0) {
            return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE source{static_cast<SIZE_T>(sourceHandle)};
        source.ptr += static_cast<SIZE_T>(sourceDescriptorOffset) * descriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE destination{
            reinterpret_cast<SIZE_T>(allocation.cpuHandle) +
            static_cast<SIZE_T>(descriptorOffset) * descriptorSize};
        d3dDevice->CopyDescriptorsSimple(
            count, destination, source,
            sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                    : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    };

    auto copyLocalStaticCbvRange = [&](SetBindingInfo &binding,
                                        const D3D12DescriptorHeapPool::Allocation &allocation,
                                        uint32_t descriptorOffset,
                                        uint32_t sourceDescriptorOffset,
                                        uint32_t count) {
        // A shader-visible heap has a CPU handle by design.  For local b1+
        // CBVs, creating the view at that final destination is legal and
        // avoids the otherwise duplicate staging CreateCBV plus descriptor
        // copy.  Non-CBV descriptors still use their staging representation.
        if (!binding.useLocalRootCbv || !binding.set || count == 0) {
            return copyRange(binding, false, allocation, descriptorOffset,
                             sourceDescriptorOffset, count);
        }

        const uint32_t uniformSlotCount = binding.set->getUniformDescriptorSlotCount();
        bool hasDirectCbv = false;
        for (uint32_t relativeOffset = 0; relativeOffset < count; ++relativeOffset) {
            const uint32_t expectedOffset = sourceDescriptorOffset + relativeOffset;
            for (uint32_t slotIndex = 0; slotIndex < uniformSlotCount; ++slotIndex) {
                uint32_t uniformOffset = 0;
                uint64_t gpuAddress = 0;
                uint32_t cbvSize = 0;
                if (!binding.set->getUniformDescriptorSignature(
                        slotIndex, uniformOffset, gpuAddress, cbvSize) ||
                    uniformOffset != expectedOffset) {
                    continue;
                }
                // Preserve the staging copy if an unusual or invalid CBV is
                // encountered.  The ordinary staging path already owns its
                // null/invalid-descriptor semantics.
                if (gpuAddress == 0 || cbvSize < 256U) {
                    return copyRange(binding, false, allocation, descriptorOffset,
                                     sourceDescriptorOffset, count);
                }
                hasDirectCbv = true;
                break;
            }
        }
        if (!hasDirectCbv) {
            return copyRange(binding, false, allocation, descriptorOffset,
                             sourceDescriptorOffset, count);
        }

        uint32_t copiedRangeStart = 0;
        uint32_t copiedRangeCount = 0;
        const auto flushCopiedRange = [&]() {
            if (copiedRangeCount == 0) {
                return true;
            }
            const bool copied = copyRange(binding, false, allocation,
                                          descriptorOffset + copiedRangeStart,
                                          sourceDescriptorOffset + copiedRangeStart,
                                          copiedRangeCount);
            copiedRangeCount = 0;
            return copied;
        };

        for (uint32_t relativeOffset = 0; relativeOffset < count; ++relativeOffset) {
            const uint32_t expectedOffset = sourceDescriptorOffset + relativeOffset;
            bool isUniformCbv = false;
            uint64_t gpuAddress = 0;
            uint32_t cbvSize = 0;
            for (uint32_t slotIndex = 0; slotIndex < uniformSlotCount; ++slotIndex) {
                uint32_t uniformOffset = 0;
                if (binding.set->getUniformDescriptorSignature(
                        slotIndex, uniformOffset, gpuAddress, cbvSize) &&
                    uniformOffset == expectedOffset) {
                    isUniformCbv = true;
                    break;
                }
            }
            if (!isUniformCbv) {
                if (copiedRangeCount == 0) {
                    copiedRangeStart = relativeOffset;
                }
                ++copiedRangeCount;
                continue;
            }
            if (!flushCopiedRange()) {
                return false;
            }
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
            cbvDesc.BufferLocation = gpuAddress;
            cbvDesc.SizeInBytes = cbvSize;
            D3D12_CPU_DESCRIPTOR_HANDLE destination{
                reinterpret_cast<SIZE_T>(allocation.cpuHandle) +
                static_cast<SIZE_T>(descriptorOffset + relativeOffset) * cbvDescriptorSize};
            d3dDevice->CreateConstantBufferView(&cbvDesc, destination);
        }
        return flushCopiedRange();
    };

    auto copyLocalStaticCbvTable = [&](SetBindingInfo &binding,
                                        const D3D12DescriptorHeapPool::Allocation &allocation,
                                        uint32_t descriptorOffset) {
        const uint32_t rootCbvOffset = binding.localRootCbvDescriptorOffset;
        if (rootCbvOffset >= binding.cbvCount ||
            binding.staticCbvSrvUavCount + binding.dynamicDescriptorCount + 1U != binding.cbvCount) {
            return false;
        }
        uint32_t destinationOffset = descriptorOffset;
        uint32_t sourceRangeStart = 0;
        uint32_t sourceRangeCount = 0;
        const auto flushStaticRange = [&]() {
            if (sourceRangeCount == 0) {
                return true;
            }
            const bool copied = copyLocalStaticCbvRange(
                binding, allocation, destinationOffset, sourceRangeStart, sourceRangeCount);
            destinationOffset += sourceRangeCount;
            sourceRangeCount = 0;
            return copied;
        };
        for (uint32_t sourceOffset = 0; sourceOffset < binding.cbvCount; ++sourceOffset) {
            bool excluded = sourceOffset == rootCbvOffset;
            for (uint32_t dynamicIndex = 0;
                 !excluded && dynamicIndex < binding.dynamicDescriptorCount;
                 ++dynamicIndex) {
                uint32_t dynamicOffset = 0;
                excluded = binding.set->getDynamicDescriptorOffset(dynamicIndex, dynamicOffset) &&
                           dynamicOffset == sourceOffset;
            }
            if (excluded) {
                if (!flushStaticRange()) {
                    return false;
                }
                continue;
            }
            if (sourceRangeCount == 0) {
                sourceRangeStart = sourceOffset;
            }
            ++sourceRangeCount;
        }
        return flushStaticRange() &&
               destinationOffset == descriptorOffset + binding.staticCbvSrvUavCount;
    };

    auto copyLocalDynamicCbvSrvUavTable = [&](SetBindingInfo &binding,
                                                const D3D12DescriptorHeapPool::Allocation &allocation,
                                                uint32_t descriptorOffset) {
        uint32_t copiedCount = 0;
        while (copiedCount < binding.dynamicDescriptorCount) {
            uint32_t sourceOffset = 0;
            if (!binding.set->getDynamicDescriptorOffset(copiedCount, sourceOffset)) {
                return false;
            }
            uint32_t rangeCount = 1;
            while (copiedCount + rangeCount < binding.dynamicDescriptorCount) {
                uint32_t nextSourceOffset = 0;
                if (!binding.set->getDynamicDescriptorOffset(copiedCount + rangeCount, nextSourceOffset) ||
                    nextSourceOffset != sourceOffset + rangeCount) {
                    break;
                }
                ++rangeCount;
            }
            if (!copyRange(binding, false, allocation, descriptorOffset + copiedCount,
                           sourceOffset, rangeCount)) {
                return false;
            }
            copiedCount += rangeCount;
        }
        return true;
    };

    auto cachedRangeMatches = [&](const SetBindingInfo &binding, bool sampler) {
        if (!binding.cachedSet) {
            return false;
        }
        const auto &cached = sampler ? binding.cachedSet->sampler : binding.cachedSet->cbvSrvUav;
        if (!cached.valid || cached.version != binding.version ||
            cached.descriptorCount != (sampler ? binding.samplerCount : binding.cbvCount) ||
            !cached.heap) {
            return false;
        }
        if (sampler &&
            (cached.heap != _impl->samplerTableCacheHeap ||
             cached.heapIndex != _impl->samplerTableCacheHeapIndex)) {
            return false;
        }
        return sampler || (binding.dynamicOffsets &&
                           binding.cachedSet->cbvDynamicOffsets == *binding.dynamicOffsets);
    };

    auto findCachedSamplerTable = [&](const ccstd::vector<uint32_t> &samplerTableKey,
                                      bool &hashCollision) -> const Impl::CachedSamplerTable * {
        hashCollision = false;
        if (!_impl->samplerTableCacheHeap) {
            return nullptr;
        }
        const auto bucketIt = _impl->samplerTableCache.find(hashSamplerTableKey(samplerTableKey));
        if (bucketIt == _impl->samplerTableCache.end()) {
            return nullptr;
        }
        for (const auto &entry : bucketIt->second) {
            if (entry.heap == _impl->samplerTableCacheHeap &&
                entry.heapIndex == _impl->samplerTableCacheHeapIndex &&
                entry.key == samplerTableKey) {
                return &entry;
            }
        }
        hashCollision = !bucketIt->second.empty();
        return nullptr;
    };

    auto cacheSamplerTableRange = [&](const ccstd::vector<uint32_t> &samplerTableKey,
                                       const PreparedRange &range) {
        if (!range.valid || !range.heap || samplerTableKey.size() != range.descriptorCount) {
            return;
        }
        if (_impl->samplerTableCacheHeap != range.heap ||
            _impl->samplerTableCacheHeapIndex != range.heapIndex) {
            _impl->samplerTableCache.clear();
            _impl->samplerTableCacheHeap = range.heap;
            _impl->samplerTableCacheHeapIndex = range.heapIndex;
        }
        auto &hashBucket = _impl->samplerTableCache[hashSamplerTableKey(samplerTableKey)];
        for (auto &entry : hashBucket) {
            if (entry.key == samplerTableKey) {
                entry.gpuHandle = range.gpuHandle;
                entry.descriptorCount = range.descriptorCount;
                entry.heapIndex = range.heapIndex;
                entry.heap = range.heap;
                return;
            }
        }
        hashBucket.emplace_back(Impl::CachedSamplerTable{
            samplerTableKey,
            range.gpuHandle,
            range.descriptorCount,
            range.heapIndex,
            range.heap,
        });
    };

    auto findCachedLocalStaticCbvSrvUavTable = [&](const SetBindingInfo &binding,
                                                    uint64_t signature,
                                                    ID3D12DescriptorHeap *heap,
                                                    uint32_t heapIndex) -> const Impl::CachedLocalStaticCbvSrvUavTable * {
        if (!heap || binding.staticCbvSrvUavCount == 0 || !binding.set ||
            !binding.set->canReuseStaticCbvSrvUavResources()) {
            return nullptr;
        }
        const auto bucketIt = _impl->localStaticCbvSrvUavTableCache.find(signature);
        if (bucketIt == _impl->localStaticCbvSrvUavTableCache.end()) {
            return nullptr;
        }
        for (const auto &entry : bucketIt->second) {
            if (entry.heap == heap && entry.heapIndex == heapIndex &&
                entry.descriptorCount == binding.staticCbvSrvUavCount && entry.owner &&
                entry.owner->hasMatchingStaticCbvSrvUavResources(*binding.set)) {
                return &entry;
            }
        }
        return nullptr;
    };

    auto cacheLocalStaticCbvSrvUavTable = [&](const SetBindingInfo &binding,
                                               uint64_t signature,
                                               const PreparedRange &range) {
        if (!range.valid || !range.heap || !binding.set ||
            !binding.set->canReuseStaticCbvSrvUavResources() ||
            range.descriptorCount != binding.staticCbvSrvUavCount) {
            return;
        }
        auto &bucket = _impl->localStaticCbvSrvUavTableCache[signature];
        for (auto &entry : bucket) {
            if (entry.heap == range.heap && entry.heapIndex == range.heapIndex &&
                entry.descriptorCount == range.descriptorCount && entry.owner &&
                entry.owner->hasMatchingStaticCbvSrvUavResources(*binding.set)) {
                entry.gpuHandle = range.gpuHandle;
                entry.owner = binding.set;
                return;
            }
        }
        bucket.emplace_back(Impl::CachedLocalStaticCbvSrvUavTable{
            binding.set, signature, range.gpuHandle, range.descriptorCount,
            range.heapIndex, range.heap,
        });
    };

    auto prepareLocalStaticSplit = [&](SetBindingInfo &binding) {
        if (!binding.splitLocalCbvSrvUav || binding.dynamicCbvCount == 0 ||
            binding.staticCbvSrvUavCount == 0 || !cbvPool) {
            return false;
        }

        if (binding.useLocalRootCbv) {
            const uint32_t rootCbvDescriptorOffset = binding.localRootCbvDescriptorOffset;
            uint32_t cbvSize = 0;
            bool rootCbvFound = false;
            const uint32_t uniformSlotCount = binding.set->getUniformDescriptorSlotCount();
            for (uint32_t slotIndex = 0; slotIndex < uniformSlotCount; ++slotIndex) {
                uint32_t descriptorOffset = 0;
                if (binding.set->getUniformDescriptorSignature(slotIndex, descriptorOffset,
                                                               binding.localRootCbvGpuAddress, cbvSize) &&
                    descriptorOffset == rootCbvDescriptorOffset) {
                    rootCbvFound = true;
                    break;
                }
            }
            if (!rootCbvFound || binding.localRootCbvGpuAddress == 0 || cbvSize < 256U) {
                auto *dummyBuffer = device->getDummyBuffer();
                binding.localRootCbvGpuAddress = dummyBuffer
                                                    ? dummyBuffer->getD3D12GPUVirtualAddress()
                                                    : 0;
            }
            if (binding.localRootCbvGpuAddress == 0) {
                return false;
            }

            uint32_t dynamicCbvDescriptors = 0;
            uint32_t staticTextureDescriptors = 0;
            uint32_t staticCbvSrvUavDescriptors = 0;
            uint64_t staticSignature = 0;
            binding.set->getStaticDescriptorAnalysis(dynamicCbvDescriptors, staticTextureDescriptors,
                                                     staticCbvSrvUavDescriptors, staticSignature);
            if (dynamicCbvDescriptors != binding.dynamicCbvCount ||
                staticCbvSrvUavDescriptors != binding.staticCbvSrvUavCount || staticSignature == 0) {
                return false;
            }

            D3D12DescriptorHeapPool::Allocation dynamicAllocation;
            ID3D12DescriptorHeap *dynamicHeap = nullptr;
            uint32_t dynamicHeapIndex = 0;
            if (binding.dynamicDescriptorCount > 0) {
                const bool allNullDynamicDescriptors = binding.set->hasOnlyNullDynamicDescriptorSources();
                const auto &cachedNullDynamic = _impl->localNullDynamicCbvSrvUavTable;
                if (allNullDynamicDescriptors &&
                    cachedNullDynamic.layout == binding.set->getLayout() &&
                    cachedNullDynamic.descriptorCount == binding.dynamicDescriptorCount &&
                    cachedNullDynamic.heap) {
                    dynamicHeap = cachedNullDynamic.heap;
                    dynamicHeapIndex = cachedNullDynamic.heapIndex;
                    binding.dynamicCbvSrvUavRange = {
                        dynamicHeap, cachedNullDynamic.gpuHandle, binding.dynamicDescriptorCount,
                        dynamicHeapIndex, true};
                } else {
                    dynamicAllocation = cbvPool->allocate(binding.dynamicDescriptorCount);
                    dynamicHeap = dynamicAllocation.isValid
                                      ? static_cast<ID3D12DescriptorHeap *>(cbvPool->getHeap(dynamicAllocation.heapIndex))
                                      : nullptr;
                    if (!dynamicHeap ||
                        !copyLocalDynamicCbvSrvUavTable(binding, dynamicAllocation, 0)) {
                        return false;
                    }
                    dynamicHeapIndex = dynamicAllocation.heapIndex;
                    binding.dynamicCbvSrvUavRange = {
                        dynamicHeap, dynamicAllocation.gpuHandle, binding.dynamicDescriptorCount,
                        dynamicHeapIndex, true};
                    if (allNullDynamicDescriptors) {
                        _impl->localNullDynamicCbvSrvUavTable = {
                            binding.set->getLayout(), dynamicAllocation.gpuHandle,
                            binding.dynamicDescriptorCount, dynamicHeapIndex, dynamicHeap};
                    }
                }
            }

            auto *activeHeap = dynamicHeap ? dynamicHeap
                                            : static_cast<ID3D12DescriptorHeap *>(cbvPool->getHeap(0));
            const uint32_t activeHeapIndex = dynamicHeap ? dynamicHeapIndex : 0;
            if (const auto *cached = findCachedLocalStaticCbvSrvUavTable(
                    binding, staticSignature, activeHeap, activeHeapIndex)) {
                binding.staticCbvSrvUavRange = {
                    cached->heap, cached->gpuHandle, cached->descriptorCount,
                    cached->heapIndex, true};
                return true;
            }

            const auto staticAllocation = cbvPool->allocate(binding.staticCbvSrvUavCount);
            auto *staticHeap = staticAllocation.isValid
                                   ? static_cast<ID3D12DescriptorHeap *>(cbvPool->getHeap(staticAllocation.heapIndex))
                                   : nullptr;
            if (!staticHeap || (dynamicHeap &&
                                (staticHeap != dynamicHeap ||
                                 staticAllocation.heapIndex != dynamicAllocation.heapIndex)) ||
                !copyLocalStaticCbvTable(binding, staticAllocation, 0)) {
                return false;
            }
            binding.staticCbvSrvUavRange = {
                staticHeap, staticAllocation.gpuHandle, binding.staticCbvSrvUavCount,
                staticAllocation.heapIndex, true};
            cacheLocalStaticCbvSrvUavTable(binding, staticSignature, binding.staticCbvSrvUavRange);
            return true;
        }

        const auto dynamicAllocation = cbvPool->allocate(binding.dynamicCbvCount);
        auto *dynamicHeap = dynamicAllocation.isValid
                                ? static_cast<ID3D12DescriptorHeap *>(cbvPool->getHeap(dynamicAllocation.heapIndex))
                                : nullptr;
        if (!dynamicHeap) {
            return false;
        }

        uint32_t dynamicCbvDescriptors = 0;
        uint32_t staticTextureDescriptors = 0;
        uint32_t staticCbvSrvUavDescriptors = 0;
        uint64_t staticSignature = 0;
        binding.set->getStaticDescriptorAnalysis(dynamicCbvDescriptors, staticTextureDescriptors,
                                                 staticCbvSrvUavDescriptors, staticSignature);
        if (dynamicCbvDescriptors != binding.dynamicCbvCount ||
            staticCbvSrvUavDescriptors != binding.staticCbvSrvUavCount || staticSignature == 0) {
            return false;
        }

        binding.dynamicCbvSrvUavRange = {
            dynamicHeap, dynamicAllocation.gpuHandle, binding.dynamicCbvCount,
            dynamicAllocation.heapIndex, true};
        if (const auto *cached = findCachedLocalStaticCbvSrvUavTable(
                binding, staticSignature, dynamicHeap, dynamicAllocation.heapIndex)) {
            binding.staticCbvSrvUavRange = {
                cached->heap, cached->gpuHandle, cached->descriptorCount,
                cached->heapIndex, true};
            if (!copyRange(binding, false, dynamicAllocation, 0, 0, binding.dynamicCbvCount)) {
                return false;
            }
            return true;
        }

        const auto staticAllocation = cbvPool->allocate(binding.staticCbvSrvUavCount);
        auto *staticHeap = staticAllocation.isValid
                               ? static_cast<ID3D12DescriptorHeap *>(cbvPool->getHeap(staticAllocation.heapIndex))
                               : nullptr;
        if (staticHeap == dynamicHeap && staticAllocation.heapIndex == dynamicAllocation.heapIndex) {
            binding.staticCbvSrvUavRange = {
                staticHeap, staticAllocation.gpuHandle, binding.staticCbvSrvUavCount,
                staticAllocation.heapIndex, true};
            if (!copyRange(binding, false, dynamicAllocation, 0, 0, binding.dynamicCbvCount) ||
                !copyLocalStaticCbvRange(binding, staticAllocation, 0, binding.dynamicCbvCount,
                                         binding.staticCbvSrvUavCount)) {
                return false;
            }
        } else {
            // The two tables must be visible through one CBV/SRV/UAV heap. A
            // fresh contiguous allocation is a correctness fallback on overflow.
            const auto combinedAllocation = cbvPool->allocate(binding.cbvCount);
            auto *combinedHeap = combinedAllocation.isValid
                                     ? static_cast<ID3D12DescriptorHeap *>(cbvPool->getHeap(combinedAllocation.heapIndex))
                                     : nullptr;
            if (!combinedHeap ||
                !copyRange(binding, false, combinedAllocation, 0, 0, binding.dynamicCbvCount) ||
                !copyLocalStaticCbvRange(binding, combinedAllocation, binding.dynamicCbvCount,
                                         binding.dynamicCbvCount, binding.staticCbvSrvUavCount)) {
                return false;
            }
            binding.dynamicCbvSrvUavRange = {
                combinedHeap, combinedAllocation.gpuHandle, binding.dynamicCbvCount,
                combinedAllocation.heapIndex, true};
            binding.staticCbvSrvUavRange = {
                combinedHeap,
                combinedAllocation.gpuHandle + static_cast<uint64_t>(binding.dynamicCbvCount) * cbvDescriptorSize,
                binding.staticCbvSrvUavCount, combinedAllocation.heapIndex, true};
        }
        cacheLocalStaticCbvSrvUavTable(binding, staticSignature, binding.staticCbvSrvUavRange);
        return true;
    };

    auto prepareRange = [&](SetBindingInfo &binding, bool sampler) {
        auto *pool = sampler ? samplerPool : cbvPool;
        const uint32_t count = sampler ? binding.samplerCount : binding.cbvCount;
        const int32_t rootIndex = sampler ? binding.samplerRootIndex : binding.cbvRootIndex;
        auto &prepared = sampler ? binding.samplerRange : binding.cbvRange;
        if (!pool || count == 0 || rootIndex < 0) {
            return true;
        }

        if (cachedRangeMatches(binding, sampler)) {
            const auto &cached = sampler ? binding.cachedSet->sampler : binding.cachedSet->cbvSrvUav;
            prepared = {cached.heap, cached.gpuHandle, cached.descriptorCount,
                        cached.heapIndex, true};
            return true;
        }

        const ccstd::vector<uint32_t> *samplerTableKey = nullptr;
        if (sampler) {
            samplerTableKey = &binding.set->getSamplerTableKey();
            bool hashCollision = false;
            if (const auto *shared = findCachedSamplerTable(*samplerTableKey, hashCollision)) {
                prepared = {shared->heap, shared->gpuHandle, shared->descriptorCount,
                            shared->heapIndex, true};
                if (binding.cachedSet) {
                    auto &cached = binding.cachedSet->sampler;
                    cached = {binding.version, shared->gpuHandle, shared->descriptorCount,
                              shared->heapIndex, shared->heap, true};
                }
                return true;
            }
        }


        const auto allocation = pool->allocate(count);
        auto *heap = allocation.isValid
                         ? static_cast<ID3D12DescriptorHeap *>(pool->getHeap(allocation.heapIndex))
                         : nullptr;
        if (!heap || !copyRange(binding, sampler, allocation, 0, 0, count)) {
            return false;
        }
        prepared = {heap, allocation.gpuHandle, count, allocation.heapIndex, true};
        if (binding.cachedSet) {
            auto &cached = sampler ? binding.cachedSet->sampler : binding.cachedSet->cbvSrvUav;
            cached = {binding.version, allocation.gpuHandle, count,
                      allocation.heapIndex, heap, true};
            if (!sampler && binding.dynamicOffsets) {
                binding.cachedSet->cbvDynamicOffsets = *binding.dynamicOffsets;
            }
        }
        if (samplerTableKey) {
            cacheSamplerTableRange(*samplerTableKey, prepared);
        }
        return true;
    };

    for (uint32_t i = 0; i < bindingCount; ++i) {
        auto &binding = bindings[i];
        const bool hasDynamicOffsets = binding.dynamicOffsets && !binding.dynamicOffsets->empty();
        const bool dynamicCbvCacheHit = !binding.splitLocalCbvSrvUav &&
                                        hasDynamicOffsets && cachedRangeMatches(binding, false);
        bool appliedDynamicOffsets = false;
        if (hasDynamicOffsets && !dynamicCbvCacheHit) {
            binding.set->applyDynamicOffsets(
                static_cast<uint32_t>(binding.dynamicOffsets->size()), binding.dynamicOffsets->data());
            appliedDynamicOffsets = true;
        }
        const bool cbvReady = binding.splitLocalCbvSrvUav
                                  ? prepareLocalStaticSplit(binding)
                                  : prepareRange(binding, false);
        const bool samplerReady = prepareRange(binding, true);
        if (appliedDynamicOffsets) {
            binding.set->restoreDynamicOffsetDescriptors();
        }
        if (!cbvReady || !samplerReady) {
            return false;
        }
    }

    auto repackRanges = [&](bool sampler) {
        auto *pool = sampler ? samplerPool : cbvPool;
        const uint32_t descriptorSize = sampler ? samplerDescriptorSize : cbvDescriptorSize;
        uint32_t totalCount = 0;
        for (uint32_t i = 0; i < bindingCount; ++i) {
            const auto &binding = bindings[i];
            if (!sampler && binding.splitLocalCbvSrvUav) {
                totalCount += binding.useLocalRootCbv
                                  ? binding.dynamicDescriptorCount + binding.staticCbvSrvUavCount
                                  : binding.dynamicCbvCount + binding.staticCbvSrvUavCount;
                continue;
            }
            const uint32_t count = sampler ? binding.samplerCount : binding.cbvCount;
            const int32_t rootIndex = sampler ? binding.samplerRootIndex : binding.cbvRootIndex;
            if (count > 0 && rootIndex >= 0) {
                totalCount += count;
            }
        }
        if (totalCount == 0) {
            return true;
        }
        if (!pool || descriptorSize == 0) {
            return false;
        }

        const auto allocation = pool->allocate(totalCount);
        auto *heap = allocation.isValid
                         ? static_cast<ID3D12DescriptorHeap *>(pool->getHeap(allocation.heapIndex))
                         : nullptr;
        if (!heap) {
            return false;
        }

        uint32_t offset = 0;
        for (uint32_t i = 0; i < bindingCount; ++i) {
            auto &binding = bindings[i];
            if (!sampler && binding.splitLocalCbvSrvUav) {
                if (binding.useLocalRootCbv) {
                    const bool hasDynamicOffsets = binding.dynamicOffsets && !binding.dynamicOffsets->empty();
                    if (hasDynamicOffsets) {
                        binding.set->applyDynamicOffsets(
                            static_cast<uint32_t>(binding.dynamicOffsets->size()), binding.dynamicOffsets->data());
                    }
                    const bool copiedDynamic = binding.dynamicDescriptorCount == 0 ||
                                               copyLocalDynamicCbvSrvUavTable(binding, allocation, offset);
                    const bool copiedStatic = copiedDynamic && copyLocalStaticCbvTable(
                        binding, allocation, offset + binding.dynamicDescriptorCount);
                    if (hasDynamicOffsets) {
                        binding.set->restoreDynamicOffsetDescriptors();
                    }
                    if (!copiedStatic) {
                        return false;
                    }
                    if (binding.dynamicDescriptorCount > 0) {
                        binding.dynamicCbvSrvUavRange = {
                            heap,
                            allocation.gpuHandle + static_cast<uint64_t>(offset) * descriptorSize,
                            binding.dynamicDescriptorCount,
                            allocation.heapIndex,
                            true,
                        };
                    }
                    binding.staticCbvSrvUavRange = {
                        heap,
                        allocation.gpuHandle + static_cast<uint64_t>(
                            offset + binding.dynamicDescriptorCount) * descriptorSize,
                        binding.staticCbvSrvUavCount,
                        allocation.heapIndex,
                        true,
                    };
                    uint32_t dynamicCbvDescriptors = 0;
                    uint32_t staticTextureDescriptors = 0;
                    uint32_t staticCbvSrvUavDescriptors = 0;
                    uint64_t staticSignature = 0;
                    binding.set->getStaticDescriptorAnalysis(dynamicCbvDescriptors, staticTextureDescriptors,
                                                             staticCbvSrvUavDescriptors, staticSignature);
                    if (staticSignature != 0 && staticCbvSrvUavDescriptors == binding.staticCbvSrvUavCount) {
                        cacheLocalStaticCbvSrvUavTable(binding, staticSignature, binding.staticCbvSrvUavRange);
                    }
                    offset += binding.dynamicDescriptorCount + binding.staticCbvSrvUavCount;
                    continue;
                }
                const bool hasDynamicOffsets = binding.dynamicOffsets && !binding.dynamicOffsets->empty();
                if (hasDynamicOffsets) {
                    binding.set->applyDynamicOffsets(
                        static_cast<uint32_t>(binding.dynamicOffsets->size()), binding.dynamicOffsets->data());
                }
                const bool copiedDynamic = copyRange(binding, false, allocation, offset, 0,
                                                     binding.dynamicCbvCount);
                const bool copiedStatic = copiedDynamic && copyLocalStaticCbvRange(
                    binding, allocation, offset + binding.dynamicCbvCount,
                    binding.dynamicCbvCount, binding.staticCbvSrvUavCount);
                if (hasDynamicOffsets) {
                    binding.set->restoreDynamicOffsetDescriptors();
                }
                if (!copiedStatic) {
                    return false;
                }
                binding.dynamicCbvSrvUavRange = {
                    heap,
                    allocation.gpuHandle + static_cast<uint64_t>(offset) * descriptorSize,
                    binding.dynamicCbvCount,
                    allocation.heapIndex,
                    true,
                };
                binding.staticCbvSrvUavRange = {
                    heap,
                    allocation.gpuHandle + static_cast<uint64_t>(offset + binding.dynamicCbvCount) * descriptorSize,
                    binding.staticCbvSrvUavCount,
                    allocation.heapIndex,
                    true,
                };
                uint32_t dynamicCbvDescriptors = 0;
                uint32_t staticTextureDescriptors = 0;
                uint32_t staticCbvSrvUavDescriptors = 0;
                uint64_t staticSignature = 0;
                binding.set->getStaticDescriptorAnalysis(dynamicCbvDescriptors, staticTextureDescriptors,
                                                         staticCbvSrvUavDescriptors, staticSignature);
                if (staticSignature != 0 && staticCbvSrvUavDescriptors == binding.staticCbvSrvUavCount) {
                    cacheLocalStaticCbvSrvUavTable(binding, staticSignature, binding.staticCbvSrvUavRange);
                }
                offset += binding.dynamicCbvCount + binding.staticCbvSrvUavCount;
                continue;
            }
            const uint32_t count = sampler ? binding.samplerCount : binding.cbvCount;
            const int32_t rootIndex = sampler ? binding.samplerRootIndex : binding.cbvRootIndex;
            if (count == 0 || rootIndex < 0) {
                continue;
            }
            const bool hasDynamicOffsets = !sampler && binding.dynamicOffsets && !binding.dynamicOffsets->empty();
            if (hasDynamicOffsets) {
                binding.set->applyDynamicOffsets(
                    static_cast<uint32_t>(binding.dynamicOffsets->size()), binding.dynamicOffsets->data());
            }
            const bool copied = copyRange(binding, sampler, allocation, offset, 0, count);
            if (hasDynamicOffsets) {
                binding.set->restoreDynamicOffsetDescriptors();
            }
            if (!copied) {
                return false;
            }

            auto &prepared = sampler ? binding.samplerRange : binding.cbvRange;
            prepared = {
                heap,
                allocation.gpuHandle + static_cast<uint64_t>(offset) * descriptorSize,
                count,
                allocation.heapIndex,
                true,
            };
            if (binding.cachedSet) {
                auto &cached = sampler ? binding.cachedSet->sampler : binding.cachedSet->cbvSrvUav;
                cached = {binding.version, prepared.gpuHandle, count,
                          allocation.heapIndex, heap, true};
                if (!sampler && binding.dynamicOffsets) {
                    binding.cachedSet->cbvDynamicOffsets = *binding.dynamicOffsets;
                }
            }
            if (sampler) {
                cacheSamplerTableRange(binding.set->getSamplerTableKey(), prepared);
            }
            offset += count;
        }
        return true;
    };

    auto rangesUseSingleHeap = [&](bool sampler) {
        ID3D12DescriptorHeap *heap = nullptr;
        for (uint32_t i = 0; i < bindingCount; ++i) {
            const auto checkRange = [&](const PreparedRange &range) {
                if (!range.valid) {
                    return true;
                }
                if (!heap) {
                    heap = range.heap;
                    return true;
                }
                return heap == range.heap;
            };
            if (sampler || !bindings[i].splitLocalCbvSrvUav) {
                if (!checkRange(sampler ? bindings[i].samplerRange : bindings[i].cbvRange)) {
                    return false;
                }
            } else if (!checkRange(bindings[i].dynamicCbvSrvUavRange) ||
                       !checkRange(bindings[i].staticCbvSrvUavRange)) {
                return false;
            }
        }
        return true;
    };

    if (!rangesUseSingleHeap(false) && !repackRanges(false)) {
        return false;
    }
    if (!rangesUseSingleHeap(true) && !repackRanges(true)) {
        return false;
    }

    ID3D12DescriptorHeap *cbvHeap = nullptr;
    ID3D12DescriptorHeap *samplerHeap = nullptr;
    for (uint32_t i = 0; i < bindingCount; ++i) {
        if (bindings[i].splitLocalCbvSrvUav) {
            if (bindings[i].dynamicCbvSrvUavRange.valid) {
                cbvHeap = bindings[i].dynamicCbvSrvUavRange.heap;
            }
            if (bindings[i].staticCbvSrvUavRange.valid) {
                cbvHeap = bindings[i].staticCbvSrvUavRange.heap;
            }
        } else if (bindings[i].cbvRange.valid) {
            cbvHeap = bindings[i].cbvRange.heap;
        }
        if (bindings[i].samplerRange.valid) {
            samplerHeap = bindings[i].samplerRange.heap;
        }
    }

    ID3D12DescriptorHeap *boundHeaps[2]{};
    UINT boundHeapCount = 0;
    if (cbvHeap) {
        boundHeaps[boundHeapCount++] = cbvHeap;
    }
    if (samplerHeap) {
        boundHeaps[boundHeapCount++] = samplerHeap;
    }
    if (boundHeapCount > 0 &&
        (_impl->boundCbvSrvUavHeap != cbvHeap || _impl->boundSamplerHeap != samplerHeap)) {
        if (_type == CommandBufferType::SECONDARY &&
            (_impl->boundCbvSrvUavHeap || _impl->boundSamplerHeap)) {
            return false;
        }
        _impl->commandList->SetDescriptorHeaps(boundHeapCount, boundHeaps);
        _impl->boundCbvSrvUavHeap = cbvHeap;
        _impl->boundSamplerHeap = samplerHeap;
        invalidateDescriptorTables();
    }

    auto bindRootTable = [&](int32_t rootIndex, const PreparedRange &range) {
        if (!range.valid || rootIndex < 0) {
            return true;
        }
        if (rootIndex >= static_cast<int32_t>(D3D12_MAX_ROOT_PARAMETERS)) {
            CC_LOG_ERROR("D3D12 root parameter index %d exceeds the command buffer cache capacity.", rootIndex);
            return false;
        }
        auto &bound = _impl->boundRootTables[rootIndex];
        if (!bound.valid || bound.gpuHandle != range.gpuHandle) {
            _impl->commandList->SetGraphicsRootDescriptorTable(
                static_cast<UINT>(rootIndex), {range.gpuHandle});
            bound = {range.gpuHandle, true};
        }
        return true;
    };

    for (uint32_t i = 0; i < bindingCount; ++i) {
        const auto &binding = bindings[i];
        if ((binding.splitLocalCbvSrvUav &&
             ((binding.useLocalRootCbv && binding.localRootCbvGpuAddress == 0) ||
              (binding.useLocalRootCbv && binding.dynamicDescriptorCount > 0 &&
               !bindRootTable(binding.dynamicCbvRootIndex, binding.dynamicCbvSrvUavRange)) ||
              (!binding.useLocalRootCbv &&
               !bindRootTable(binding.dynamicCbvRootIndex, binding.dynamicCbvSrvUavRange)) ||
              !bindRootTable(binding.staticCbvSrvUavRootIndex, binding.staticCbvSrvUavRange))) ||
            (!binding.splitLocalCbvSrvUav && !bindRootTable(binding.cbvRootIndex, binding.cbvRange)) ||
            !bindRootTable(binding.samplerRootIndex, binding.samplerRange)) {
            return false;
        }
        if (binding.useLocalRootCbv) {
            _impl->commandList->SetGraphicsRootConstantBufferView(
                static_cast<UINT>(binding.localRootCbvRootIndex), binding.localRootCbvGpuAddress);
        }
    }

    _impl->descriptorSetsDirty = false;
    _impl->localDescriptorSetOnlyDirty = false;
    return true;
}

void CCD3D12CommandBuffer::flushDescriptorSets() {
    if (flushDescriptorSetsIncremental()) {
        return;
    }
    // The fallback path owns its descriptor allocations independently. Drop
    // incremental sampler ranges so a later draw cannot reuse another heap epoch.
    _impl->samplerTableCache.clear();
    _impl->localStaticCbvSrvUavTableCache.clear();
    _impl->samplerTableCacheHeap = nullptr;
    _impl->samplerTableCacheHeapIndex = std::numeric_limits<uint32_t>::max();
    if (!_impl->descriptorSetsDirty || !_impl->commandList) return;
    _impl->descriptorSetsDirty = false;
    _impl->localDescriptorSetOnlyDirty = false;

    auto *device = CCD3D12Device::getInstance();
    if (!device) return;

    auto *boundLayout = static_cast<CCD3D12PipelineLayout *>(_impl->boundPipelineLayout);
    if (!boundLayout) return;

    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    auto *heapPool = device->getGPUDescriptorHeapPool();
    auto *samplerPool = device->getSamplerDescriptorHeapPool();
    if (!d3dDevice || !heapPool) return;

    // Collect all CBV/SRV/UAV and Sampler descriptors across all pending sets,
    // copy them into a single GPU-visible heap allocation per type, then bind once.
    ID3D12DescriptorHeap *boundHeaps[2] = {};
    UINT boundHeapCount = 0;

    // Fixed-size stack arrays — avoid per-draw-call malloc/free.
    // D3D12 root signature allows at most D3D12_MAX_ROOT_COST (64 DWORDs);
    // in practice we bind ≤ 4 descriptor sets × 2 tables = 8 entries each.
    constexpr uint32_t MAX_ROOT_TABLE_ENTRIES = 16;
    struct RootTableEntry {
        UINT rootParameterIndex;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
    };
    RootTableEntry cbvEntries[MAX_ROOT_TABLE_ENTRIES];
    RootTableEntry samplerEntries[MAX_ROOT_TABLE_ENTRIES];
    uint32_t cbvEntryCount = 0;
    uint32_t samplerEntryCount = 0;
    ID3D12DescriptorHeap *cbvHeap = nullptr;
    ID3D12DescriptorHeap *samplerHeap = nullptr;

    struct SetBindingInfo {
        CCD3D12DescriptorSet *set{nullptr};
        ccstd::vector<uint32_t> dynamicOffsets;
        uint32_t cbvCount{0};
        uint32_t samplerCount{0};
        int cbvRootIndex{-1};
        int samplerRootIndex{-1};
    };
    SetBindingInfo bindings[D3D12_MAX_BOUND_SETS];
    uint32_t bindingCount = 0;
    uint32_t totalCbvCount = 0;
    uint32_t totalSamplerCount = 0;

    for (uint32_t i = 0; i < _impl->pendingSetCount; ++i) {
        if (!_impl->pendingSets[i].valid || !_impl->pendingSets[i].set) continue;

        const uint32_t setIdx = _impl->pendingSets[i].setIndex;
        auto *d3d12Set = static_cast<CCD3D12DescriptorSet *>(_impl->pendingSets[i].set);

        const auto cbvCount = d3d12Set->getCbvSrvUavDescriptorCount();
        const auto samplerCount = d3d12Set->getSamplerDescriptorCount();
        const auto cbvRootIndex = boundLayout->getCbvSrvUavRootParameterIndex(setIdx);
        const auto samplerRootIndex = boundLayout->getSamplerRootParameterIndex(setIdx);

        if (bindingCount < D3D12_MAX_BOUND_SETS) {
            bindings[bindingCount++] = {
                d3d12Set,
                _impl->pendingSets[i].dynamicOffsets,
                cbvCount,
                samplerCount,
                cbvRootIndex,
                samplerRootIndex,
            };
        }
        if (cbvCount > 0 && cbvRootIndex >= 0) {
            totalCbvCount += cbvCount;
        }
        if (samplerCount > 0 && samplerRootIndex >= 0 && samplerPool) {
            totalSamplerCount += samplerCount;
        }
    }

    D3D12DescriptorHeapPool::Allocation cbvAlloc;
    if (totalCbvCount > 0) {
        cbvAlloc = heapPool->allocate(totalCbvCount);
        if (cbvAlloc.isValid) {
            cbvHeap = static_cast<ID3D12DescriptorHeap *>(heapPool->getHeap(cbvAlloc.heapIndex));
        }
    }

    D3D12DescriptorHeapPool::Allocation samplerAlloc;
    if (totalSamplerCount > 0 && samplerPool) {
        samplerAlloc = samplerPool->allocate(totalSamplerCount);
        if (samplerAlloc.isValid) {
            samplerHeap = static_cast<ID3D12DescriptorHeap *>(samplerPool->getHeap(samplerAlloc.heapIndex));
        }
    }

    uint32_t cbvOffset = 0;
    uint32_t samplerOffset = 0;
    const uint32_t cbvDescriptorSize = heapPool->getDescriptorSize();
    const uint32_t samplerDescriptorSize = samplerPool ? samplerPool->getDescriptorSize() : 0;
    for (uint32_t i = 0; i < bindingCount; ++i) {
        auto &binding = bindings[i];
        bool appliedDynamicOffsets = false;

        if (!binding.dynamicOffsets.empty()) {
            binding.set->forceUpdate();
            binding.set->applyDynamicOffsets(static_cast<uint32_t>(binding.dynamicOffsets.size()), binding.dynamicOffsets.data());
            appliedDynamicOffsets = true;
        } else {
            binding.set->update();
        }

        if (binding.cbvCount > 0 && binding.cbvRootIndex >= 0 && cbvAlloc.isValid && cbvHeap) {
            auto *srcHeap = static_cast<ID3D12DescriptorHeap *>(binding.set->getCbvSrvUavDescriptorHeap());
            if (srcHeap) {
                D3D12_CPU_DESCRIPTOR_HANDLE srcStart{binding.set->getCbvSrvUavCPUDescriptorHandle()};
                D3D12_CPU_DESCRIPTOR_HANDLE dstStart{};
                dstStart.ptr = reinterpret_cast<SIZE_T>(cbvAlloc.cpuHandle) +
                               static_cast<SIZE_T>(cbvOffset) * cbvDescriptorSize;
                d3dDevice->CopyDescriptorsSimple(binding.cbvCount, dstStart, srcStart, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                if (cbvEntryCount < MAX_ROOT_TABLE_ENTRIES) {
                    cbvEntries[cbvEntryCount++] = {
                        static_cast<UINT>(binding.cbvRootIndex),
                        {cbvAlloc.gpuHandle + static_cast<uint64_t>(cbvOffset) * cbvDescriptorSize},
                    };
                }
            }
            cbvOffset += binding.cbvCount;
        }

        if (binding.samplerCount > 0 && binding.samplerRootIndex >= 0 && samplerAlloc.isValid && samplerHeap) {
            auto *srcHeap = static_cast<ID3D12DescriptorHeap *>(binding.set->getSamplerDescriptorHeap());
            if (srcHeap) {
                D3D12_CPU_DESCRIPTOR_HANDLE srcStart{binding.set->getSamplerCPUDescriptorHandle()};
                D3D12_CPU_DESCRIPTOR_HANDLE dstStart{};
                dstStart.ptr = reinterpret_cast<SIZE_T>(samplerAlloc.cpuHandle) +
                               static_cast<SIZE_T>(samplerOffset) * samplerDescriptorSize;
                d3dDevice->CopyDescriptorsSimple(binding.samplerCount, dstStart, srcStart, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                if (samplerEntryCount < MAX_ROOT_TABLE_ENTRIES) {
                    samplerEntries[samplerEntryCount++] = {
                        static_cast<UINT>(binding.samplerRootIndex),
                        {samplerAlloc.gpuHandle + static_cast<uint64_t>(samplerOffset) * samplerDescriptorSize},
                    };
                }
            }
            samplerOffset += binding.samplerCount;
        }

        if (appliedDynamicOffsets) {
            binding.set->forceUpdate();
        }
    }

    // Set descriptor heaps ONCE for all sets
    if (cbvHeap) {
        boundHeaps[boundHeapCount++] = cbvHeap;
    }
    if (samplerHeap) {
        // Avoid duplicate if sampler heap is same as CBV heap (shouldn't happen, but be safe)
        bool alreadyBound = false;
        for (UINT h = 0; h < boundHeapCount; ++h) {
            if (boundHeaps[h] == samplerHeap) { alreadyBound = true; break; }
        }
        if (!alreadyBound) {
            boundHeaps[boundHeapCount++] = samplerHeap;
        }
    }

    if (boundHeapCount > 0) {
        bool setDescriptorHeaps = true;
        if (_type == CommandBufferType::SECONDARY) {
            const bool bundleAlreadyBoundHeaps = _impl->boundCbvSrvUavHeap || _impl->boundSamplerHeap;
            if (bundleAlreadyBoundHeaps &&
                (_impl->boundCbvSrvUavHeap != cbvHeap || _impl->boundSamplerHeap != samplerHeap)) {
                CC_LOG_ERROR("D3D12 bundle descriptor allocation changed heap objects; bundle recording is invalid.");
                return;
            }
            setDescriptorHeaps = !bundleAlreadyBoundHeaps;
        }
        if (setDescriptorHeaps) {
            _impl->commandList->SetDescriptorHeaps(boundHeapCount, boundHeaps);
        }
        _impl->boundCbvSrvUavHeap = cbvHeap;
        _impl->boundSamplerHeap = samplerHeap;
    }

    // Set all root descriptor tables
    for (uint32_t i = 0; i < cbvEntryCount; ++i) {
        _impl->commandList->SetGraphicsRootDescriptorTable(cbvEntries[i].rootParameterIndex, cbvEntries[i].gpuHandle);
    }
    for (uint32_t i = 0; i < samplerEntryCount; ++i) {
        _impl->commandList->SetGraphicsRootDescriptorTable(samplerEntries[i].rootParameterIndex, samplerEntries[i].gpuHandle);
    }
}

bool CCD3D12CommandBuffer::captureLocalRootCbvBatchBinding(uint64_t &gpuAddress,
                                                            uint32_t &rootParameterIndex,
                                                            CCD3D12DescriptorSet *&descriptorSet,
                                                            uint64_t &staticSignature,
                                                            CCD3D12DescriptorSet *directDescriptorSet) {
    gpuAddress = 0;
    rootParameterIndex = 0;
    descriptorSet = nullptr;
    staticSignature = 0;
    if (!_impl || !_impl->boundPipelineLayout) {
        return false;
    }

    auto *boundLayout = static_cast<CCD3D12PipelineLayout *>(_impl->boundPipelineLayout);
    auto *set = directDescriptorSet;
    if (!set) {
        auto *localPending = static_cast<Impl::PendingDescriptorSet *>(nullptr);
        for (uint32_t i = 0; i < _impl->pendingSetCount; ++i) {
            auto &pending = _impl->pendingSets[i];
            if (pending.valid && pending.set && pending.setIndex == D3D12_LOCAL_DESCRIPTOR_SET_INDEX) {
                localPending = &pending;
                break;
            }
        }
        if (!localPending) {
            return false;
        }
        set = static_cast<CCD3D12DescriptorSet *>(localPending->set);
    }

    const int32_t rootCbvIndex = boundLayout->getLocalRootCbvParameterIndex(D3D12_LOCAL_DESCRIPTOR_SET_INDEX);
    if (!set || rootCbvIndex < 0 || !_impl->boundRootSignature) {
        return false;
    }

    set->updateForLocalRootCbv(true);
    uint32_t rootCbvDescriptorOffset = 0;
    uint32_t staticCbvSrvUavCount = 0;
    if (!set->getCbvSrvUavPartition(rootCbvDescriptorOffset, staticCbvSrvUavCount) ||
        !set->hasOnlyNullDynamicDescriptorSources()) {
        return false;
    }

    uint32_t dynamicCbvDescriptors = 0;
    uint32_t staticTextureDescriptors = 0;
    uint32_t staticCbvSrvUavDescriptors = 0;
    set->getStaticDescriptorAnalysis(dynamicCbvDescriptors, staticTextureDescriptors,
                                     staticCbvSrvUavDescriptors, staticSignature);
    if (dynamicCbvDescriptors != 1 || staticSignature == 0 ||
        staticCbvSrvUavDescriptors != staticCbvSrvUavCount ||
        !set->canReuseStaticCbvSrvUavResources()) {
        return false;
    }

    uint32_t rootCbvSize = 0;
    const uint32_t uniformSlotCount = set->getUniformDescriptorSlotCount();
    for (uint32_t slotIndex = 0; slotIndex < uniformSlotCount; ++slotIndex) {
        uint32_t descriptorOffset = 0;
        if (set->getUniformDescriptorSignature(slotIndex, descriptorOffset, gpuAddress, rootCbvSize) &&
            descriptorOffset == rootCbvDescriptorOffset) {
            break;
        }
    }
    if (gpuAddress == 0 || rootCbvSize < 256U) {
        auto *device = CCD3D12Device::getInstance();
        auto *dummyBuffer = device ? device->getDummyBuffer() : nullptr;
        gpuAddress = dummyBuffer ? dummyBuffer->getD3D12GPUVirtualAddress() : 0;
    }
    if (gpuAddress == 0) {
        return false;
    }

    rootParameterIndex = static_cast<uint32_t>(rootCbvIndex);
    descriptorSet = set;
    return true;
}

void CCD3D12CommandBuffer::flushLocalRootCbvBatch() {
    if (!_impl || !_impl->localRootCbvBatchActive) {
        return;
    }

    const bool indexed = _impl->localRootCbvBatchIndexed;
    const uint32_t commandCount = indexed
                                      ? static_cast<uint32_t>(_impl->localRootCbvBatchIndexedDraws.size())
                                      : static_cast<uint32_t>(_impl->localRootCbvBatchDraws.size());
    if (commandCount == 0) {
        _impl->localRootCbvBatchReady = false;
        _impl->localRootCbvBatchDescriptorSet = nullptr;
        _impl->localRootCbvBatchStaticSignature = 0;
        _impl->localRootCbvBatchSamplerKey.clear();
        return;
    }

    auto *device = CCD3D12Device::getInstance();
    auto *signature = static_cast<ID3D12CommandSignature *>(
        device ? device->getOrCreateLocalRootCbvIndirectSignature(
                     _impl->localRootCbvBatchRootSignature,
                     _impl->localRootCbvBatchRootParameterIndex, indexed)
               : nullptr);
    const uint64_t stride = indexed ? sizeof(Impl::LocalRootCbvIndexedIndirectDraw)
                                    : sizeof(Impl::LocalRootCbvIndirectDraw);
    const uint64_t byteCount = stride * commandCount;
    const auto upload = signature && device
                            ? device->allocateUploadBuffer(byteCount, sizeof(uint64_t))
                            : D3D12UploadAllocation{};

    if (signature && upload.isValid && upload.resource && upload.mappedData) {
        const void *commands = indexed
                                   ? static_cast<const void *>(_impl->localRootCbvBatchIndexedDraws.data())
                                   : static_cast<const void *>(_impl->localRootCbvBatchDraws.data());
        std::memcpy(upload.mappedData, commands, static_cast<size_t>(byteCount));
        auto *resource = static_cast<ID3D12Resource *>(upload.resource);
        retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, resource);
        _impl->commandList->ExecuteIndirect(signature, commandCount, resource, upload.offset, nullptr, 0);
    } else {
        // Allocation/signature failures are intentionally non-fatal: preserve
        // the original Root-CBV plus Draw sequence for this batch.
        if (indexed) {
            for (const auto &command : _impl->localRootCbvBatchIndexedDraws) {
                _impl->commandList->SetGraphicsRootConstantBufferView(
                    _impl->localRootCbvBatchRootParameterIndex, command.rootCbvGpuAddress);
                _impl->commandList->DrawIndexedInstanced(
                    command.arguments.IndexCountPerInstance, command.arguments.InstanceCount,
                    command.arguments.StartIndexLocation, command.arguments.BaseVertexLocation,
                    command.arguments.StartInstanceLocation);
            }
        } else {
            for (const auto &command : _impl->localRootCbvBatchDraws) {
                _impl->commandList->SetGraphicsRootConstantBufferView(
                    _impl->localRootCbvBatchRootParameterIndex, command.rootCbvGpuAddress);
                _impl->commandList->DrawInstanced(
                    command.arguments.VertexCountPerInstance, command.arguments.InstanceCount,
                    command.arguments.StartVertexLocation, command.arguments.StartInstanceLocation);
            }
        }
    }

    _numDrawCalls += commandCount;
    if (indexed) {
        for (const auto &command : _impl->localRootCbvBatchIndexedDraws) {
            _numInstances += command.arguments.InstanceCount;
            _numTriangles += (command.arguments.IndexCountPerInstance / 3U) * command.arguments.InstanceCount;
        }
        _impl->localRootCbvBatchIndexedDraws.clear();
    } else {
        for (const auto &command : _impl->localRootCbvBatchDraws) {
            _numInstances += command.arguments.InstanceCount;
            _numTriangles += (command.arguments.VertexCountPerInstance / 3U) * command.arguments.InstanceCount;
        }
        _impl->localRootCbvBatchDraws.clear();
    }
    _impl->localRootCbvBatchReady = false;
    _impl->localRootCbvBatchDescriptorSet = nullptr;
    _impl->localRootCbvBatchStaticSignature = 0;
    _impl->localRootCbvBatchSamplerKey.clear();
}

void CCD3D12CommandBuffer::bindInputAssembler(InputAssembler *ia) {
    if (!_impl->commandList || !ia) return;

    auto *const previousInputAssembler = _impl->boundIA;
    const bool sameLogicalInputAssembler = previousInputAssembler == ia;
    auto *d3d12IA = static_cast<CCD3D12InputAssembler *>(ia);
    d3d12IA->refreshBufferViews();
    _impl->boundIA = ia;
    // refreshBufferViews() may observe a backing-resource version change even
    // when the IA views themselves are identical. Only an emitted IA view
    // change must split a deferred local Root-CBV indirect batch.
    const uint32_t vbCount = d3d12IA->getVertexBufferCount();
    if (vbCount > D3D12_MAX_VERTEX_BUFFERS) {
        CC_LOG_ERROR("D3D12 InputAssembler uses %u vertex buffers; maximum supported is %u.",
                     vbCount, D3D12_MAX_VERTEX_BUFFERS);
        return;
    }
    D3D12_VERTEX_BUFFER_VIEW vbViews[D3D12_MAX_VERTEX_BUFFERS]{};
    if (vbCount > 0) {
        d3d12IA->fillVertexBufferViews(vbViews);
    }
    const bool vertexBuffersChanged =
        !_impl->vertexBufferStateValid ||
        _impl->boundVertexBufferCount != vbCount ||
        (vbCount > 0 && std::memcmp(_impl->boundVertexBufferViews, vbViews,
                                    sizeof(D3D12_VERTEX_BUFFER_VIEW) * vbCount) != 0);
    if (vertexBuffersChanged) {
        if (vbCount > 0) {
            _impl->commandList->IASetVertexBuffers(0, vbCount, vbViews);
        } else if (_impl->vertexBufferStateValid && _impl->boundVertexBufferCount > 0) {
            D3D12_VERTEX_BUFFER_VIEW emptyViews[D3D12_MAX_VERTEX_BUFFERS]{};
            _impl->commandList->IASetVertexBuffers(
                0, _impl->boundVertexBufferCount, emptyViews);
        }
        std::memcpy(_impl->boundVertexBufferViews, vbViews,
                    sizeof(D3D12_VERTEX_BUFFER_VIEW) * vbCount);
        _impl->boundVertexBufferCount = vbCount;
        _impl->vertexBufferStateValid = true;
    }

    const bool hasIndexBuffer = d3d12IA->hasIndexBuffer();
    D3D12_INDEX_BUFFER_VIEW ibView{};
    if (hasIndexBuffer) {
        d3d12IA->fillIndexBufferView(&ibView);
    }
    const bool indexBufferChanged =
        !_impl->indexBufferStateValid ||
        _impl->boundHasIndexBuffer != hasIndexBuffer ||
        (hasIndexBuffer && std::memcmp(&_impl->boundIndexBufferView, &ibView, sizeof(ibView)) != 0);
    // Different InputAssembler actors may describe the exact same native IA
    // views. A D3D12 batch only needs to split when a command-list view would
    // actually change; object identity itself carries no GPU state.
    if (_impl->localRootCbvBatchActive && (vertexBuffersChanged || indexBufferChanged)) {
        flushLocalRootCbvBatch();
    }
    if (!vertexBuffersChanged && !indexBufferChanged) {
        return;
    }
    if (indexBufferChanged) {
        _impl->commandList->IASetIndexBuffer(hasIndexBuffer ? &ibView : nullptr);
        _impl->boundIndexBufferView = ibView;
        _impl->boundHasIndexBuffer = hasIndexBuffer;
        _impl->indexBufferStateValid = true;
    }


    // Note: primitive topology is set in bindPipelineState from PSO info
}

void CCD3D12CommandBuffer::setViewport(const Viewport &vp) {
    if (!_impl->commandList) return;
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot set viewport state.");
        return;
    }

    D3D12_VIEWPORT d3dViewport{};
    d3dViewport.TopLeftX = static_cast<float>(vp.left);
    d3dViewport.TopLeftY = static_cast<float>(vp.top);
    d3dViewport.Width = static_cast<float>(vp.width);
    d3dViewport.Height = static_cast<float>(vp.height);
    d3dViewport.MinDepth = vp.minDepth;
    d3dViewport.MaxDepth = vp.maxDepth;
    if (!_impl->viewportValid || std::memcmp(&_impl->boundViewport, &d3dViewport, sizeof(d3dViewport)) != 0) {
        _impl->commandList->RSSetViewports(1, &d3dViewport);
        _impl->boundViewport = d3dViewport;
        _impl->viewportValid = true;
    }
}

void CCD3D12CommandBuffer::setScissor(const Rect &rect) {
    if (!_impl->commandList) return;
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot set scissor state.");
        return;
    }

    D3D12_RECT d3dRect{};
    d3dRect.left = rect.x;
    d3dRect.top = rect.y;
    d3dRect.right = static_cast<LONG>(rect.x + rect.width);
    d3dRect.bottom = static_cast<LONG>(rect.y + rect.height);
    if (!_impl->scissorValid || std::memcmp(&_impl->boundScissor, &d3dRect, sizeof(d3dRect)) != 0) {
        _impl->commandList->RSSetScissorRects(1, &d3dRect);
        _impl->boundScissor = d3dRect;
        _impl->scissorValid = true;
    }
}

void CCD3D12CommandBuffer::setLineWidth(float width) {
    (void)width;
    if (width != 1.0f) {
        CC_LOG_WARNING("[D3D12] setLineWidth(%.1f) ignored - D3D12 does not support line width > 1", width);
    }
}

void CCD3D12CommandBuffer::setDepthBias(float constant, float clamp, float slope) {
    _impl->dynamicDepthBias = constant;
    _impl->dynamicDepthBiasClamp = clamp;
    _impl->dynamicDepthBiasSlope = slope;
    _impl->hasDynamicDepthBias = true;
    applyDynamicPipelineState();
}

void CCD3D12CommandBuffer::setBlendConstants(const Color &constants) {
    if (!_impl->commandList) return;
    const float *blendFactor = &constants.x;
    if (!_impl->blendFactorValid ||
        std::memcmp(_impl->boundBlendFactor, blendFactor, sizeof(_impl->boundBlendFactor)) != 0) {
        _impl->commandList->OMSetBlendFactor(blendFactor);
        std::memcpy(_impl->boundBlendFactor, blendFactor, sizeof(_impl->boundBlendFactor));
        _impl->blendFactorValid = true;
    }
}

void CCD3D12CommandBuffer::setDepthBound(float minBounds, float maxBounds) {
    if (!_impl->commandList) return;
    // OMSetDepthBounds is available on ID3D12GraphicsCommandList1 (D3D12.1+).
    // Query the extended interface; fall back silently if unavailable.
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> cmdList1;
    if (SUCCEEDED(_impl->commandList->QueryInterface(IID_PPV_ARGS(&cmdList1)))) {
        cmdList1->OMSetDepthBounds(minBounds, maxBounds);
    }
}

void CCD3D12CommandBuffer::setStencilWriteMask(StencilFace face, uint32_t mask) {
    if (face != StencilFace::ALL) {
        CC_LOG_WARNING("D3D12 setStencilWriteMask: per-face masks are not supported; applying mask globally.");
    }
    _impl->dynamicStencilWriteMask = mask;
    _impl->hasDynamicStencilWriteMask = true;
    applyDynamicPipelineState();
}

void CCD3D12CommandBuffer::setStencilCompareMask(StencilFace face, uint32_t ref, uint32_t mask) {
    if (!_impl->commandList) return;
    if (face != StencilFace::ALL) {
        CC_LOG_WARNING("D3D12 setStencilCompareMask: per-face masks/references are not supported; applying them globally.");
    }
    _impl->dynamicStencilReadMask = mask;
    _impl->hasDynamicStencilReadMask = true;
    if (!_impl->stencilRefValid || _impl->boundStencilRef != ref) {
        _impl->commandList->OMSetStencilRef(ref);
        _impl->boundStencilRef = ref;
        _impl->stencilRefValid = true;
    }
    applyDynamicPipelineState();
}

void CCD3D12CommandBuffer::nextSubpass() {
    if (!_impl->commandList || !_impl->inRenderPass || !_impl->activeRenderPass || !_impl->activeFramebuffer) {
        return;
    }
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot change subpasses.");
        return;
    }

    const auto &subpasses = _impl->activeRenderPass->getSubpasses();
    if (subpasses.empty()) {
        return;
    }

    const uint32_t nextSubpassIndex = _impl->currentSubpass + 1;
    if (nextSubpassIndex >= subpasses.size()) {
        CC_LOG_WARNING("D3D12 nextSubpass ignored: requested subpass %u but render pass has %zu subpasses.",
                       nextSubpassIndex, subpasses.size());
        return;
    }

    resolveSubpass(_impl->currentSubpass);

    auto *framebuffer = _impl->activeFramebuffer;
    const auto &subpass = subpasses[nextSubpassIndex];
    auto containsAttachment = [](const ccstd::vector<uint32_t> &attachments, uint32_t index) {
        return std::find(attachments.begin(), attachments.end(), index) != attachments.end();
    };
    for (uint32_t input : subpass.inputs) {
        if (containsAttachment(subpass.colors, input)) {
            CC_LOG_WARNING("D3D12 nextSubpass: input/color self-dependency on attachment %u is not natively expressible; "
                           "keeping it as a render target.",
                           input);
            continue;
        }
        transitionColorAttachment(input, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    const bool hasDSV = subpass.depthStencil != INVALID_BINDING;
    if (hasDSV) {
        if (_impl->activeDepthStencil && _impl->activeDepthTexture) {
            const D3D12_RESOURCE_STATES prevState = _impl->activeDepthTexture->getCurrentState();
            if (prevState != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = _impl->activeDepthStencil;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = prevState;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                _impl->commandList->ResourceBarrier(1, &barrier);
                _impl->activeDepthTexture->setCurrentState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
            }
        }
    }

    bindSubpassRenderTargets(nextSubpassIndex);
    _impl->currentSubpass = nextSubpassIndex;
}

void CCD3D12CommandBuffer::beginDrawBatch() {
    if (!_impl->commandList || _type != CommandBufferType::PRIMARY) {
        return;
    }
    flushLocalRootCbvBatch();
    // RenderQueue records a CPU-stable sequence. Drain every queued buffer
    // update before binding its first PSO/IA so the drain's conservative
    // graphics-state invalidation cannot split every following indirect draw.
    if (auto *device = CCD3D12Device::getInstance()) {
        device->flushPendingBufferUpdates(this);
    }
    _impl->localRootCbvBatchActive = true;
    _impl->localRootCbvBatchReady = false;
}

void CCD3D12CommandBuffer::endDrawBatch() {
    if (!_impl->localRootCbvBatchActive) {
        return;
    }
    flushLocalRootCbvBatch();
    _impl->localRootCbvBatchActive = false;
    _impl->localRootCbvBatchReady = false;
    _impl->localRootCbvBatchRootSignature = nullptr;
}

void CCD3D12CommandBuffer::drawWithInputAssemblerAndDescriptorSet(InputAssembler *inputAssembler,
                                                                   uint32_t set,
                                                                   DescriptorSet *descriptorSet,
                                                                   const DrawInfo &info) {
    bindInputAssembler(inputAssembler);
    if (!_impl->commandList || set != D3D12_LOCAL_DESCRIPTOR_SET_INDEX || !descriptorSet ||
        !_impl->localRootCbvBatchActive || _type != CommandBufferType::PRIMARY) {
        bindDescriptorSet(set, descriptorSet, 0, nullptr);
        draw(info);
        return;
    }

    auto *d3d12Set = static_cast<CCD3D12DescriptorSet *>(descriptorSet);
    uint32_t rootCbvDescriptorOffset = 0;
    uint32_t staticCbvSrvUavCount = 0;
    uint32_t dynamicCbvDescriptors = 0;
    uint32_t staticTextureDescriptors = 0;
    uint32_t staticCbvSrvUavDescriptors = 0;
    uint64_t staticSignature = 0;
    const bool directBatchEligible =
        d3d12Set->getCbvSrvUavPartition(rootCbvDescriptorOffset, staticCbvSrvUavCount) &&
        d3d12Set->hasOnlyNullDynamicDescriptorSources() &&
        d3d12Set->canReuseStaticCbvSrvUavResources();
    if (directBatchEligible) {
        d3d12Set->getStaticDescriptorAnalysis(dynamicCbvDescriptors, staticTextureDescriptors,
                                               staticCbvSrvUavDescriptors, staticSignature);
    }
    const bool hasExpectedLocalPartition = directBatchEligible && dynamicCbvDescriptors == 1 &&
                                           staticSignature != 0 &&
                                           staticCbvSrvUavDescriptors == staticCbvSrvUavCount;
    const bool localStaticStateChanged =
        !_impl->localRootCbvBatchReady || !hasExpectedLocalPartition ||
        staticSignature != _impl->localRootCbvBatchStaticSignature ||
        !_impl->localRootCbvBatchDescriptorSet ||
        !d3d12Set->hasMatchingStaticCbvSrvUavResources(*_impl->localRootCbvBatchDescriptorSet) ||
        d3d12Set->getSamplerTableKey() != _impl->localRootCbvBatchSamplerKey;
    if (localStaticStateChanged) {
        flushLocalRootCbvBatch();
        // The first command of a compatible run still establishes all static
        // root tables through the normal path. Later commands only encode b0.
        bindDescriptorSet(set, descriptorSet, 0, nullptr);
    }

    drawLocalRootCbvBatchInternal(info, d3d12Set);
}

void CCD3D12CommandBuffer::drawLocalRootCbvBatchInternal(const DrawInfo &info,
                                                          CCD3D12DescriptorSet *directDescriptorSet) {
    if (!_impl->commandList || !_impl->localRootCbvBatchActive ||
        _type != CommandBufferType::PRIMARY || !_impl->boundIA) {
        draw(info);
        return;
    }

    auto *ia = static_cast<CCD3D12InputAssembler *>(_impl->boundIA);
    if (ia->getIndirectBuffer()) {
        flushLocalRootCbvBatch();
        draw(info);
        return;
    }

    const bool indexed = info.indexCount > 0;
    if (_impl->localRootCbvBatchReady && indexed != _impl->localRootCbvBatchIndexed) {
        flushLocalRootCbvBatch();
    }

    if (!_impl->localRootCbvBatchReady) {
        // The first draw of each compatible run establishes the static
        // descriptor tables. Subsequent draws only append their local b0
        // address to the indirect argument stream.
        flushDescriptorSets();
    }

    uint64_t rootCbvGpuAddress = 0;
    uint32_t rootParameterIndex = 0;
    CCD3D12DescriptorSet *descriptorSet = nullptr;
    uint64_t staticSignature = 0;
    if (!captureLocalRootCbvBatchBinding(rootCbvGpuAddress, rootParameterIndex,
                                         descriptorSet, staticSignature, directDescriptorSet)) {
        flushLocalRootCbvBatch();
        if (directDescriptorSet) {
            bindDescriptorSet(D3D12_LOCAL_DESCRIPTOR_SET_INDEX, directDescriptorSet, 0, nullptr);
        }
        draw(info);
        return;
    }

    if (_impl->localRootCbvBatchReady) {
        if (rootParameterIndex != _impl->localRootCbvBatchRootParameterIndex ||
            _impl->boundRootSignature != _impl->localRootCbvBatchRootSignature ||
            staticSignature != _impl->localRootCbvBatchStaticSignature ||
            !descriptorSet || !_impl->localRootCbvBatchDescriptorSet ||
            !descriptorSet->hasMatchingStaticCbvSrvUavResources(*_impl->localRootCbvBatchDescriptorSet) ||
            descriptorSet->getSamplerTableKey() != _impl->localRootCbvBatchSamplerKey) {
            flushLocalRootCbvBatch();
            if (directDescriptorSet) {
                bindDescriptorSet(D3D12_LOCAL_DESCRIPTOR_SET_INDEX, directDescriptorSet, 0, nullptr);
            }
            drawLocalRootCbvBatchInternal(info, directDescriptorSet);
            return;
        }
    } else {
        _impl->localRootCbvBatchReady = true;
        _impl->localRootCbvBatchIndexed = indexed;
        _impl->localRootCbvBatchRootParameterIndex = rootParameterIndex;
        _impl->localRootCbvBatchRootSignature = _impl->boundRootSignature;
        _impl->localRootCbvBatchDescriptorSet = descriptorSet;
        _impl->localRootCbvBatchStaticSignature = staticSignature;
        _impl->localRootCbvBatchSamplerKey = descriptorSet->getSamplerTableKey();
    }

    if (indexed) {
        auto &command = _impl->localRootCbvBatchIndexedDraws.emplace_back();
        command.rootCbvGpuAddress = rootCbvGpuAddress;
        command.arguments.IndexCountPerInstance = info.indexCount;
        command.arguments.InstanceCount = std::max<uint32_t>(info.instanceCount, 1U);
        command.arguments.StartIndexLocation = info.firstIndex;
        command.arguments.BaseVertexLocation = info.vertexOffset;
        command.arguments.StartInstanceLocation = info.firstInstance;
    } else {
        auto &command = _impl->localRootCbvBatchDraws.emplace_back();
        command.rootCbvGpuAddress = rootCbvGpuAddress;
        command.arguments.VertexCountPerInstance = info.vertexCount;
        command.arguments.InstanceCount = std::max<uint32_t>(info.instanceCount, 1U);
        command.arguments.StartVertexLocation = info.firstVertex;
        command.arguments.StartInstanceLocation = info.firstInstance;
    }

    // The root-CBV address is encoded in the pending indirect command, so a
    // later normal draw must not re-flush this local descriptor binding.
    _impl->descriptorSetsDirty = false;
    _impl->localDescriptorSetOnlyDirty = false;
}

void CCD3D12CommandBuffer::draw(const DrawInfo &info) {
    if (!_impl->commandList) return;
    if (_impl->localRootCbvBatchActive) {
        flushLocalRootCbvBatch();
    }

    if (_type == CommandBufferType::PRIMARY) {
        CCD3D12Device::getInstance()->flushPendingBufferUpdates(this);
    }

    // Flush any pending descriptor set bindings before drawing
    flushDescriptorSets();

    const uint32_t instanceCount = std::max<uint32_t>(info.instanceCount, 1);
    const uint32_t firstInstance = info.firstInstance;
    // Check for indirect draw via InputAssembler's indirect buffer
    if (_impl->boundIA) {
        auto *ia = static_cast<CCD3D12InputAssembler *>(_impl->boundIA);
        Buffer *indirectBuf = ia->getIndirectBuffer();
        if (indirectBuf) {
            auto *d3d12Buf = static_cast<CCD3D12Buffer *>(indirectBuf);
            ID3D12Resource *resource = static_cast<ID3D12Resource *>(d3d12Buf->getD3D12ResourceHandle());
            if (resource) {
                retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, resource);
                auto *device = CCD3D12Device::getInstance();
                if (info.indexCount > 0) {
                    auto *sig = static_cast<ID3D12CommandSignature *>(device->getDrawIndexedIndirectSignature());
                    if (sig) {
                        _impl->commandList->ExecuteIndirect(sig, 1, resource, 0, nullptr, 0);
                    }
                } else {
                    auto *sig = static_cast<ID3D12CommandSignature *>(device->getDrawIndirectSignature());
                    if (sig) {
                        _impl->commandList->ExecuteIndirect(sig, 1, resource, 0, nullptr, 0);
                    }
                }
                ++_numDrawCalls;
                return;
            }
        }
    }

    if (info.indexCount > 0) {
        // Indexed draw
        _impl->commandList->DrawIndexedInstanced(
            info.indexCount,
            instanceCount,
            info.firstIndex,
            info.vertexOffset,
            firstInstance);
    } else {
        // Non-indexed draw
        _impl->commandList->DrawInstanced(
            info.vertexCount,
            instanceCount,
            info.firstVertex,
            firstInstance);
    }

    ++_numDrawCalls;
    _numInstances += instanceCount;
    _numTriangles += info.indexCount > 0 ? (info.indexCount / 3) * instanceCount : (info.vertexCount / 3) * instanceCount;
}

void CCD3D12CommandBuffer::updateBuffer(Buffer *buff, const void *data, uint32_t size) {
    if (!_impl->commandList || !buff || !data || size == 0) return;

    auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(buff);
    const uint32_t copySize = std::min(size, buff->getSize());
    if (copySize == 0) return;

    // Upload heaps can be mapped directly; default heaps, including uniform
    // buffers, are updated through a GPU copy so descriptors keep stable backing.
    auto *resource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
    if (!resource) return;
    if (_impl->bufferUpdateBatchActive && _impl->bufferUpdateBatchDestinationsAreUnique) {
        // The proof is batch-local, so this resource may already be retained by
        // another path. An extra owning reference is safe and avoids a set lookup.
        Microsoft::WRL::ComPtr<ID3D12Resource> retained;
        retained = resource;
        _impl->pendingUploadResources.push_back(std::move(retained));
    } else {
        retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, resource);
    }

    if (!d3d12Buffer->isD3D12UploadHeap()) {
        auto *device = CCD3D12Device::getInstance();
        auto upload = device ? device->allocateUploadBuffer(copySize, 256) : D3D12UploadAllocation{};
        if (!upload.isValid || !upload.mappedData || !upload.resource) {
            return;
        }
        std::memcpy(upload.mappedData, data, copySize);

        const auto previousState = d3d12Buffer->getCurrentState();
        if (_impl->bufferUpdateBatchActive) {
            uint32_t transitionIndex = 0;
            if (_impl->bufferUpdateBatchDestinationsAreUnique) {
                transitionIndex = static_cast<uint32_t>(_impl->pendingDefaultBufferTransitions.size());
                _impl->pendingDefaultBufferTransitions.push_back({resource, previousState});
            } else {
                const auto nextTransitionIndex = static_cast<uint32_t>(_impl->pendingDefaultBufferTransitions.size());
                const auto [transitionIt, inserted] =
                    _impl->pendingDefaultBufferTransitionIndices.try_emplace(resource, nextTransitionIndex);
                transitionIndex = transitionIt->second;
                if (inserted) {
                    _impl->pendingDefaultBufferTransitions.push_back({resource, previousState});
                } else {
                    ++_impl->pendingDefaultBufferTransitions[transitionIndex].copyCount;
                }
            }
            _impl->pendingDefaultBufferCopies.push_back({
                d3d12Buffer,
                resource,
                d3d12Buffer->getD3D12ResourceOffset(),
                static_cast<ID3D12Resource *>(upload.resource),
                upload.offset,
                copySize,
                transitionIndex,
            });
            return;
        }

        // Completed D3D12 buffer submissions decay to COMMON. The first COPY_DEST
        // access can then use free implicit promotion instead of an explicit barrier.
        if (previousState != D3D12_RESOURCE_STATE_COPY_DEST &&
            previousState != D3D12_RESOURCE_STATE_COMMON) {
            D3D12_RESOURCE_BARRIER toCopyDest{};
            toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopyDest.Transition.pResource = resource;
            toCopyDest.Transition.StateBefore = previousState;
            toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            _impl->commandList->ResourceBarrier(1, &toCopyDest);
        }
        _impl->commandList->CopyBufferRegion(
            resource,
            d3d12Buffer->getD3D12ResourceOffset(),
            static_cast<ID3D12Resource *>(upload.resource),
            upload.offset,
            copySize);
        D3D12_RESOURCE_BARRIER toRead{};
        toRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRead.Transition.pResource = resource;
        toRead.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toRead.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
        toRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        _impl->commandList->ResourceBarrier(1, &toRead);
        d3d12Buffer->setCurrentState(D3D12_RESOURCE_STATE_GENERIC_READ);
        return;
    }

    void *mappedData = nullptr;
    D3D12_RANGE readRange{};
    HRESULT hr = resource->Map(0, &readRange, &mappedData);
    if (FAILED(hr) || !mappedData) return;

    auto *dst = static_cast<uint8_t *>(mappedData) + d3d12Buffer->getD3D12ResourceOffset();
    std::memcpy(dst, data, copySize);
    D3D12_RANGE writeRange{d3d12Buffer->getD3D12ResourceOffset(), d3d12Buffer->getD3D12ResourceOffset() + copySize};
    resource->Unmap(0, &writeRange);
}

void CCD3D12CommandBuffer::copyBuffersToTexture(const uint8_t *const *buffers, Texture *texture, const BufferTextureCopy *regions, uint32_t count) {
    if (!_impl->commandList || !buffers || !texture || !regions || count == 0) return;
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot copy buffers to textures.");
        return;
    }

    auto *d3d12Texture = static_cast<CCD3D12Texture *>(texture);
    auto *textureResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
    if (!textureResource) return;
    retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, textureResource);

    auto *device = CCD3D12Device::getInstance();
    if (!device) return;

    auto *d3dDevice = static_cast<ID3D12Device *>(device->getD3D12DeviceHandle());
    if (!d3dDevice) return;

    const auto &textureInfo = texture->getInfo();
    if (formatSize(textureInfo.format, 1, 1, 1) == 0) return;

    // Transition to copy dest
    D3D12_RESOURCE_BARRIER toCopyDest{};
    toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopyDest.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    toCopyDest.Transition.pResource = textureResource;
    toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopyDest.Transition.StateBefore = d3d12Texture->getCurrentState();
    toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    _impl->commandList->ResourceBarrier(1, &toCopyDest);
    d3d12Texture->setCurrentState(D3D12_RESOURCE_STATE_COPY_DEST);

    for (uint32_t i = 0; i < count; ++i) {
        if (!buffers[i]) continue;

        const auto &region = regions[i];
        const uint32_t mipLevel = region.texSubres.mipLevel;
        const uint32_t arrayLayer = textureInfo.type == TextureType::TEX3D ? 0 : region.texSubres.baseArrayLayer;
        const uint32_t subresource = mipLevel + arrayLayer * textureInfo.levelCount;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 uploadSize = 0;
        D3D12_RESOURCE_DESC texDesc = textureResource->GetDesc();
        d3dDevice->GetCopyableFootprints(&texDesc, subresource, 1, 0, &footprint, &rowCount, &rowSizeInBytes, &uploadSize);
        if (uploadSize == 0 || rowCount == 0) continue;

        auto upload = device->allocateUploadBuffer(uploadSize + footprint.Offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!upload.isValid || !upload.mappedData || !upload.resource) continue;

        const uint32_t srcRowTexels = region.buffStride > 0 ? region.buffStride : region.texExtent.width;
        const uint32_t srcRows = region.buffTexHeight > 0 ? region.buffTexHeight : region.texExtent.height;
        const uint32_t srcRowPitch = formatSize(textureInfo.format, srcRowTexels, 1, 1);
        const uint32_t srcSlicePitch = formatSize(textureInfo.format, srcRowTexels, srcRows, 1);
        const uint32_t copyRowBytes = formatSize(textureInfo.format, region.texExtent.width, 1, 1);
        const auto blockAlignment = formatAlignment(textureInfo.format);
        const uint32_t blockHeight = std::max<uint32_t>(blockAlignment.second, 1);
        const uint32_t copyRows = std::min<uint32_t>(
            (region.texExtent.height + blockHeight - 1) / blockHeight, rowCount);
        const uint32_t copyDepth = std::max<uint32_t>(region.texExtent.depth, 1);
        const auto *src = buffers[i] + region.buffOffset;
        auto *dst = static_cast<uint8_t *>(upload.mappedData) + footprint.Offset;

        for (uint32_t z = 0; z < copyDepth; ++z) {
            for (uint32_t row = 0; row < copyRows; ++row) {
                const uint8_t *srcRow = src + z * srcSlicePitch + row * srcRowPitch;
                uint8_t *dstRow = dst + z * footprint.Footprint.RowPitch * rowCount + row * footprint.Footprint.RowPitch;
                std::memcpy(dstRow, srcRow, std::min<uint32_t>(copyRowBytes, static_cast<uint32_t>(rowSizeInBytes)));
            }
        }

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = static_cast<ID3D12Resource *>(upload.resource);
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprint;
        srcLoc.PlacedFootprint.Offset = upload.offset + footprint.Offset;

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = textureResource;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = subresource;

        D3D12_BOX srcBox{};
        srcBox.left = 0;
        srcBox.top = 0;
        srcBox.front = 0;
        srcBox.right = region.texExtent.width;
        srcBox.bottom = region.texExtent.height;
        srcBox.back = copyDepth;

        _impl->commandList->CopyTextureRegion(&dstLoc, region.texOffset.x, region.texOffset.y, region.texOffset.z, &srcLoc, &srcBox);
        d3d12Texture->markBaseMipLayerUploaded(
            mipLevel,
            arrayLayer,
            textureInfo.type == TextureType::TEX3D ? 1 : region.texSubres.layerCount);
    }

    D3D12_RESOURCE_STATES postCopyState = getPostTransferTextureState(textureInfo);
    if (hasFlag(textureInfo.flags, TextureFlagBit::GEN_MIPMAP) && textureInfo.levelCount > 1) {
        const bool shouldGenerateMipmaps = d3d12Texture->shouldGenerateMipmapsAfterUpload();
        if (shouldGenerateMipmaps &&
            generateD3D12Mipmaps(d3dDevice, _impl->commandList.Get(), textureResource, textureInfo,
                                 _impl->pendingDescriptorHeaps)) {
            d3d12Texture->markMipmapsGenerated();
            constexpr D3D12_RESOURCE_STATES GENERATED_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if (postCopyState != GENERATED_STATE) {
                D3D12_RESOURCE_BARRIER toPostCopy{};
                toPostCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                toPostCopy.Transition.pResource = textureResource;
                toPostCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                toPostCopy.Transition.StateBefore = GENERATED_STATE;
                toPostCopy.Transition.StateAfter = postCopyState;
                _impl->commandList->ResourceBarrier(1, &toPostCopy);
            }
            // Mipmap generation uses its own PSO, root signature, heaps,
            // viewport and scissor on this command list.
            invalidateGraphicsState();
            d3d12Texture->setCurrentState(postCopyState);
            return;
        }
        if (shouldGenerateMipmaps) {
            CC_LOG_WARNING("D3D12 failed to generate mipmaps for texture format %u; sampling is limited to uploaded levels.",
                           static_cast<unsigned>(textureInfo.format));
        }
    }

    // Transition back to appropriate state after copy.
    D3D12_RESOURCE_BARRIER toPostCopy{};
    toPostCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPostCopy.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    toPostCopy.Transition.pResource = textureResource;
    toPostCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toPostCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toPostCopy.Transition.StateAfter = postCopyState;
    _impl->commandList->ResourceBarrier(1, &toPostCopy);
    d3d12Texture->setCurrentState(postCopyState);
}

void CCD3D12CommandBuffer::blitTexture(Texture *srcTexture, Texture *dstTexture, const TextureBlit *regions, uint32_t count, Filter filter) {
    if (!_impl->commandList || !srcTexture || !dstTexture || !regions || count == 0) return;
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot blit textures.");
        return;
    }

    auto *srcD3D12 = static_cast<CCD3D12Texture *>(srcTexture);
    auto *dstD3D12 = static_cast<CCD3D12Texture *>(dstTexture);
    auto *srcResource = static_cast<ID3D12Resource *>(srcD3D12->getD3D12ResourceHandle());
    auto *dstResource = static_cast<ID3D12Resource *>(dstD3D12->getD3D12ResourceHandle());
    if (!srcResource || !dstResource) return;
    retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, srcResource);
    retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, dstResource);

    const auto &srcInfo = srcTexture->getInfo();
    const auto &dstInfo = dstTexture->getInfo();

    D3D12_RESOURCE_STATES srcState = srcD3D12->getCurrentState();
    D3D12_RESOURCE_STATES dstState = dstD3D12->getCurrentState();
    bool graphicsStateClobbered = false;
    auto transitionIfNeeded = [&](ID3D12Resource *resource, D3D12_RESOURCE_STATES &currentState, D3D12_RESOURCE_STATES nextState) {
        if (currentState == nextState) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = currentState;
        barrier.Transition.StateAfter = nextState;
        _impl->commandList->ResourceBarrier(1, &barrier);
        currentState = nextState;
    };

    for (uint32_t i = 0; i < count; ++i) {
        const auto &region = regions[i];

        const bool needsScaling = (region.srcExtent.width != region.dstExtent.width ||
                                    region.srcExtent.height != region.dstExtent.height ||
                                    region.srcExtent.depth != region.dstExtent.depth);
        const uint32_t layerCount = std::max<uint32_t>(std::min(region.srcSubres.layerCount, region.dstSubres.layerCount), 1);

        if (needsScaling) {
            if (!supportsShaderBlit(srcInfo, dstInfo)) {
                CC_LOG_ERROR("[D3D12] blitTexture region %u requires scaling but format/type is unsupported. "
                             "srcFormat=%u dstFormat=%u srcType=%u dstType=%u",
                             i, static_cast<unsigned>(srcInfo.format), static_cast<unsigned>(dstInfo.format),
                             static_cast<unsigned>(srcInfo.type), static_cast<unsigned>(dstInfo.type));
                continue;
            }

            transitionIfNeeded(srcResource, srcState,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transitionIfNeeded(dstResource, dstState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            graphicsStateClobbered = true;
            for (uint32_t layer = 0; layer < layerCount; ++layer) {
                const uint32_t srcLayer = region.srcSubres.baseArrayLayer + layer;
                const uint32_t dstLayer = region.dstSubres.baseArrayLayer + layer;
                if (!shaderBlitRegion(_impl->d3dDevice.Get(), _impl->commandList.Get(), srcResource, dstResource,
                                      srcInfo, dstInfo, region, srcLayer, dstLayer, filter, _impl->pendingDescriptorHeaps)) {
                    CC_LOG_ERROR("[D3D12] blitTexture shader blit failed for region %u layer %u.", i, layer);
                }
            }
            continue;
        }

        transitionIfNeeded(srcResource, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transitionIfNeeded(dstResource, dstState, D3D12_RESOURCE_STATE_COPY_DEST);

        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            const uint32_t srcSubresource = region.srcSubres.mipLevel +
                (region.srcSubres.baseArrayLayer + layer) * srcInfo.levelCount;
            const uint32_t dstSubresource = region.dstSubres.mipLevel +
                (region.dstSubres.baseArrayLayer + layer) * dstInfo.levelCount;

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = srcResource;
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = srcSubresource;

            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = dstResource;
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = dstSubresource;

            D3D12_BOX srcBox{};
            srcBox.left = static_cast<UINT>(region.srcOffset.x);
            srcBox.top = static_cast<UINT>(region.srcOffset.y);
            srcBox.front = static_cast<UINT>(region.srcOffset.z);
            srcBox.right = srcBox.left + region.srcExtent.width;
            srcBox.bottom = srcBox.top + region.srcExtent.height;
            srcBox.back = srcBox.front + std::max<uint32_t>(region.srcExtent.depth, 1);

            _impl->commandList->CopyTextureRegion(
                &dstLoc,
                region.dstOffset.x, region.dstOffset.y, region.dstOffset.z,
                &srcLoc,
                &srcBox);
        }
    }

    D3D12_RESOURCE_STATES srcPostState = getPostTransferTextureState(srcInfo);
    D3D12_RESOURCE_STATES dstPostState = getPostTransferTextureState(dstInfo);
    transitionIfNeeded(srcResource, srcState, srcPostState);
    transitionIfNeeded(dstResource, dstState, dstPostState);
    srcD3D12->setCurrentState(srcPostState);
    dstD3D12->setCurrentState(dstPostState);

    if (graphicsStateClobbered) {
        // Shader blits bind their own heaps, root signature, viewport,
        // scissor, PSO, and topology on this command list.
        invalidateGraphicsState();
    } else if (_impl->pendingSetCount > 0) {
        _impl->descriptorSetsDirty = true;
        _impl->localDescriptorSetOnlyDirty = false;
    }
}

void CCD3D12CommandBuffer::copyTexture(Texture *srcTexture, Texture *dstTexture, const TextureCopy *regions, uint32_t count) {
    if (!_impl->commandList || !srcTexture || !dstTexture || !regions || count == 0) return;
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot copy textures.");
        return;
    }

    auto *srcD3D12 = static_cast<CCD3D12Texture *>(srcTexture);
    auto *dstD3D12 = static_cast<CCD3D12Texture *>(dstTexture);
    auto *srcResource = static_cast<ID3D12Resource *>(srcD3D12->getD3D12ResourceHandle());
    auto *dstResource = static_cast<ID3D12Resource *>(dstD3D12->getD3D12ResourceHandle());
    if (!srcResource || !dstResource) {
        CC_LOG_WARNING("[D3D12] copyTexture: null resource (src=%p dst=%p)", srcResource, dstResource);
        return;
    }
    retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, srcResource);
    retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, dstResource);

    const auto &srcInfo = srcTexture->getInfo();
    const auto &dstInfo = dstTexture->getInfo();

    // Transition src to COPY_SOURCE, dst to COPY_DEST
    D3D12_RESOURCE_BARRIER preBarriers[4];
    uint32_t preBarrierCount = 0;

    D3D12_RESOURCE_STATES srcPrevState = srcD3D12->getCurrentState();
    if (srcPrevState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = srcPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (preBarrierCount < 4) preBarriers[preBarrierCount++] = b;
    }

    D3D12_RESOURCE_STATES dstPrevState = dstD3D12->getCurrentState();
    if (dstPrevState != D3D12_RESOURCE_STATE_COPY_DEST) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = dstPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        if (preBarrierCount < 4) preBarriers[preBarrierCount++] = b;
    }

    if (preBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(preBarrierCount, preBarriers);
    }
    srcD3D12->setCurrentState(D3D12_RESOURCE_STATE_COPY_SOURCE);
    dstD3D12->setCurrentState(D3D12_RESOURCE_STATE_COPY_DEST);

    // Perform texture-to-texture copy for each region
    for (uint32_t i = 0; i < count; ++i) {
        const auto &region = regions[i];
        const uint32_t layerCount = std::max<uint32_t>(std::min(region.srcSubres.layerCount, region.dstSubres.layerCount), 1);

        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            const uint32_t srcSubresource = region.srcSubres.mipLevel +
                (region.srcSubres.baseArrayLayer + layer) * srcInfo.levelCount;
            const uint32_t dstSubresource = region.dstSubres.mipLevel +
                (region.dstSubres.baseArrayLayer + layer) * dstInfo.levelCount;

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = srcResource;
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = srcSubresource;

            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = dstResource;
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = dstSubresource;

            D3D12_BOX srcBox{};
            srcBox.left = static_cast<UINT>(region.srcOffset.x);
            srcBox.top = static_cast<UINT>(region.srcOffset.y);
            srcBox.front = static_cast<UINT>(region.srcOffset.z);
            srcBox.right = srcBox.left + region.extent.width;
            srcBox.bottom = srcBox.top + region.extent.height;
            srcBox.back = srcBox.front + std::max<uint32_t>(region.extent.depth, 1);

            _impl->commandList->CopyTextureRegion(
                &dstLoc,
                region.dstOffset.x, region.dstOffset.y, region.dstOffset.z,
                &srcLoc,
                &srcBox);
        }
    }

    // Transition src and dst back to reasonable states after copy
    D3D12_RESOURCE_BARRIER postBarriers[4];
    uint32_t postBarrierCount = 0;

    // Src back to shader resource
    D3D12_RESOURCE_STATES srcPostState = getPostTransferTextureState(srcInfo);
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = srcPostState;
        if (postBarrierCount < 4) postBarriers[postBarrierCount++] = b;
    }

    // Dst back to shader resource (or render target)
    D3D12_RESOURCE_STATES dstPostState = getPostTransferTextureState(dstInfo);
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = dstPostState;
        if (postBarrierCount < 4) postBarriers[postBarrierCount++] = b;
    }

    if (postBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(postBarrierCount, postBarriers);
    }
    srcD3D12->setCurrentState(srcPostState);
    dstD3D12->setCurrentState(dstPostState);
}

void CCD3D12CommandBuffer::resolveTexture(Texture *srcTexture, Texture *dstTexture, const TextureCopy *regions, uint32_t count) {
    if (!_impl->commandList || !srcTexture || !dstTexture || !regions || count == 0) return;
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot resolve textures.");
        return;
    }

    auto *srcD3D12 = static_cast<CCD3D12Texture *>(srcTexture);
    auto *dstD3D12 = static_cast<CCD3D12Texture *>(dstTexture);
    auto *srcResource = static_cast<ID3D12Resource *>(srcD3D12->getD3D12ResourceHandle());
    auto *dstResource = static_cast<ID3D12Resource *>(dstD3D12->getD3D12ResourceHandle());
    if (!srcResource || !dstResource) return;
    retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, srcResource);
    retainCommandListResource(_impl->pendingUploadResources, _impl->pendingUploadResourceSet, dstResource);

    const auto &srcInfo = srcTexture->getInfo();
    const auto &dstInfo = dstTexture->getInfo();

    // Verify src is MSAA and dst is non-MSAA
    const bool srcIsMSAA = (srcInfo.samples != SampleCount::X1);
    if (!srcIsMSAA) {
        CC_LOG_WARNING("[D3D12] resolveTexture: source is not MSAA (samples=%u), falling back to copyTexture",
                       srcInfo.samples);
        // Fall back to regular copy for non-MSAA sources
        copyTexture(srcTexture, dstTexture, regions, count);
        return;
    }

    // Transition src to RESOLVE_SOURCE, dst to RESOLVE_DEST
    D3D12_RESOURCE_BARRIER preBarriers[4];
    uint32_t preBarrierCount = 0;

    D3D12_RESOURCE_STATES srcPrevState = srcD3D12->getCurrentState();
    if (srcPrevState != D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = srcPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        if (preBarrierCount < 4) preBarriers[preBarrierCount++] = b;
    }

    D3D12_RESOURCE_STATES dstPrevState = dstD3D12->getCurrentState();
    if (dstPrevState != D3D12_RESOURCE_STATE_RESOLVE_DEST) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = dstPrevState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        if (preBarrierCount < 4) preBarriers[preBarrierCount++] = b;
    }

    if (preBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(preBarrierCount, preBarriers);
    }
    srcD3D12->setCurrentState(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    dstD3D12->setCurrentState(D3D12_RESOURCE_STATE_RESOLVE_DEST);

    // ResolveSubresource operates on a single subresource pair at a time.
    // For each region, resolve the corresponding mip+layer combination.
    for (uint32_t i = 0; i < count; ++i) {
        const auto &region = regions[i];
        const uint32_t layerCount = std::max<uint32_t>(std::min(region.srcSubres.layerCount, region.dstSubres.layerCount), 1);

        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            const uint32_t srcSubresource = region.srcSubres.mipLevel +
                (region.srcSubres.baseArrayLayer + layer) * srcInfo.levelCount;
            const uint32_t dstSubresource = region.dstSubres.mipLevel +
                (region.dstSubres.baseArrayLayer + layer) * dstInfo.levelCount;

            // ResolveSubresource requires a DXGI format when the source and destination
            // formats differ (format conversion resolve). For same-format resolves, pass
            // DXGI_FORMAT_UNKNOWN which lets the runtime use the source format.
            _impl->commandList->ResolveSubresource(
                dstResource, dstSubresource,
                srcResource, srcSubresource,
                DXGI_FORMAT_UNKNOWN);
        }
    }

    // Transition back to shader resource after resolve
    D3D12_RESOURCE_BARRIER postBarriers[4];
    uint32_t postBarrierCount = 0;

    {
        D3D12_RESOURCE_STATES srcPostState = getPostTransferTextureState(srcInfo);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = srcResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        b.Transition.StateAfter = srcPostState;
        if (postBarrierCount < 4) postBarriers[postBarrierCount++] = b;
        srcD3D12->setCurrentState(srcPostState);
    }
    {
        D3D12_RESOURCE_STATES dstPostState = getPostTransferTextureState(dstInfo);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = dstResource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        b.Transition.StateAfter = dstPostState;
        if (postBarrierCount < 4) postBarriers[postBarrierCount++] = b;
        dstD3D12->setCurrentState(dstPostState);
    }

    if (postBarrierCount > 0) {
        _impl->commandList->ResourceBarrier(postBarrierCount, postBarriers);
    }
}

void CCD3D12CommandBuffer::dispatch(const DispatchInfo &info) {
    if (!_impl->commandList) return;
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot dispatch compute work.");
        return;
    }
    CCD3D12Device::getInstance()->flushPendingBufferUpdates(this);
    // Flush any pending descriptor set bindings before dispatching
    flushDescriptorSets();

    if (info.indirectBuffer) {
        // Indirect dispatch — arguments come from a GPU buffer
        auto *d3d12Buf = static_cast<CCD3D12Buffer *>(info.indirectBuffer);
        ID3D12Resource *resource = static_cast<ID3D12Resource *>(d3d12Buf->getD3D12ResourceHandle());
        if (resource) {
            auto *sig = static_cast<ID3D12CommandSignature *>(
                CCD3D12Device::getInstance()->getDispatchIndirectSignature());
            if (sig) {
                _impl->commandList->ExecuteIndirect(sig, 1, resource, info.indirectOffset, nullptr, 0);
            }
        }
    } else {
        _impl->commandList->Dispatch(info.groupCountX, info.groupCountY, info.groupCountZ);
    }
}

void CCD3D12CommandBuffer::pipelineBarrier(const GeneralBarrier *barrier, const BufferBarrier *const *bufferBarriers, const Buffer *const *buffers, uint32_t bufferCount, const TextureBarrier *const *textureBarriers, const Texture *const *textures, uint32_t textureBarrierCount) {
    if (!_impl->commandList) return;
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot record resource barriers.");
        return;
    }

    auto hasWriteAccess = [](AccessFlags access) {
        return hasFlag(access, AccessFlagBit::VERTEX_SHADER_WRITE) ||
               hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_WRITE) ||
               hasFlag(access, AccessFlagBit::COLOR_ATTACHMENT_WRITE) ||
               hasFlag(access, AccessFlagBit::DEPTH_STENCIL_ATTACHMENT_WRITE) ||
               hasFlag(access, AccessFlagBit::COMPUTE_SHADER_WRITE) ||
               hasFlag(access, AccessFlagBit::TRANSFER_WRITE) ||
               hasFlag(access, AccessFlagBit::HOST_WRITE);
    };

    // Map Cocos AccessFlags to D3D12_RESOURCE_STATES
    auto accessFlagsToD3D12State = [](AccessFlags access) -> D3D12_RESOURCE_STATES {
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        if (hasFlag(access, AccessFlagBit::COLOR_ATTACHMENT_WRITE) ||
            hasFlag(access, AccessFlagBit::COLOR_ATTACHMENT_READ)) {
            state |= D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        if (hasFlag(access, AccessFlagBit::DEPTH_STENCIL_ATTACHMENT_WRITE)) {
            state |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
        } else if (hasFlag(access, AccessFlagBit::DEPTH_STENCIL_ATTACHMENT_READ)) {
            state |= D3D12_RESOURCE_STATE_DEPTH_READ;
        }
        if (hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_READ_TEXTURE) ||
            hasFlag(access, AccessFlagBit::VERTEX_SHADER_READ_TEXTURE) ||
            hasFlag(access, AccessFlagBit::COMPUTE_SHADER_READ_TEXTURE)) {
            state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        if (hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_READ_COLOR_INPUT_ATTACHMENT)) {
            state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        if (hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_READ_DEPTH_STENCIL_INPUT_ATTACHMENT)) {
            state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        if (hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_WRITE) ||
            hasFlag(access, AccessFlagBit::COMPUTE_SHADER_WRITE)) {
            state |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (hasFlag(access, AccessFlagBit::VERTEX_SHADER_READ_UNIFORM_BUFFER) ||
            hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_READ_UNIFORM_BUFFER) ||
            hasFlag(access, AccessFlagBit::COMPUTE_SHADER_READ_UNIFORM_BUFFER)) {
            state |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        }
        if (hasFlag(access, AccessFlagBit::INDEX_BUFFER)) {
            state |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
        }
        if (hasFlag(access, AccessFlagBit::VERTEX_BUFFER)) {
            state |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        }
        if (hasFlag(access, AccessFlagBit::INDIRECT_BUFFER)) {
            state |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        }
        if (hasFlag(access, AccessFlagBit::TRANSFER_READ)) {
            state |= D3D12_RESOURCE_STATE_COPY_SOURCE;
        }
        if (hasFlag(access, AccessFlagBit::TRANSFER_WRITE)) {
            state |= D3D12_RESOURCE_STATE_COPY_DEST;
        }
        if (hasFlag(access, AccessFlagBit::PRESENT)) {
            state = D3D12_RESOURCE_STATE_PRESENT;
        }
        if (hasFlag(access, AccessFlagBit::VERTEX_SHADER_READ_OTHER) ||
            hasFlag(access, AccessFlagBit::FRAGMENT_SHADER_READ_OTHER) ||
            hasFlag(access, AccessFlagBit::COMPUTE_SHADER_READ_OTHER)) {
            state |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        return state;
    };

    ccstd::vector<D3D12_RESOURCE_BARRIER> barriers;
    uint32_t textureTransitions = 0;
    uint32_t bufferTransitions = 0;
    uint32_t uavBarriers = 0;
    uint32_t trackedAlreadyNext = 0;

    // Process texture barriers
    for (uint32_t i = 0; i < textureBarrierCount; ++i) {
        if (!textureBarriers[i] || !textures[i]) continue;

        const auto &texBarrierInfo = textureBarriers[i]->getInfo();
        if (texBarrierInfo.type == BarrierType::SPLIT_BEGIN) {
            continue;
        }

        auto *d3d12Texture = static_cast<CCD3D12Texture *>(const_cast<Texture *>(textures[i]));
        if (!d3d12Texture) continue;

        auto *resource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
        if (!resource) continue;

        // Skip swapchain textures only during a render pass — their barriers are
        // managed by beginRenderPass/endRenderPass. Outside a render pass, allow
        // barriers for swapchain textures (e.g., for copy operations).
        if (d3d12Texture->isSwapchainColorTexture() && _impl->inRenderPass) continue;

        D3D12_RESOURCE_STATES prevState = accessFlagsToD3D12State(texBarrierInfo.prevAccesses);
        D3D12_RESOURCE_STATES nextState = accessFlagsToD3D12State(texBarrierInfo.nextAccesses);

        // If no explicit prevAccesses, use tracked state
        if (texBarrierInfo.prevAccesses == AccessFlagBit::NONE) {
            prevState = d3d12Texture->getCurrentState();
        }

        // If states match, no barrier needed
        if (prevState == nextState) continue;

        const auto &range = texBarrierInfo.range;
        const auto &textureInfo = d3d12Texture->getInfo();
        const uint32_t mipCount = std::max<uint32_t>(textureInfo.levelCount, 1);
        const uint32_t arraySize = std::max<uint32_t>(textureInfo.layerCount, 1);
        const uint32_t planeCount = std::max<uint32_t>(range.planeCount, 1);
        const bool hasExplicitRange = range.numSlices > 0 || range.levelCount > 0 || range.planeCount > 0;
        const uint32_t firstSlice = std::min<uint32_t>(range.firstSlice, arraySize - 1);
        const uint32_t sliceCount = range.numSlices > 0 ? std::min<uint32_t>(range.numSlices, arraySize - firstSlice) : arraySize;
        const uint32_t firstMip = std::min<uint32_t>(range.mipLevel, mipCount - 1);
        const uint32_t levelCount = range.levelCount > 0 ? std::min<uint32_t>(range.levelCount, mipCount - firstMip) : mipCount;
        const uint32_t firstPlane = range.basePlane;

        if (!hasExplicitRange) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            b.Transition.pResource = resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = prevState;
            b.Transition.StateAfter = nextState;
            barriers.push_back(b);
            ++textureTransitions;
        } else {
            for (uint32_t plane = 0; plane < planeCount; ++plane) {
                for (uint32_t slice = 0; slice < sliceCount; ++slice) {
                    for (uint32_t mip = 0; mip < levelCount; ++mip) {
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                        b.Transition.pResource = resource;
                        b.Transition.Subresource = (firstMip + mip) +
                                                   (firstSlice + slice) * mipCount +
                                                   (firstPlane + plane) * mipCount * arraySize;
                        b.Transition.StateBefore = prevState;
                        b.Transition.StateAfter = nextState;
                        barriers.push_back(b);
                        ++textureTransitions;
                    }
                }
            }
        }

        // Update tracked state
        d3d12Texture->setCurrentState(nextState);
    }

    // Process buffer barriers.
    for (uint32_t i = 0; i < bufferCount; ++i) {
        if (!bufferBarriers[i] || !buffers[i]) continue;

        const auto &bufBarrierInfo = bufferBarriers[i]->getInfo();
        if (bufBarrierInfo.type == BarrierType::SPLIT_BEGIN) {
            continue;
        }

        auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(const_cast<Buffer *>(buffers[i]));
        if (!d3d12Buffer) {
            continue;
        }

        auto *resource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
        if (!resource) {
            continue;
        }

        D3D12_RESOURCE_STATES prevState = accessFlagsToD3D12State(bufBarrierInfo.prevAccesses);
        D3D12_RESOURCE_STATES nextState = accessFlagsToD3D12State(bufBarrierInfo.nextAccesses);
        const D3D12_RESOURCE_STATES trackedState = d3d12Buffer->getCurrentState();

        if (bufBarrierInfo.prevAccesses == AccessFlagBit::NONE) {
            prevState = trackedState;
        }

        const bool needsTransition = prevState != nextState;
        const bool trackedAlreadyInNextState = bufBarrierInfo.prevAccesses != AccessFlagBit::NONE &&
                                               needsTransition && trackedState == nextState;
        if (trackedAlreadyInNextState) {
            ++trackedAlreadyNext;
        }
        if (needsTransition) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            b.Transition.pResource = resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = prevState;
            b.Transition.StateAfter = nextState;
            barriers.push_back(b);
            ++bufferTransitions;
            d3d12Buffer->setCurrentState(nextState);
        }

        const bool uavRelated = (prevState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0 ||
                                (nextState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0;
        // If a UAV buffer is written and remains in the same state, preserve
        // write ordering with a UAV barrier. Transitions already provide the
        // required ordering for state changes.
        if (!needsTransition && uavRelated && hasWriteAccess(bufBarrierInfo.prevAccesses)) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            b.UAV.pResource = resource;
            barriers.push_back(b);
            ++uavBarriers;
        }
    }

    if (barrier) {
        const auto &barrierInfo = barrier->getInfo();
        if (barrierInfo.type != BarrierType::SPLIT_BEGIN &&
            (hasWriteAccess(barrierInfo.prevAccesses) || hasWriteAccess(barrierInfo.nextAccesses))) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            b.UAV.pResource = nullptr;
            barriers.push_back(b);
            ++uavBarriers;
        }
    }


    if (!barriers.empty()) {
        _impl->commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }
}

void CCD3D12CommandBuffer::beginQuery(QueryPool *queryPool, uint32_t id) {
    if (!_impl || !_impl->commandList) return;
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot begin queries.");
        return;
    }
    auto *d3d12Pool = static_cast<CCD3D12QueryPool *>(queryPool);
    auto *heap = static_cast<ID3D12QueryHeap *>(d3d12Pool->getD3D12QueryHeap());
    if (!heap) return;
    const uint32_t queryIndex = d3d12Pool->beginD3D12Query(id);
    if (queryIndex == std::numeric_limits<uint32_t>::max()) return;
    if (d3d12Pool->getType() == QueryType::OCCLUSION) {
        _impl->commandList->BeginQuery(heap, D3D12_QUERY_TYPE_OCCLUSION, queryIndex);
    }
}

void CCD3D12CommandBuffer::endQuery(QueryPool *queryPool, uint32_t id) {
    if (!_impl || !_impl->commandList) return;
    if (_type == CommandBufferType::SECONDARY) {
        CC_LOG_ERROR("D3D12 bundle cannot end queries.");
        return;
    }
    auto *d3d12Pool = static_cast<CCD3D12QueryPool *>(queryPool);
    auto *heap = static_cast<ID3D12QueryHeap *>(d3d12Pool->getD3D12QueryHeap());
    if (!heap) return;
    const uint32_t queryIndex = d3d12Pool->endD3D12Query(id);
    if (queryIndex == std::numeric_limits<uint32_t>::max()) return;
    const D3D12_QUERY_TYPE queryType = d3d12Pool->getType() == QueryType::TIMESTAMP
                                           ? D3D12_QUERY_TYPE_TIMESTAMP
                                           : D3D12_QUERY_TYPE_OCCLUSION;
    _impl->commandList->EndQuery(heap, queryType, queryIndex);
}

void CCD3D12CommandBuffer::resetQueryPool(QueryPool *queryPool) {
    if (!_impl || !_impl->commandList) return;
    auto *d3d12Pool = static_cast<CCD3D12QueryPool *>(queryPool);
    // D3D12 doesn't have a direct "reset query pool" on command list.
    // The results are overwritten on next BeginQuery/EndQuery cycle.
    // We clear the CPU-side results here.
    d3d12Pool->resetD3D12Queries();
}

void CCD3D12CommandBuffer::customCommand(CustomCommand &&cmd) {
    if (cmd && _impl && _impl->commandList) {
        cmd(static_cast<void *>(_impl->commandList.Get()));
    }
}

void *CCD3D12CommandBuffer::getD3D12CommandList() const {
    return _impl ? _impl->commandList.Get() : nullptr;
}

} // namespace gfx
} // namespace cc
