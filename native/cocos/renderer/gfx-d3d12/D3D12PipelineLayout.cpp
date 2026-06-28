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

#include "D3D12PipelineLayout.h"
#include "D3D12DescriptorSetLayout.h"
#include "D3D12Device.h"
#include "base/Log.h"
#include "gfx-base/GFXDef.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <chrono>
    #include <d3d12.h>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

namespace {
using D3D12PerfClock = std::chrono::steady_clock;

uint64_t elapsedMs(D3D12PerfClock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(D3D12PerfClock::now() - start).count());
}

D3D12_SHADER_VISIBILITY toD3D12ShaderVisibility(ShaderStageFlags stageFlags) {
    if (hasAnyFlags(stageFlags, ShaderStageFlagBit::ALL)) {
        return D3D12_SHADER_VISIBILITY_ALL;
    }
    // If multiple stages, use ALL
    uint32_t count = 0;
    if (hasFlag(stageFlags, ShaderStageFlagBit::VERTEX)) ++count;
    if (hasFlag(stageFlags, ShaderStageFlagBit::FRAGMENT)) ++count;
    if (hasFlag(stageFlags, ShaderStageFlagBit::COMPUTE)) ++count;
    if (hasFlag(stageFlags, ShaderStageFlagBit::GEOMETRY)) ++count;
    if (hasFlag(stageFlags, ShaderStageFlagBit::CONTROL)) ++count;
    if (hasFlag(stageFlags, ShaderStageFlagBit::EVALUATION)) ++count;

    if (count > 1) return D3D12_SHADER_VISIBILITY_ALL;
    if (hasFlag(stageFlags, ShaderStageFlagBit::VERTEX)) return D3D12_SHADER_VISIBILITY_VERTEX;
    if (hasFlag(stageFlags, ShaderStageFlagBit::FRAGMENT)) return D3D12_SHADER_VISIBILITY_PIXEL;
    if (hasFlag(stageFlags, ShaderStageFlagBit::COMPUTE)) return D3D12_SHADER_VISIBILITY_ALL;
    if (hasFlag(stageFlags, ShaderStageFlagBit::GEOMETRY)) return D3D12_SHADER_VISIBILITY_GEOMETRY;
    if (hasFlag(stageFlags, ShaderStageFlagBit::CONTROL)) return D3D12_SHADER_VISIBILITY_HULL;
    if (hasFlag(stageFlags, ShaderStageFlagBit::EVALUATION)) return D3D12_SHADER_VISIBILITY_DOMAIN;
    return D3D12_SHADER_VISIBILITY_ALL;
}

D3D12_DESCRIPTOR_RANGE_TYPE toD3D12DescriptorRangeType(DescriptorType type) {
    switch (type) {
        case DescriptorType::UNIFORM_BUFFER:
        case DescriptorType::DYNAMIC_UNIFORM_BUFFER:
            return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        case DescriptorType::STORAGE_BUFFER:
        case DescriptorType::DYNAMIC_STORAGE_BUFFER:
            return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        case DescriptorType::SAMPLER_TEXTURE:
        case DescriptorType::SAMPLER:
            return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        case DescriptorType::TEXTURE:
        case DescriptorType::INPUT_ATTACHMENT:
            return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        case DescriptorType::STORAGE_IMAGE:
            return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        default:
            return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    }
}

} // namespace

struct CCD3D12PipelineLayout::Impl {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    ccstd::vector<int32_t> cbvSrvUavRootParameterIndices;
    ccstd::vector<int32_t> samplerRootParameterIndices;
};

CCD3D12PipelineLayout::CCD3D12PipelineLayout()
: _impl(std::make_unique<Impl>()) {
}

CCD3D12PipelineLayout::~CCD3D12PipelineLayout() {
    destroy();
}

