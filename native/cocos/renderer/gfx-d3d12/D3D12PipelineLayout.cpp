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

static constexpr uint32_t D3D12_LOCAL_DESCRIPTOR_SET_INDEX = 2;
// Local set is split only when CommandBuffer can reuse the static suffix by an
// exact resource comparison in the current descriptor-heap epoch.
static constexpr bool D3D12_ENABLE_LOCAL_ROOT_TABLE_SPLIT = true;

namespace {
using D3D12PerfClock = std::chrono::steady_clock;

uint64_t elapsedMs(D3D12PerfClock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(D3D12PerfClock::now() - start).count());
}

uint64_t hashRootSignatureBlob(const void *data, size_t size) {
    constexpr uint64_t FNV1A64_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV1A64_PRIME = 1099511628211ULL;
    uint64_t hash = FNV1A64_OFFSET;
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= FNV1A64_PRIME;
    }
    return hash;
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
    uint64_t rootSignatureHash{0};
    ccstd::vector<int32_t> cbvSrvUavRootParameterIndices;
    ccstd::vector<int32_t> dynamicCbvSrvUavRootParameterIndices;
    ccstd::vector<int32_t> localRootCbvParameterIndices;
    ccstd::vector<int32_t> staticCbvSrvUavRootParameterIndices;
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
    _impl->dynamicCbvSrvUavRootParameterIndices.assign(_setLayouts.size(), -1);
    _impl->localRootCbvParameterIndices.assign(_setLayouts.size(), -1);
    _impl->staticCbvSrvUavRootParameterIndices.assign(_setLayouts.size(), -1);
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
        ccstd::vector<D3D12_DESCRIPTOR_RANGE> dynamicBufferCbvSrvUavRanges;
        ccstd::vector<D3D12_DESCRIPTOR_RANGE> staticCbvSrvUavRanges;
        bool localPartitionValid = setIndex == D3D12_LOCAL_DESCRIPTOR_SET_INDEX;
        bool localRootCbvAssigned = false;

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
                if (localPartitionValid) {
                    staticCbvSrvUavRanges.push_back(srvRange);
                }

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
                if (localPartitionValid) {
                    if (!localRootCbvAssigned && descType == DescriptorType::UNIFORM_BUFFER &&
                        binding.binding == 0 && binding.count > 0) {
                        localRootCbvAssigned = true;
                        if (binding.count > 1) {
                            D3D12_DESCRIPTOR_RANGE suffixRange = range;
                            suffixRange.NumDescriptors = binding.count - 1;
                            suffixRange.BaseShaderRegister = binding.binding + 1;
                            staticCbvSrvUavRanges.push_back(suffixRange);
                        }
                    } else if (descType == DescriptorType::DYNAMIC_UNIFORM_BUFFER ||
                               descType == DescriptorType::DYNAMIC_STORAGE_BUFFER) {
                        dynamicBufferCbvSrvUavRanges.push_back(range);
                    } else {
                        staticCbvSrvUavRanges.push_back(range);
                    }
                }
            }
        }

        // Use ALL visibility for root parameters — D3D12 root signature must match
        // shader resource declarations which may be used by any stage.
        // Using per-binding visibility risks mismatches when the same descriptor
        // set is accessed from multiple shader stages.
        D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;

        const bool splitLocalCbvSrvUav = D3D12_ENABLE_LOCAL_ROOT_TABLE_SPLIT && localPartitionValid &&
                                         localRootCbvAssigned &&
                                         !staticCbvSrvUavRanges.empty();
        if (splitLocalCbvSrvUav) {
            _impl->localRootCbvParameterIndices[setIndex] = static_cast<int32_t>(rootParameters.size());
            D3D12_ROOT_PARAMETER rootCbvParam{};
            rootCbvParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rootCbvParam.ShaderVisibility = visibility;
            rootCbvParam.Descriptor.ShaderRegister = 0;
            rootCbvParam.Descriptor.RegisterSpace = setIndex;
            rootParameters.push_back(rootCbvParam);

            if (!dynamicBufferCbvSrvUavRanges.empty()) {
                _impl->dynamicCbvSrvUavRootParameterIndices[setIndex] = static_cast<int32_t>(rootParameters.size());
                D3D12_ROOT_PARAMETER dynamicParam{};
                dynamicParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                dynamicParam.ShaderVisibility = visibility;
                dynamicParam.DescriptorTable.NumDescriptorRanges =
                    static_cast<UINT>(dynamicBufferCbvSrvUavRanges.size());
                rootParameters.push_back(dynamicParam);
                allRanges.insert(allRanges.end(), dynamicBufferCbvSrvUavRanges.begin(),
                                 dynamicBufferCbvSrvUavRanges.end());
            }

            _impl->staticCbvSrvUavRootParameterIndices[setIndex] = static_cast<int32_t>(rootParameters.size());
            D3D12_ROOT_PARAMETER staticParam{};
            staticParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            staticParam.ShaderVisibility = visibility;
            staticParam.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(staticCbvSrvUavRanges.size());
            rootParameters.push_back(staticParam);
            allRanges.insert(allRanges.end(), staticCbvSrvUavRanges.begin(), staticCbvSrvUavRanges.end());
        } else if (!cbvSrvUavRanges.empty()) {
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
        if (param.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            continue;
        }
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
    _impl->rootSignatureHash = hashRootSignatureBlob(signatureBlob->GetBufferPointer(),
                                                      signatureBlob->GetBufferSize());

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
    _impl->rootSignatureHash = 0;
    _impl->cbvSrvUavRootParameterIndices.clear();
    _impl->dynamicCbvSrvUavRootParameterIndices.clear();
    _impl->localRootCbvParameterIndices.clear();
    _impl->staticCbvSrvUavRootParameterIndices.clear();
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

int32_t CCD3D12PipelineLayout::getDynamicCbvSrvUavRootParameterIndex(uint32_t set) const {
    return (!_impl || set >= _impl->dynamicCbvSrvUavRootParameterIndices.size()) ? -1 :
           _impl->dynamicCbvSrvUavRootParameterIndices[set];
}

int32_t CCD3D12PipelineLayout::getLocalRootCbvParameterIndex(uint32_t set) const {
    return (!_impl || set >= _impl->localRootCbvParameterIndices.size()) ? -1 :
           _impl->localRootCbvParameterIndices[set];
}

int32_t CCD3D12PipelineLayout::getStaticCbvSrvUavRootParameterIndex(uint32_t set) const {
    return (!_impl || set >= _impl->staticCbvSrvUavRootParameterIndices.size()) ? -1 :
           _impl->staticCbvSrvUavRootParameterIndices[set];
}

uint64_t CCD3D12PipelineLayout::getRootSignatureHash() const {
    return _impl ? _impl->rootSignatureHash : 0;
}


} // namespace gfx
} // namespace cc