void CCD3D12PipelineLayout::doInit(const PipelineLayoutInfo &info) {
    (void)info;
    const auto initStart = D3D12PerfClock::now();
    _impl->cbvSrvUavRootParameterIndices.assign(_setLayouts.size(), -1);
    _impl->samplerRootParameterIndices.assign(_setLayouts.size(), -1);

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12PipelineLayout: device unavailable.");
        return;
    }

    // Collect all descriptor ranges and root parameters from all set layouts
    ccstd::vector<D3D12_DESCRIPTOR_RANGE> allRanges;
    ccstd::vector<D3D12_ROOT_PARAMETER> rootParameters;

    uint32_t setIndex = 0;
    for (auto *setLayout : _setLayouts) {
        auto *d3d12Layout = static_cast<const CCD3D12DescriptorSetLayout *>(setLayout);
        if (!d3d12Layout) {
            ++setIndex;
            continue;
        }

        const auto &bindings = d3d12Layout->getBindings();

        // Separate ranges by heap type: Sampler vs CBV/SRV/UAV
        ccstd::vector<D3D12_DESCRIPTOR_RANGE> samplerRanges;
        ccstd::vector<D3D12_DESCRIPTOR_RANGE> cbvSrvUavRanges;

        for (const auto &binding : bindings) {
            DescriptorType descType = binding.descriptorType;
            D3D12_SHADER_VISIBILITY visibility = toD3D12ShaderVisibility(binding.stageFlags);

            if (descType == DescriptorType::SAMPLER_TEXTURE) {
                // SAMPLER_TEXTURE produces two ranges: one SRV and one SAMPLER
                uint32_t baseRegisterSRV = static_cast<uint32_t>(cbvSrvUavRanges.size());
                D3D12_DESCRIPTOR_RANGE srvRange{};
                srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                srvRange.NumDescriptors = binding.count;
                srvRange.BaseShaderRegister = binding.binding;
                srvRange.RegisterSpace = setIndex;
                srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                cbvSrvUavRanges.push_back(srvRange);

                uint32_t baseRegisterSampler = static_cast<uint32_t>(samplerRanges.size());
                D3D12_DESCRIPTOR_RANGE samplerRange{};
                samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                samplerRange.NumDescriptors = binding.count;
                samplerRange.BaseShaderRegister = binding.binding;
                samplerRange.RegisterSpace = setIndex;
                samplerRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                samplerRanges.push_back(samplerRange);
            } else if (descType == DescriptorType::SAMPLER) {
                D3D12_DESCRIPTOR_RANGE range{};
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                range.NumDescriptors = binding.count;
                range.BaseShaderRegister = binding.binding;
                range.RegisterSpace = setIndex;
                range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                samplerRanges.push_back(range);
            } else {
                // Buffer, Texture, StorageImage, InputAttachment -> CBV/SRV/UAV
                D3D12_DESCRIPTOR_RANGE range{};
                range.RangeType = toD3D12DescriptorRangeType(descType);
                range.NumDescriptors = binding.count;
                range.BaseShaderRegister = binding.binding;
                range.RegisterSpace = setIndex;
                range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                cbvSrvUavRanges.push_back(range);
            }
        }

        // Use ALL visibility for root parameters — D3D12 root signature must match
        // shader resource declarations which may be used by any stage.
        // Using per-binding visibility risks mismatches when the same descriptor
        // set is accessed from multiple shader stages.
        D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;

        if (!cbvSrvUavRanges.empty()) {
            _impl->cbvSrvUavRootParameterIndices[setIndex] = static_cast<int32_t>(rootParameters.size());
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.ShaderVisibility = visibility;
            param.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(cbvSrvUavRanges.size());
            param.DescriptorTable.pDescriptorRanges = nullptr; // will fix up after collection
            rootParameters.push_back(param);

            // Append ranges to the global pool
            for (auto &r : cbvSrvUavRanges) {
                allRanges.push_back(r);
            }
        } else {
            // Empty set layout: still create a CBV/SRV/UAV root parameter with a
            // dummy range so that flushDescriptorSets can bind this set index.
            // Without this, sets with no bindings (e.g. PER_PASS global set when
            // the pass has no UBO declarations) would have rootParameterIndex = -1,
            // and bindDescriptorSet for that set would be silently skipped.
            // The dummy range maps 1 CBV at register 0 in this set's space.
            _impl->cbvSrvUavRootParameterIndices[setIndex] = static_cast<int32_t>(rootParameters.size());
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.ShaderVisibility = visibility;
            param.DescriptorTable.NumDescriptorRanges = 1;
            param.DescriptorTable.pDescriptorRanges = nullptr;
            rootParameters.push_back(param);

            D3D12_DESCRIPTOR_RANGE dummyRange{};
            dummyRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            dummyRange.NumDescriptors = 1;
            dummyRange.BaseShaderRegister = 0;
            dummyRange.RegisterSpace = setIndex;
            dummyRange.OffsetInDescriptorsFromTableStart = 0;
            allRanges.push_back(dummyRange);
        }

        if (!samplerRanges.empty()) {
            _impl->samplerRootParameterIndices[setIndex] = static_cast<int32_t>(rootParameters.size());
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.ShaderVisibility = visibility;
            param.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(samplerRanges.size());
            param.DescriptorTable.pDescriptorRanges = nullptr; // will fix up after collection
            rootParameters.push_back(param);

            for (auto &r : samplerRanges) {
                allRanges.push_back(r);
            }
        }

        ++setIndex;
    }

    // Now we need to fix up the pDescriptorRanges pointers to point into allRanges
    // We iterate again, counting ranges per parameter
    uint32_t rangeIdx = 0;
    for (auto &param : rootParameters) {
        uint32_t numRanges = param.DescriptorTable.NumDescriptorRanges;
        param.DescriptorTable.pDescriptorRanges = &allRanges[rangeIdx];
        rangeIdx += numRanges;
    }

    // Build root signature desc
    D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
    rootSigDesc.NumParameters = static_cast<UINT>(rootParameters.size());
    rootSigDesc.pParameters = rootParameters.empty() ? nullptr : rootParameters.data();
    rootSigDesc.NumStaticSamplers = 0;
    rootSigDesc.pStaticSamplers = nullptr;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // Serialize the root signature
    Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    const auto serializeStart = D3D12PerfClock::now();
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &signatureBlob, &errorBlob);
    const auto serializeMs = elapsedMs(serializeStart);
    if (FAILED(hr)) {
        if (errorBlob) {
            CC_LOG_ERROR("D3D12PipelineLayout: D3D12SerializeRootSignature failed: %s",
                         static_cast<const char *>(errorBlob->GetBufferPointer()));
        } else {
            CC_LOG_ERROR("D3D12PipelineLayout: D3D12SerializeRootSignature failed. HRESULT=0x%08x",
                         static_cast<unsigned>(hr));
        }
        return;
    }

    const auto createStart = D3D12PerfClock::now();
    hr = d3dDevice->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
                                         signatureBlob->GetBufferSize(),
                                         IID_PPV_ARGS(&_impl->rootSignature));
    const auto createMs = elapsedMs(createStart);
    if (FAILED(hr)) {
        CC_LOG_ERROR("D3D12PipelineLayout: CreateRootSignature failed. HRESULT=0x%08x",
                     static_cast<unsigned>(hr));
        return;
    }

    CC_LOG_INFO("D3D12 PipelineLayout initialized: %u set layouts, %u root parameters",
                static_cast<uint32_t>(_setLayouts.size()),
                static_cast<uint32_t>(rootParameters.size()));
    CC_LOG_INFO("[D3D12-PERF] PipelineLayoutInit setLayouts=%u rootParameters=%u descriptorRanges=%u serializeRootSigMs=%llu createRootSigMs=%llu totalMs=%llu",
                static_cast<uint32_t>(_setLayouts.size()),
                static_cast<uint32_t>(rootParameters.size()),
                static_cast<uint32_t>(allRanges.size()),
                static_cast<unsigned long long>(serializeMs),
                static_cast<unsigned long long>(createMs),
                static_cast<unsigned long long>(elapsedMs(initStart)));
}

void CCD3D12PipelineLayout::doDestroy() {
    if (_impl && _impl->rootSignature) {
        _impl->rootSignature.Reset();
    }
    _impl->cbvSrvUavRootParameterIndices.clear();
    _impl->samplerRootParameterIndices.clear();
}

void *CCD3D12PipelineLayout::getID3D12RootSignature() const {
    return _impl ? _impl->rootSignature.Get() : nullptr;
}

int32_t CCD3D12PipelineLayout::getCbvSrvUavRootParameterIndex(uint32_t set) const {
    if (!_impl || set >= _impl->cbvSrvUavRootParameterIndices.size()) {
        return -1;
    }
    return _impl->cbvSrvUavRootParameterIndices[set];
}

int32_t CCD3D12PipelineLayout::getSamplerRootParameterIndex(uint32_t set) const {
    if (!_impl || set >= _impl->samplerRootParameterIndices.size()) {
        return -1;
    }
    return _impl->samplerRootParameterIndices[set];
}

} // namespace gfx
} // namespace cc
