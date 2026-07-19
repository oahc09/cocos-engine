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

#include "D3D12DescriptorSet.h"
#include "D3D12Buffer.h"
#include "D3D12DescriptorHeapPool.h"
#include "D3D12DescriptorSetLayout.h"
#include "D3D12Device.h"
#include "D3D12Texture.h"
#include "base/Log.h"
#include "gfx-base/GFXDef.h"
#include "gfx-base/GFXSamplerUtils.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

namespace {

// Map engine Format to DXGI_FORMAT for SRV/UAV descriptors
DXGI_FORMAT toSRVFormat(Format format, DXGI_FORMAT resourceFormat) {
    if (format == Format::DEPTH) {
        return DXGI_FORMAT_R32_FLOAT;
    }
    if (format == Format::DEPTH_STENCIL) {
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    }
    if (resourceFormat != DXGI_FORMAT_UNKNOWN) {
        return resourceFormat;
    }

    switch (format) {
        case Format::R8:          return DXGI_FORMAT_R8_UNORM;
        case Format::R8SN:        return DXGI_FORMAT_R8_SNORM;
        case Format::R8UI:        return DXGI_FORMAT_R8_UINT;
        case Format::R8I:         return DXGI_FORMAT_R8_SINT;
        case Format::R16F:        return DXGI_FORMAT_R16_FLOAT;
        case Format::R16UI:       return DXGI_FORMAT_R16_UINT;
        case Format::R16I:        return DXGI_FORMAT_R16_SINT;
        case Format::R32F:        return DXGI_FORMAT_R32_FLOAT;
        case Format::R32UI:       return DXGI_FORMAT_R32_UINT;
        case Format::R32I:        return DXGI_FORMAT_R32_SINT;
        case Format::RG8:         return DXGI_FORMAT_R8G8_UNORM;
        case Format::RG8SN:       return DXGI_FORMAT_R8G8_SNORM;
        case Format::RG16F:       return DXGI_FORMAT_R16G16_FLOAT;
        case Format::RG32F:       return DXGI_FORMAT_R32G32_FLOAT;
        case Format::RGB32F:      return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::RGBA8:       return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::BGRA8:       return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::RGBA8SN:     return DXGI_FORMAT_R8G8B8A8_SNORM;
        case Format::RGBA8UI:     return DXGI_FORMAT_R8G8B8A8_UINT;
        case Format::RGBA8I:      return DXGI_FORMAT_R8G8B8A8_SINT;
        case Format::RGBA16F:     return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::RGBA32F:     return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::RGB10A2:     return DXGI_FORMAT_R10G10B10A2_UNORM;
        case Format::R11G11B10F:  return DXGI_FORMAT_R11G11B10_FLOAT;
        default:                  return DXGI_FORMAT_UNKNOWN;
    }
}

D3D12_SRV_DIMENSION toSRVDimension(TextureType type, uint32_t layerCount, bool isMS) {
    if (isMS) {
        if (layerCount > 1) return D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
        return D3D12_SRV_DIMENSION_TEXTURE2DMS;
    }
    switch (type) {
        case TextureType::TEX1D:
            return (layerCount > 1) ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY : D3D12_SRV_DIMENSION_TEXTURE1D;
        case TextureType::TEX2D:
            return (layerCount > 1) ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
        case TextureType::TEX3D:
            return D3D12_SRV_DIMENSION_TEXTURE3D;
        case TextureType::CUBE:
            return (layerCount > 6) ? D3D12_SRV_DIMENSION_TEXTURECUBEARRAY : D3D12_SRV_DIMENSION_TEXTURECUBE;
        default:
            return D3D12_SRV_DIMENSION_TEXTURE2D;
    }
}

D3D12_UAV_DIMENSION toUAVDimension(TextureType type, uint32_t layerCount) {
    switch (type) {
        case TextureType::TEX1D:
            return (layerCount > 1) ? D3D12_UAV_DIMENSION_TEXTURE1DARRAY : D3D12_UAV_DIMENSION_TEXTURE1D;
        case TextureType::TEX2D:
            return (layerCount > 1) ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
        case TextureType::TEX3D:
            return D3D12_UAV_DIMENSION_TEXTURE3D;
        default:
            return D3D12_UAV_DIMENSION_TEXTURE2D;
    }
}

D3D12_SHADER_RESOURCE_VIEW_DESC makeTextureSRVDesc(const Texture *gfxTexture, const CCD3D12Texture *d3d12Texture) {
    const auto &texInfo = gfxTexture->getInfo();
    const auto &viewInfo = gfxTexture->getViewInfo();
    const bool isView = gfxTexture->isTextureView();

    const Format format = isView ? viewInfo.format : texInfo.format;
    const TextureType type = isView ? viewInfo.type : texInfo.type;
    const uint32_t baseLevel = isView ? viewInfo.baseLevel : 0;
    const uint32_t baseLayer = isView ? viewInfo.baseLayer : 0;
    uint32_t layerCount = isView ? viewInfo.layerCount : texInfo.layerCount;
    if (layerCount == 0) {
        layerCount = 1;
    }
    uint32_t mipLevels = isView ? viewInfo.levelCount : texInfo.levelCount;
    if (mipLevels == 0) {
        mipLevels = 1;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    auto *resource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
    const DXGI_FORMAT resourceFormat = resource ? resource->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
    desc.Format = toSRVFormat(format, resourceFormat);
    desc.ViewDimension = toSRVDimension(type, layerCount, texInfo.samples != SampleCount::X1);
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    switch (desc.ViewDimension) {
        case D3D12_SRV_DIMENSION_TEXTURE2D:
            desc.Texture2D.MostDetailedMip = baseLevel;
            desc.Texture2D.MipLevels = mipLevels;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            desc.Texture2DArray.MostDetailedMip = baseLevel;
            desc.Texture2DArray.MipLevels = mipLevels;
            desc.Texture2DArray.FirstArraySlice = baseLayer;
            desc.Texture2DArray.ArraySize = layerCount;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
            desc.Texture2DMSArray.FirstArraySlice = baseLayer;
            desc.Texture2DMSArray.ArraySize = layerCount;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:
            desc.TextureCube.MostDetailedMip = baseLevel;
            desc.TextureCube.MipLevels = mipLevels;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
            desc.TextureCubeArray.MostDetailedMip = baseLevel;
            desc.TextureCubeArray.MipLevels = mipLevels;
            desc.TextureCubeArray.First2DArrayFace = baseLayer;
            desc.TextureCubeArray.NumCubes = layerCount / 6;
            if (desc.TextureCubeArray.NumCubes == 0) {
                desc.TextureCubeArray.NumCubes = 1;
            }
            break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:
            desc.Texture3D.MostDetailedMip = baseLevel;
            desc.Texture3D.MipLevels = mipLevels;
            break;
        default:
            break;
    }

    return desc;
}

D3D12_SHADER_RESOURCE_VIEW_DESC makeRawBufferSRVDesc(uint64_t firstElement, uint64_t sizeInBytes) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = firstElement;
    desc.Buffer.NumElements = sizeInBytes / 4U;
    desc.Buffer.StructureByteStride = 0;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    return desc;
}

D3D12_TEXTURE_ADDRESS_MODE toAddressMode(Address addr) {
    switch (addr) {
        case Address::WRAP: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case Address::MIRROR: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case Address::CLAMP: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case Address::BORDER: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

D3D12_COMPARISON_FUNC toComparisonFunc(ComparisonFunc func) {
    switch (func) {
        case ComparisonFunc::NEVER: return D3D12_COMPARISON_FUNC_NEVER;
        case ComparisonFunc::LESS: return D3D12_COMPARISON_FUNC_LESS;
        case ComparisonFunc::EQUAL: return D3D12_COMPARISON_FUNC_EQUAL;
        case ComparisonFunc::LESS_EQUAL: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case ComparisonFunc::GREATER: return D3D12_COMPARISON_FUNC_GREATER;
        case ComparisonFunc::NOT_EQUAL: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case ComparisonFunc::GREATER_EQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case ComparisonFunc::ALWAYS: return D3D12_COMPARISON_FUNC_ALWAYS;
        default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

D3D12_FILTER toSamplerFilter(const SamplerInfo &info) {
    const bool comparison = info.cmpFunc != ComparisonFunc::ALWAYS;
    if (info.minFilter == Filter::ANISOTROPIC ||
        info.magFilter == Filter::ANISOTROPIC ||
        info.mipFilter == Filter::ANISOTROPIC) {
        return comparison ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
    }

    const auto toFilterType = [](Filter filter) {
        return filter == Filter::LINEAR ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    };
    const auto reductionType = comparison ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON
                                          : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    return D3D12_ENCODE_BASIC_FILTER(toFilterType(info.minFilter),
                                     toFilterType(info.magFilter),
                                     toFilterType(info.mipFilter),
                                     reductionType);
}

D3D12_SAMPLER_DESC makeSamplerDesc(const SamplerInfo &info) {
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = toSamplerFilter(info);
    desc.AddressU = toAddressMode(info.addressU);
    desc.AddressV = toAddressMode(info.addressV);
    desc.AddressW = toAddressMode(info.addressW);
    desc.MaxAnisotropy = info.maxAnisotropy > 0 ? static_cast<UINT>(info.maxAnisotropy) : 1U;
    desc.ComparisonFunc = info.cmpFunc == ComparisonFunc::ALWAYS
                              ? D3D12_COMPARISON_FUNC_NEVER
                              : toComparisonFunc(info.cmpFunc);
    desc.MinLOD = 0.0f;
    desc.MaxLOD = info.mipFilter == Filter::NONE ? 0.0f : D3D12_FLOAT32_MAX;
    desc.MipLODBias = 0.0f;
    return desc;
}

SamplerInfo makeDefaultSamplerInfo() {
    SamplerInfo info{};
    info.minFilter = Filter::POINT;
    info.magFilter = Filter::POINT;
    info.mipFilter = Filter::POINT;
    info.addressU = Address::CLAMP;
    info.addressV = Address::CLAMP;
    info.addressW = Address::CLAMP;
    info.maxAnisotropy = 1;
    info.cmpFunc = ComparisonFunc::ALWAYS;
    return info;
}

} // namespace

// CPU-side descriptor storage for a single binding slot.
// These hold the raw data needed to later write into a GPU-visible descriptor heap.
struct DescriptorData {
    // Buffer descriptor data
    D3D12_GPU_VIRTUAL_ADDRESS gpuVA{0};
    uint32_t bufferSize{0};
    bool isBuffer{false};

    // Texture descriptor data
    void *resourceHandle{nullptr};  // ID3D12Resource*
    bool isTexture{false};

    // Sampler descriptor data (hash for now; actual D3D12_SAMPLER_DESC built at write time)
    ccstd::hash_t samplerHash{0};
    bool isSampler{false};

    // Dirty flag per descriptor
    bool dirty{false};
};

struct CCD3D12DescriptorSet::Impl {
    struct UniformBufferDescriptorSlot {
        uint32_t descriptorIndex{0};
        uint32_t cbvSrvUavOffset{0};
        CCD3D12Buffer *buffer{nullptr};
        uint64_t version{0};
    };

    struct DynamicDescriptorSlot {
        DescriptorType type{DescriptorType::UNKNOWN};
        uint32_t binding{0};
        uint32_t descriptorIndex{0};
        uint32_t cbvSrvUavOffset{0};
    };

    ccstd::vector<DescriptorData> descriptors;
    ccstd::vector<UniformBufferDescriptorSlot> uniformBufferDescriptorSlots;
    ccstd::vector<DynamicDescriptorSlot> dynamicDescriptorSlots;

    // Persistent suballocations from device-level CPU-visible staging pools.
    D3D12DescriptorHeapPool::Allocation cbvSrvUavAllocation;
    D3D12DescriptorHeapPool::Allocation samplerAllocation;
    uint32_t cbvSrvUavDescriptorCount{0};
    uint32_t samplerDescriptorCount{0};

    // Descriptor handles for the start of this set's allocation
    D3D12_CPU_DESCRIPTOR_HANDLE cbvSrvUavCpuStart{};
    D3D12_CPU_DESCRIPTOR_HANDLE samplerCpuStart{};
    uint32_t cbvSrvUavDescriptorSize{0};
    uint32_t samplerDescriptorSize{0};

    bool needsCbvSrvUav{false};
    bool needsSampler{false};
    uint64_t version{0};
    uint64_t staticDescriptorVersion{0};
    uint32_t appliedDynamicOffsetCount{0};
    ccstd::vector<uint32_t> zeroDynamicOffsets;
    ccstd::vector<uint32_t> samplerTableKey;
    uint64_t observedTransientUniformUploadGeneration{0};

    struct StaticDescriptorMetadata {
        uint32_t dynamicCbvDescriptors{0};
        uint32_t staticTextureDescriptors{0};
        uint32_t staticCbvSrvUavDescriptors{0};
        uint32_t rootCbvDescriptorOffset{0};
        uint32_t staticCbvSrvUavTableCount{0};
        uint64_t staticSignature{0};
        bool cbvSrvUavPartitionValid{false};
        bool staticResourceIdentityValid{false};
        ccstd::vector<uintptr_t> staticResourceIdentity;
    } staticDescriptorMetadata;

    bool ensureStagingAllocations(CCD3D12Device *device) {
        if (!device) {
            return false;
        }

        auto ensureAllocation = [&](D3D12DescriptorHeapPool *pool,
                                    uint32_t descriptorCount,
                                    bool required,
                                    D3D12DescriptorHeapPool::Allocation &allocation,
                                    D3D12_CPU_DESCRIPTOR_HANDLE &cpuStart,
                                    uint32_t &descriptorSize,
                                    const char *name) {
            if (!required || descriptorCount == 0) {
                cpuStart = {};
                descriptorSize = 0;
                return true;
            }

            if (!pool) {
                CC_LOG_ERROR("D3D12DescriptorSet: %s staging pool unavailable.", name);
                return false;
            }

            descriptorSize = pool->getDescriptorSize();
            if (!allocation.isValid) {
                allocation = pool->allocate(descriptorCount);
            }
            auto *heap = static_cast<ID3D12DescriptorHeap *>(pool->getHeap(allocation.heapIndex));
            cpuStart.ptr = reinterpret_cast<SIZE_T>(allocation.cpuHandle);
            if (!allocation.isValid || !heap || cpuStart.ptr == 0 || descriptorSize == 0) {
                CC_LOG_ERROR("D3D12DescriptorSet: failed to allocate %s staging descriptors "
                             "(descriptors=%u, poolHeaps=%u, increment=%u).",
                             name, descriptorCount, pool->getHeapCount(), descriptorSize);
                if (allocation.isValid) {
                    pool->deallocate(allocation);
                }
                allocation = {};
                cpuStart = {};
                return false;
            }
            return true;
        };

        const bool cbvReady = ensureAllocation(
            device->getCPUDescriptorHeapPool(),
            cbvSrvUavDescriptorCount, needsCbvSrvUav,
            cbvSrvUavAllocation, cbvSrvUavCpuStart, cbvSrvUavDescriptorSize,
            "CBV_SRV_UAV");
        const bool samplerReady = ensureAllocation(
            device->getCPUSamplerDescriptorHeapPool(),
            samplerDescriptorCount, needsSampler,
            samplerAllocation, samplerCpuStart, samplerDescriptorSize,
            "Sampler");
        return cbvReady && samplerReady;
    }
};

CCD3D12DescriptorSet::CCD3D12DescriptorSet()
: _impl(std::make_unique<Impl>()) {
}

CCD3D12DescriptorSet::~CCD3D12DescriptorSet() {
    destroy();
}

void CCD3D12DescriptorSet::doInit(const DescriptorSetInfo &info) {
    (void)info;

    auto *layout = static_cast<const CCD3D12DescriptorSetLayout *>(_layout);
    if (!layout) {
        CC_LOG_ERROR("D3D12DescriptorSet: null layout.");
        return;
    }

    const uint32_t totalDescriptors = _layout->getDescriptorCount();
    _impl->descriptors.resize(totalDescriptors);
    _impl->cbvSrvUavDescriptorCount = layout->getTextureCount() + layout->getBufferCount() +
                                       layout->getStorageImageCount() + layout->getInputAttachmentCount();
    _impl->samplerDescriptorCount = layout->getSamplerCount();
    _impl->needsCbvSrvUav = (_impl->cbvSrvUavDescriptorCount > 0);
    _impl->needsSampler = (_impl->samplerDescriptorCount > 0);

    // Descriptor layout metadata is immutable. Cache the exact CPU staging
    // slots used by dynamic buffers so per-draw offset updates do not rescan
    // every texture, sampler, and static buffer binding twice.
    const auto &bindings = layout->getBindings();
    const auto &descriptorIndices = layout->getDescriptorIndices();
    uint32_t cbvSrvUavOffset = 0;
    for (const auto &binding : bindings) {
        const uint32_t baseDescIdx = descriptorIndices[binding.binding];
        for (uint32_t i = 0; i < binding.count; ++i) {
            if (binding.descriptorType == DescriptorType::SAMPLER ||
                binding.descriptorType == DescriptorType::UNKNOWN) {
                continue;
            }
            if (binding.descriptorType == DescriptorType::DYNAMIC_UNIFORM_BUFFER ||
                binding.descriptorType == DescriptorType::DYNAMIC_STORAGE_BUFFER) {
                _impl->dynamicDescriptorSlots.push_back({
                    binding.descriptorType,
                    binding.binding,
                    baseDescIdx + i,
                    cbvSrvUavOffset,
                });
            } else if (binding.descriptorType == DescriptorType::UNIFORM_BUFFER) {
                _impl->uniformBufferDescriptorSlots.push_back({baseDescIdx + i, cbvSrvUavOffset});
            }
            ++cbvSrvUavOffset;
        }
    }

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12DescriptorSet: device unavailable.");
        return;
    }

    // Mark all descriptors as dirty initially
    for (auto &desc : _impl->descriptors) {
        desc.dirty = true;
    }

    if (!_impl->ensureStagingAllocations(device)) {
        _isDirty = true;
        return;
    }

    refreshStaticDescriptorMetadata();

    CC_LOG_DEBUG("D3D12 DescriptorSet initialized: %zu descriptors (CBV/SRV/UAV=%u, Sampler=%u)",
                 _impl->descriptors.size(), _impl->cbvSrvUavDescriptorCount, _impl->samplerDescriptorCount);
}

void CCD3D12DescriptorSet::doDestroy() {
    if (_impl) {
        _impl->descriptors.clear();
        _impl->uniformBufferDescriptorSlots.clear();
        _impl->dynamicDescriptorSlots.clear();
        auto *device = CCD3D12Device::getInstance();
        auto *cbvPool = device ? device->getCPUDescriptorHeapPool() : nullptr;
        auto *samplerPool = device ? device->getCPUSamplerDescriptorHeapPool() : nullptr;
        if (cbvPool && _impl->cbvSrvUavAllocation.isValid) {
            cbvPool->deallocate(_impl->cbvSrvUavAllocation);
        }
        if (samplerPool && _impl->samplerAllocation.isValid) {
            samplerPool->deallocate(_impl->samplerAllocation);
        }
        _impl->cbvSrvUavAllocation = {};
        _impl->samplerAllocation = {};
        _impl->cbvSrvUavDescriptorCount = 0;
        _impl->samplerDescriptorCount = 0;
        _impl->cbvSrvUavCpuStart = {};
        _impl->samplerCpuStart = {};
        _impl->cbvSrvUavDescriptorSize = 0;
        _impl->samplerDescriptorSize = 0;
        _impl->needsCbvSrvUav = false;
        _impl->needsSampler = false;
    }
}

void CCD3D12DescriptorSet::update() {
    if (!_impl) return;

    auto *device = CCD3D12Device::getInstance();
    const uint64_t transientGeneration = device
                                             ? device->getTransientUniformUploadGeneration()
                                             : 0;
    if (!_isDirty &&
        _impl->observedTransientUniformUploadGeneration == transientGeneration) {
        return;
    }

    if (_isDirty) {
        forceUpdate();
        return;
    }

    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice || !_impl->ensureStagingAllocations(device)) {
        _isDirty = true;
        return;
    }
    _impl->cbvSrvUavCpuStart.ptr = _impl->needsCbvSrvUav
                                           ? reinterpret_cast<SIZE_T>(_impl->cbvSrvUavAllocation.cpuHandle)
                                           : 0;

    // Ordinary uniform buffers may point at a new fence-safe upload slice in
    // each command-buffer epoch. Rewrite only the changed CBV staging slots;
    // textures, samplers, storage descriptors, and unchanged CBVs remain intact.
    bool descriptorChanged = false;
    bool staticDescriptorChanged = false;
    for (auto &slot : _impl->uniformBufferDescriptorSlots) {
        auto *buffer = static_cast<CCD3D12Buffer *>(_buffers[slot.descriptorIndex].ptr);
        if (buffer != slot.buffer) {
            _isDirty = true;
            break;
        }
        // Pending ordinary-uniform updates are batched immediately before the
        // draw/dispatch descriptor flush. Do not allocate an individual upload
        // slice here: that would upload the same contents once now and again in
        // CCD3D12Device::flushPendingBufferUpdates().
        const uint64_t version = buffer ? buffer->getUniformDescriptorVersion() : 0;
        if (version == slot.version) {
            continue;
        }

        const uint64_t gpuAddress = buffer ? buffer->getD3D12UniformGPUVirtualAddress() : 0;
        const uint32_t cbvSize = buffer ? buffer->getD3D12ConstantBufferSize() : 0;
        if (!buffer || gpuAddress == 0 || cbvSize < 256U ||
            slot.cbvSrvUavOffset >= _impl->cbvSrvUavDescriptorCount) {
            _isDirty = true;
            break;
        }

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
        cbvDesc.BufferLocation = gpuAddress;
        cbvDesc.SizeInBytes = cbvSize;
        D3D12_CPU_DESCRIPTOR_HANDLE handle{};
        handle.ptr = _impl->cbvSrvUavCpuStart.ptr +
                     slot.cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
        d3dDevice->CreateConstantBufferView(&cbvDesc, handle);
        slot.version = version;
        descriptorChanged = true;
        staticDescriptorChanged = staticDescriptorChanged ||
                                  (_impl->staticDescriptorMetadata.cbvSrvUavPartitionValid &&
                                   slot.cbvSrvUavOffset != _impl->staticDescriptorMetadata.rootCbvDescriptorOffset);
    }

    if (_isDirty) {
        forceUpdate();
        return;
    }
    _impl->observedTransientUniformUploadGeneration =
        device->getTransientUniformUploadGeneration();
    if (descriptorChanged) {
        if (staticDescriptorChanged) {
            refreshStaticDescriptorMetadata();
            ++_impl->staticDescriptorVersion;
        }
        ++_impl->version;
    }
}

void CCD3D12DescriptorSet::updateForLocalRootCbv(bool skipStaticCbvStaging) {
    if (!_impl) {
        return;
    }

    if (_isDirty) {
        forceUpdate();
        return;
    }

    if (skipStaticCbvStaging) {
        // The local root-table flush writes ordinary suffix CBVs directly to
        // its current shader-visible allocation.  Keep the CPU staging copy
        // untouched: it remains the conservative source for non-local and
        // fallback paths, while avoiding a duplicate CreateCBV per draw.
        for (const auto &slot : _impl->uniformBufferDescriptorSlots) {
            if (slot.cbvSrvUavOffset == _impl->staticDescriptorMetadata.rootCbvDescriptorOffset) {
                continue;
            }
            auto *buffer = static_cast<CCD3D12Buffer *>(_buffers[slot.descriptorIndex].ptr);
            const uint64_t version = buffer ? buffer->getUniformDescriptorVersion() : 0;
            if (buffer != slot.buffer || version != slot.version) {
                _isDirty = true;
                break;
            }
        }
        if (_isDirty) {
            forceUpdate();
        }
        return;
    }

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice || !_impl->ensureStagingAllocations(device)) {
        _isDirty = true;
        return;
    }
    _impl->cbvSrvUavCpuStart.ptr = _impl->needsCbvSrvUav
                                           ? reinterpret_cast<SIZE_T>(_impl->cbvSrvUavAllocation.cpuHandle)
                                           : 0;

    // The local root-CBV path sources b0 directly from the buffer GPU VA, so
    // its CPU staging descriptor is not referenced by the command list. Keep
    // all following ordinary CBVs exact: they remain part of the static table.
    bool staticDescriptorChanged = false;
    for (auto &slot : _impl->uniformBufferDescriptorSlots) {
        if (slot.cbvSrvUavOffset == _impl->staticDescriptorMetadata.rootCbvDescriptorOffset) {
            continue;
        }
        auto *buffer = static_cast<CCD3D12Buffer *>(_buffers[slot.descriptorIndex].ptr);
        if (buffer != slot.buffer) {
            _isDirty = true;
            break;
        }
        const uint64_t version = buffer ? buffer->getUniformDescriptorVersion() : 0;
        if (version == slot.version) {
            continue;
        }

        const uint64_t gpuAddress = buffer ? buffer->getD3D12UniformGPUVirtualAddress() : 0;
        const uint32_t cbvSize = buffer ? buffer->getD3D12ConstantBufferSize() : 0;
        if (!buffer || gpuAddress == 0 || cbvSize < 256U ||
            slot.cbvSrvUavOffset >= _impl->cbvSrvUavDescriptorCount) {
            _isDirty = true;
            break;
        }

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
        cbvDesc.BufferLocation = gpuAddress;
        cbvDesc.SizeInBytes = cbvSize;
        D3D12_CPU_DESCRIPTOR_HANDLE handle{};
        handle.ptr = _impl->cbvSrvUavCpuStart.ptr +
                     slot.cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
        d3dDevice->CreateConstantBufferView(&cbvDesc, handle);
        slot.version = version;
        staticDescriptorChanged = true;
    }

    if (_isDirty) {
        forceUpdate();
        return;
    }
    if (staticDescriptorChanged) {
        refreshStaticDescriptorMetadata();
        ++_impl->staticDescriptorVersion;
        ++_impl->version;
    }
}

void CCD3D12DescriptorSet::forceUpdate() {
    if (!_impl || _impl->descriptors.empty()) return;

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) return;

    if (!_impl->ensureStagingAllocations(device)) {
        _isDirty = true;
        return;
    }
    _impl->cbvSrvUavCpuStart.ptr = _impl->needsCbvSrvUav
                                           ? reinterpret_cast<SIZE_T>(_impl->cbvSrvUavAllocation.cpuHandle)
                                           : 0;
    const D3D12_CPU_DESCRIPTOR_HANDLE samplerCpuStart =
        {_impl->needsSampler ? reinterpret_cast<SIZE_T>(_impl->samplerAllocation.cpuHandle) : 0};

    // Helper: write a dummy texture SRV to a heap slot (for null texture bindings)
    auto writeDummyTextureSRV = [&](D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        auto *dummyTex = device->getDummyTexture();
        if (dummyTex) {
            auto *rawRes = static_cast<ID3D12Resource *>(dummyTex->getD3D12ResourceHandle());
            if (rawRes) {
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.Texture2D.MipLevels = 1;
                srvDesc.Texture2D.MostDetailedMip = 0;
                d3dDevice->CreateShaderResourceView(rawRes, &srvDesc, handle);
                return;
            }
        }
        // Fallback: null CBV descriptor (same slot size)
        d3dDevice->CreateConstantBufferView(nullptr, handle);
    };

    // Helper: write a dummy buffer SRV to a heap slot (for null buffer SRV bindings)
    auto writeDummyBufferSRV = [&](D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        auto *dummyBuf = device->getDummyBuffer();
        if (dummyBuf) {
            auto *rawRes = static_cast<ID3D12Resource *>(dummyBuf->getD3D12ResourceHandle());
            if (rawRes) {
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = makeRawBufferSRVDesc(0, 4U);
                d3dDevice->CreateShaderResourceView(rawRes, &srvDesc, handle);
                return;
            }
        }
        d3dDevice->CreateConstantBufferView(nullptr, handle);
    };

    // Helper: write a valid dummy CBV to a heap slot (for null/small buffer CBV bindings).
    auto writeDummyCBV = [&](D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        auto *dummyBuf = device->getDummyBuffer();
        if (dummyBuf) {
            auto *rawRes = static_cast<ID3D12Resource *>(dummyBuf->getD3D12ResourceHandle());
            if (rawRes) {
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
                cbvDesc.BufferLocation = dummyBuf->getD3D12GPUVirtualAddress();
                cbvDesc.SizeInBytes = 256U;
                d3dDevice->CreateConstantBufferView(&cbvDesc, handle);
                return;
            }
        }
        d3dDevice->CreateConstantBufferView(nullptr, handle);
    };

    // Helper: write a dummy texture UAV to a heap slot (for null UAV bindings)
    auto writeDummyTextureUAV = [&](D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        auto *dummyTex = device->getDummyTexture();
        if (dummyTex) {
            auto *rawRes = static_cast<ID3D12Resource *>(dummyTex->getD3D12ResourceHandle());
            if (rawRes) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                d3dDevice->CreateUnorderedAccessView(rawRes, nullptr, &uavDesc, handle);
                return;
            }
        }
        d3dDevice->CreateConstantBufferView(nullptr, handle);
    };

    // Walk through the base class _buffers/_textures/_samplers arrays
    // and write D3D12 descriptors to the CPU staging heap
    const auto &bindings = _layout->getBindings();
    const auto &descriptorIndices = _layout->getDescriptorIndices();

    uint32_t cbvSrvUavOffset = 0;
    uint32_t samplerOffset = 0;
    _impl->samplerTableKey.assign(_impl->samplerDescriptorCount, 0U);

    for (const auto &binding : bindings) {
        const uint32_t baseDescIdx = descriptorIndices[binding.binding];

        for (uint32_t i = 0; i < binding.count; ++i) {
            const uint32_t descIdx = baseDescIdx + i;

            switch (binding.descriptorType) {
                case DescriptorType::UNIFORM_BUFFER:
                case DescriptorType::DYNAMIC_UNIFORM_BUFFER: {
                    auto *gfxBuffer = _buffers[descIdx].ptr;
                    if (gfxBuffer && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(gfxBuffer);
                        const bool dynamicUniform = binding.descriptorType == DescriptorType::DYNAMIC_UNIFORM_BUFFER;
                        d3d12Buffer->markUniformDescriptorBinding(dynamicUniform);
                        const bool transientUniform = !dynamicUniform && d3d12Buffer->ensureTransientUniformUpload();
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
                        if (rawResource) {
                            UINT64 cbvSize = d3d12Buffer->getD3D12ConstantBufferSize();
                            UINT64 bufferLocation = d3d12Buffer->getD3D12UniformGPUVirtualAddress();
                            if (!transientUniform) {
                                const UINT64 resourceWidth = rawResource->GetDesc().Width;
                                const UINT64 resourceOffset = static_cast<UINT64>(d3d12Buffer->getD3D12ResourceOffset());
                                const UINT64 availableSize = (resourceWidth > resourceOffset) ? (resourceWidth - resourceOffset) : 0ULL;
                                const UINT64 logicalAligned = static_cast<UINT64>(gfxBuffer->getSize()) & ~255ULL;
                                // Preserve the existing DEFAULT-resource workaround: some paths expose
                                // a logical size smaller than the shader block but have a larger backing
                                // allocation. A transient slice, however, is exactly the logical size.
                                const UINT64 availableAligned = availableSize & ~255ULL;
                                cbvSize = std::min<UINT64>(availableAligned, 64ULL * 1024ULL);
                                if (logicalAligned >= 256ULL && availableAligned < logicalAligned) {
                                    CC_LOG_WARNING("[D3D12-CBV] available range smaller than logical buffer size: binding=%u descIdx=%u logical=%llu available=%llu rawWidth=%llu rawOffset=%llu",
                                                   binding.binding,
                                                   descIdx,
                                                   static_cast<unsigned long long>(logicalAligned),
                                                   static_cast<unsigned long long>(availableAligned),
                                                   static_cast<unsigned long long>(resourceWidth),
                                                   static_cast<unsigned long long>(resourceOffset));
                                }
                                bufferLocation = d3d12Buffer->getD3D12GPUVirtualAddress();
                            }
                            if (cbvSize >= 256U) {
                                D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
                                cbvDesc.BufferLocation = bufferLocation;
                                cbvDesc.SizeInBytes = static_cast<UINT>(cbvSize);

                                D3D12_CPU_DESCRIPTOR_HANDLE handle;
                                handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                                d3dDevice->CreateConstantBufferView(&cbvDesc, handle);
                            } else {
                                // Buffer too small for CBV (shouldn't happen with 256-byte aligned creation).
                                // Write a null descriptor to keep heap layout consistent.
                                D3D12_CPU_DESCRIPTOR_HANDLE handle;
                                handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                                writeDummyCBV(handle);
                            }
                        } else {
                            // Buffer exists but has no D3D12 resource -- write null CBV descriptor
                            // to keep heap layout consistent and avoid "No Resource" in RenderDoc.
                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            writeDummyCBV(handle);
                        }
                    } else if (cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        // Null buffer binding: write a null CBV descriptor to keep heap slot valid.
                        // Without this, the slot contains zeroed memory which D3D12 runtime interprets
                        // as an invalid descriptor, causing "No Resource" in RenderDoc and GPU hangs.
                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                        writeDummyCBV(handle);
                    }
                    ++cbvSrvUavOffset;
                    break;
                }
                case DescriptorType::STORAGE_BUFFER:
                case DescriptorType::DYNAMIC_STORAGE_BUFFER: {
                    auto *gfxBuffer = _buffers[descIdx].ptr;
                    if (gfxBuffer && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(gfxBuffer);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
                        if (rawResource) {
                            const uint64_t firstElement = d3d12Buffer->getD3D12ResourceOffset() / 4U;
                            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = makeRawBufferSRVDesc(firstElement, gfxBuffer->getSize());

                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            d3dDevice->CreateShaderResourceView(rawResource, &srvDesc, handle);
                        } else {
                            // Buffer exists but has no D3D12 resource -- write null SRV descriptor.
                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            writeDummyBufferSRV(handle);
                        }
                    } else if (cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        // Null buffer binding: write null SRV descriptor to keep heap slot valid.
                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                        writeDummyBufferSRV(handle);
                    }
                    ++cbvSrvUavOffset;
                    break;
                }
                case DescriptorType::SAMPLER_TEXTURE: {
                    // Texture (SRV) part
                    auto *gfxTexture = _textures[descIdx].ptr;
                    auto *gfxSampler = _samplers[descIdx].ptr;
                    if (gfxTexture && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Texture = static_cast<CCD3D12Texture *>(gfxTexture);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
                        if (rawResource) {
                            const auto srvDesc = makeTextureSRVDesc(gfxTexture, d3d12Texture);
                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            d3dDevice->CreateShaderResourceView(rawResource, &srvDesc, handle);
                        } else {
                            // Texture exists but has no D3D12 resource -- write null SRV descriptor.
                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            writeDummyTextureSRV(handle);
                        }
                    } else if (cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        // Null texture binding: write null SRV descriptor to keep heap slot valid.
                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                        writeDummyTextureSRV(handle);
                    }
                    ++cbvSrvUavOffset;

                    // Sampler part
                    if (gfxSampler && samplerOffset < _impl->samplerDescriptorCount) {
                        const auto &samplerInfo = gfxSampler->getInfo();
                        D3D12_SAMPLER_DESC samplerDesc = makeSamplerDesc(samplerInfo);
                        _impl->samplerTableKey[samplerOffset] = packSamplerInfo(samplerInfo);

                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = samplerCpuStart.ptr + samplerOffset * _impl->samplerDescriptorSize;
                        d3dDevice->CreateSampler(&samplerDesc, handle);
                    } else if (samplerOffset < _impl->samplerDescriptorCount) {
                        // Null sampler binding: write default sampler to keep heap slot valid.
                        const SamplerInfo defaultSamplerInfo = makeDefaultSamplerInfo();
                        D3D12_SAMPLER_DESC defaultSampler = makeSamplerDesc(defaultSamplerInfo);
                        _impl->samplerTableKey[samplerOffset] = packSamplerInfo(defaultSamplerInfo);
                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = samplerCpuStart.ptr + samplerOffset * _impl->samplerDescriptorSize;
                        d3dDevice->CreateSampler(&defaultSampler, handle);
                    }
                    ++samplerOffset;
                    break;
                }
                case DescriptorType::TEXTURE: {
                    auto *gfxTexture = _textures[descIdx].ptr;
                    if (gfxTexture && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Texture = static_cast<CCD3D12Texture *>(gfxTexture);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
                        if (rawResource) {
                            const auto srvDesc = makeTextureSRVDesc(gfxTexture, d3d12Texture);
                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            d3dDevice->CreateShaderResourceView(rawResource, &srvDesc, handle);
                        } else {
                            // Texture exists but has no D3D12 resource -- write null SRV descriptor.
                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            writeDummyTextureSRV(handle);
                        }
                    } else if (cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        // Null texture binding: write null SRV descriptor to keep heap slot valid.
                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                        writeDummyTextureSRV(handle);
                    }
                    ++cbvSrvUavOffset;
                    break;
                }
                case DescriptorType::SAMPLER: {
                    auto *gfxSampler = _samplers[descIdx].ptr;
                    if (gfxSampler && samplerOffset < _impl->samplerDescriptorCount) {
                        const auto &samplerInfo = gfxSampler->getInfo();
                        D3D12_SAMPLER_DESC samplerDesc = makeSamplerDesc(samplerInfo);
                        _impl->samplerTableKey[samplerOffset] = packSamplerInfo(samplerInfo);

                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = samplerCpuStart.ptr + samplerOffset * _impl->samplerDescriptorSize;
                        d3dDevice->CreateSampler(&samplerDesc, handle);
                    } else if (samplerOffset < _impl->samplerDescriptorCount) {
                        // Null sampler binding: write default sampler to keep heap slot valid.
                        const SamplerInfo defaultSamplerInfo = makeDefaultSamplerInfo();
                        D3D12_SAMPLER_DESC defaultSampler = makeSamplerDesc(defaultSamplerInfo);
                        _impl->samplerTableKey[samplerOffset] = packSamplerInfo(defaultSamplerInfo);
                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = samplerCpuStart.ptr + samplerOffset * _impl->samplerDescriptorSize;
                        d3dDevice->CreateSampler(&defaultSampler, handle);
                    }
                    ++samplerOffset;
                    break;
                }
                case DescriptorType::STORAGE_IMAGE: {
                    auto *gfxTexture = _textures[descIdx].ptr;
                    if (cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                        if (gfxTexture) {
                            auto *d3d12Texture = static_cast<CCD3D12Texture *>(gfxTexture);
                            auto *rawResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
                            if (rawResource) {
                                const auto &texInfo = gfxTexture->getInfo();
                                D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                                uavDesc.Format = rawResource->GetDesc().Format;
                                if (uavDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                                    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                                } else if (uavDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                                    uavDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                                }
                                uavDesc.ViewDimension = toUAVDimension(texInfo.type, texInfo.layerCount);
                                d3dDevice->CreateUnorderedAccessView(rawResource, nullptr, &uavDesc, handle);
                            } else {
                                // Null resource: write null UAV descriptor to keep heap slot valid.
                                writeDummyTextureUAV(handle);
                            }
                        } else {
                            // Null texture binding: write null UAV descriptor to keep heap slot valid.
                            writeDummyTextureUAV(handle);
                        }
                    }
                    ++cbvSrvUavOffset;
                    break;
                }
                case DescriptorType::INPUT_ATTACHMENT: {
                    // Input attachment is treated as SRV in D3D12
                    auto *gfxTexture = _textures[descIdx].ptr;
                    if (gfxTexture && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Texture = static_cast<CCD3D12Texture *>(gfxTexture);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
                        if (rawResource) {
                            const auto srvDesc = makeTextureSRVDesc(gfxTexture, d3d12Texture);
                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            d3dDevice->CreateShaderResourceView(rawResource, &srvDesc, handle);
                        } else {
                            // Texture exists but has no D3D12 resource -- write null SRV descriptor.
                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            writeDummyTextureSRV(handle);
                        }
                    } else if (cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        // Null texture binding: write null SRV descriptor to keep heap slot valid.
                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                        writeDummyTextureSRV(handle);
                    }
                    ++cbvSrvUavOffset;
                    break;
                }
                default:
                    break;
            }
        }
    }

    for (auto &slot : _impl->uniformBufferDescriptorSlots) {
        slot.buffer = static_cast<CCD3D12Buffer *>(_buffers[slot.descriptorIndex].ptr);
        slot.version = slot.buffer ? slot.buffer->getUniformDescriptorVersion() : 0;
    }

    refreshStaticDescriptorMetadata();
    _impl->observedTransientUniformUploadGeneration =
        device->getTransientUniformUploadGeneration();
    _isDirty = false;
    ++_impl->staticDescriptorVersion;
    ++_impl->version;
}

void *CCD3D12DescriptorSet::getCbvSrvUavDescriptorHeap() const {
    auto *device = CCD3D12Device::getInstance();
    auto *pool = device ? device->getCPUDescriptorHeapPool() : nullptr;
    return (_impl && pool && _impl->cbvSrvUavAllocation.isValid)
               ? pool->getHeap(_impl->cbvSrvUavAllocation.heapIndex)
               : nullptr;
}

void *CCD3D12DescriptorSet::getSamplerDescriptorHeap() const {
    auto *device = CCD3D12Device::getInstance();
    auto *pool = device ? device->getCPUSamplerDescriptorHeapPool() : nullptr;
    return (_impl && pool && _impl->samplerAllocation.isValid)
               ? pool->getHeap(_impl->samplerAllocation.heapIndex)
               : nullptr;
}

uint64_t CCD3D12DescriptorSet::getCbvSrvUavCPUDescriptorHandle() const {
    return (_impl && _impl->cbvSrvUavAllocation.isValid)
               ? reinterpret_cast<uint64_t>(_impl->cbvSrvUavAllocation.cpuHandle)
               : 0;
}

uint64_t CCD3D12DescriptorSet::getSamplerCPUDescriptorHandle() const {
    return (_impl && _impl->samplerAllocation.isValid)
               ? reinterpret_cast<uint64_t>(_impl->samplerAllocation.cpuHandle)
               : 0;
}

uint32_t CCD3D12DescriptorSet::getCbvSrvUavDescriptorCount() const {
    return _impl ? _impl->cbvSrvUavDescriptorCount : 0;
}

bool CCD3D12DescriptorSet::getCbvSrvUavPartition(uint32_t &rootCbvDescriptorOffset,
                                                  uint32_t &staticTableDescriptorCount) const {
    rootCbvDescriptorOffset = 0;
    staticTableDescriptorCount = 0;
    if (!_impl) {
        return false;
    }
    const auto &metadata = _impl->staticDescriptorMetadata;
    rootCbvDescriptorOffset = metadata.rootCbvDescriptorOffset;
    staticTableDescriptorCount = metadata.staticCbvSrvUavTableCount;
    return metadata.cbvSrvUavPartitionValid;
}

bool CCD3D12DescriptorSet::canReuseStaticCbvSrvUavResources() const {
    if (!_impl) {
        return false;
    }
    const auto &metadata = _impl->staticDescriptorMetadata;
    return metadata.staticResourceIdentityValid && metadata.staticCbvSrvUavTableCount > 0;
}

bool CCD3D12DescriptorSet::hasMatchingStaticCbvSrvUavResources(const CCD3D12DescriptorSet &other) const {
    if (!_impl || !other._impl || _layout != other._layout) {
        return false;
    }
    const auto &metadata = _impl->staticDescriptorMetadata;
    return metadata.staticResourceIdentityValid &&
           other._impl->staticDescriptorMetadata.staticResourceIdentityValid &&
           metadata.staticResourceIdentity == other._impl->staticDescriptorMetadata.staticResourceIdentity;
}

uint32_t CCD3D12DescriptorSet::getSamplerDescriptorCount() const {
    return _impl ? _impl->samplerDescriptorCount : 0;
}

uint64_t CCD3D12DescriptorSet::getVersion() const {
    return _impl ? _impl->version : 0;
}

uint64_t CCD3D12DescriptorSet::getStaticDescriptorVersion() const {
    return _impl ? _impl->staticDescriptorVersion : 0;
}

uint32_t CCD3D12DescriptorSet::getUniformDescriptorSlotCount() const {
    return _impl ? static_cast<uint32_t>(_impl->uniformBufferDescriptorSlots.size()) : 0;
}

uint32_t CCD3D12DescriptorSet::getDynamicDescriptorSlotCount() const {
    return _impl ? static_cast<uint32_t>(_impl->dynamicDescriptorSlots.size()) : 0;
}

bool CCD3D12DescriptorSet::getDynamicDescriptorOffset(uint32_t index, uint32_t &descriptorOffset) const {
    descriptorOffset = 0;
    if (!_impl || index >= _impl->dynamicDescriptorSlots.size()) {
        return false;
    }
    descriptorOffset = _impl->dynamicDescriptorSlots[index].cbvSrvUavOffset;
    return true;
}

bool CCD3D12DescriptorSet::getDynamicDescriptorSource(uint32_t index, uint64_t &gpuAddress,
                                                       uint64_t &size) const {
    gpuAddress = 0;
    size = 0;
    if (!_impl || index >= _impl->dynamicDescriptorSlots.size()) {
        return false;
    }
    const auto &slot = _impl->dynamicDescriptorSlots[index];
    if (slot.descriptorIndex >= _impl->descriptors.size()) {
        return false;
    }
    auto *buffer = static_cast<CCD3D12Buffer *>(_buffers[slot.descriptorIndex].ptr);
    if (!buffer) {
        return false;
    }
    gpuAddress = buffer->getD3D12GPUVirtualAddress();
    size = buffer->getSize();
    // Dynamic storage views need not expose a GPU virtual address. Their
    // buffer-object identity still distinguishes whether a reusable dynamic
    // descriptor table is possible.
    if (gpuAddress == 0) {
        gpuAddress = reinterpret_cast<uintptr_t>(buffer);
    }
    return gpuAddress != 0;
}

bool CCD3D12DescriptorSet::hasOnlyNullDynamicDescriptorSources() const {
    if (!_impl || _impl->dynamicDescriptorSlots.empty()) {
        return false;
    }
    for (const auto &slot : _impl->dynamicDescriptorSlots) {
        if (slot.descriptorIndex >= _buffers.size() || _buffers[slot.descriptorIndex].ptr) {
            return false;
        }
    }
    return true;
}

bool CCD3D12DescriptorSet::getUniformDescriptorSignature(uint32_t index, uint32_t &descriptorOffset,
                                                          uint64_t &gpuAddress, uint32_t &size) const {
    if (!_impl || index >= _impl->uniformBufferDescriptorSlots.size()) {
        return false;
    }
    const auto &slot = _impl->uniformBufferDescriptorSlots[index];
    descriptorOffset = slot.cbvSrvUavOffset;
    gpuAddress = slot.buffer ? slot.buffer->getD3D12UniformGPUVirtualAddress() : 0;
    size = slot.buffer ? slot.buffer->getD3D12ConstantBufferSize() : 0;
    return true;
}

void CCD3D12DescriptorSet::getStaticDescriptorAnalysis(uint32_t &dynamicCbvDescriptors,
                                                        uint32_t &staticTextureDescriptors,
                                                        uint32_t &staticCbvSrvUavDescriptors,
                                                        uint64_t &staticSignature) const {
    dynamicCbvDescriptors = 0;
    staticTextureDescriptors = 0;
    staticCbvSrvUavDescriptors = 0;
    staticSignature = 0;
    if (!_impl) {
        return;
    }
    const auto &metadata = _impl->staticDescriptorMetadata;
    dynamicCbvDescriptors = metadata.dynamicCbvDescriptors;
    staticTextureDescriptors = metadata.staticTextureDescriptors;
    staticCbvSrvUavDescriptors = metadata.staticCbvSrvUavDescriptors;
    staticSignature = metadata.staticSignature;
}

void CCD3D12DescriptorSet::refreshStaticDescriptorMetadata() {
    if (!_impl || !_layout) {
        return;
    }

    auto &metadata = _impl->staticDescriptorMetadata;
    metadata = {};
    const auto mixSignature = [](uint64_t &hash, uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };

    uint64_t signature = 1469598103934665603ULL;
    bool rootCbvSeen = false;
    bool partitionOrderValid = true;
    bool identityValid = true;
    uint32_t cbvSrvUavOffset = 0;
    const auto &bindings = _layout->getBindings();
    const auto &descriptorIndices = _layout->getDescriptorIndices();
    for (const auto &binding : bindings) {
        const DescriptorType type = binding.descriptorType;
        if (type == DescriptorType::SAMPLER || type == DescriptorType::UNKNOWN) {
            continue;
        }
        const bool rootCbv = !rootCbvSeen &&
                                   type == DescriptorType::UNIFORM_BUFFER &&
                                   binding.binding == 0 && binding.count == 1;
        if (rootCbv) {
            metadata.dynamicCbvDescriptors += binding.count;
            metadata.rootCbvDescriptorOffset = cbvSrvUavOffset;
            rootCbvSeen = true;
            cbvSrvUavOffset += binding.count;
            continue;
        }

        if (type == DescriptorType::DYNAMIC_UNIFORM_BUFFER ||
            type == DescriptorType::DYNAMIC_STORAGE_BUFFER) {
            cbvSrvUavOffset += binding.count;
            continue;
        }
        metadata.staticCbvSrvUavDescriptors += binding.count;
        metadata.staticCbvSrvUavTableCount += binding.count;
        const uint32_t baseDescriptorIndex = descriptorIndices[binding.binding];
        for (uint32_t element = 0; element < binding.count; ++element) {
            const uint32_t descriptorIndex = baseDescriptorIndex + element;
            mixSignature(signature, binding.binding);
            mixSignature(signature, element);
            mixSignature(signature, static_cast<uint32_t>(type));
            switch (type) {
                case DescriptorType::SAMPLER_TEXTURE:
                case DescriptorType::TEXTURE:
                case DescriptorType::STORAGE_IMAGE:
                case DescriptorType::INPUT_ATTACHMENT: {
                    const uintptr_t resource = reinterpret_cast<uintptr_t>(_textures[descriptorIndex].ptr);
                    ++metadata.staticTextureDescriptors;
                    metadata.staticResourceIdentity.push_back(resource);
                    mixSignature(signature, resource);
                    break;
                }
                case DescriptorType::STORAGE_BUFFER:
                case DescriptorType::DYNAMIC_STORAGE_BUFFER: {
                    const uintptr_t resource = reinterpret_cast<uintptr_t>(_buffers[descriptorIndex].ptr);
                    metadata.staticResourceIdentity.push_back(resource);
                    mixSignature(signature, resource);
                    break;
                }
                case DescriptorType::UNIFORM_BUFFER: {
                    // Ordinary suffix CBVs are safe to share only while they
                    // name the same stable buffer object. updateForLocalRootCbv
                    // validates its descriptor version before using this key.
                    const uintptr_t resource = reinterpret_cast<uintptr_t>(_buffers[descriptorIndex].ptr);
                    metadata.staticResourceIdentity.push_back(resource);
                    mixSignature(signature, resource);
                    break;
                }
                default:
                    identityValid = false;
                    mixSignature(signature, 0U);
                    break;
            }
        }
        cbvSrvUavOffset += binding.count;
    }

    metadata.staticResourceIdentityValid = identityValid;
    if (metadata.staticCbvSrvUavDescriptors > 0) {
        metadata.staticSignature = signature;
    }
    metadata.cbvSrvUavPartitionValid = partitionOrderValid && rootCbvSeen &&
                                        metadata.dynamicCbvDescriptors == 1 &&
                                        metadata.staticCbvSrvUavTableCount > 0 &&
                                        metadata.rootCbvDescriptorOffset < _impl->cbvSrvUavDescriptorCount &&
                                        metadata.dynamicCbvDescriptors + metadata.staticCbvSrvUavTableCount +
                                                _impl->dynamicDescriptorSlots.size() ==
                                            _impl->cbvSrvUavDescriptorCount;
}

uint32_t CCD3D12DescriptorSet::getDescriptorSemanticCount() const {
    return _impl ? static_cast<uint32_t>(_impl->descriptors.size()) : 0;
}

bool CCD3D12DescriptorSet::getDescriptorSemanticSignature(uint32_t index, uint32_t &kind,
                                                           uint64_t &value0, uint64_t &value1) const {
    if (!_impl || index >= _impl->descriptors.size()) {
        return false;
    }
    const auto &descriptor = _impl->descriptors[index];
    if (descriptor.isBuffer) {
        kind = 1;
        value0 = descriptor.gpuVA;
        value1 = descriptor.bufferSize;
    } else if (descriptor.isTexture) {
        kind = 2;
        value0 = reinterpret_cast<uintptr_t>(descriptor.resourceHandle);
        value1 = 0;
    } else if (descriptor.isSampler) {
        kind = 3;
        value0 = static_cast<uint64_t>(descriptor.samplerHash);
        value1 = 0;
    } else {
        kind = 0;
        value0 = 0;
        value1 = 0;
    }
    return true;
}

const ccstd::vector<uint32_t> &CCD3D12DescriptorSet::getSamplerTableKey() const {
    static const ccstd::vector<uint32_t> EMPTY_KEY;
    return _impl ? _impl->samplerTableKey : EMPTY_KEY;
}

void CCD3D12DescriptorSet::restoreDynamicOffsetDescriptors() {
    if (!_impl || _impl->appliedDynamicOffsetCount == 0) {
        return;
    }

    const uint32_t dynamicOffsetCount = _impl->appliedDynamicOffsetCount;
    auto &zeroOffsets = _impl->zeroDynamicOffsets;
    zeroOffsets.assign(dynamicOffsetCount, 0U);
    applyDynamicOffsets(dynamicOffsetCount, zeroOffsets.data());
    _impl->appliedDynamicOffsetCount = 0;
}

void CCD3D12DescriptorSet::applyDynamicOffsets(uint32_t dynamicOffsetCount, const uint32_t *dynamicOffsets) {
    if (!_impl || !_layout || dynamicOffsetCount == 0 || !dynamicOffsets) {
        return;
    }

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        return;
    }
    if (!_impl->ensureStagingAllocations(device)) {
        _isDirty = true;
        return;
    }
    _impl->appliedDynamicOffsetCount = dynamicOffsetCount;
    _impl->cbvSrvUavCpuStart.ptr = _impl->needsCbvSrvUav
                                           ? reinterpret_cast<SIZE_T>(_impl->cbvSrvUavAllocation.cpuHandle)
                                           : 0;

    auto writeDummyCBV = [&](D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        auto *dummyBuf = device->getDummyBuffer();
        if (dummyBuf) {
            auto *rawRes = static_cast<ID3D12Resource *>(dummyBuf->getD3D12ResourceHandle());
            if (rawRes) {
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
                cbvDesc.BufferLocation = dummyBuf->getD3D12GPUVirtualAddress();
                cbvDesc.SizeInBytes = 256U;
                d3dDevice->CreateConstantBufferView(&cbvDesc, handle);
                return;
            }
        }
        d3dDevice->CreateConstantBufferView(nullptr, handle);
    };

    uint32_t dynamicOffsetIndex = 0;
    for (const auto &slot : _impl->dynamicDescriptorSlots) {
        const uint32_t descIdx = slot.descriptorIndex;
        const uint32_t cbvSrvUavOffset = slot.cbvSrvUavOffset;
        const uint32_t dynamicOffset = dynamicOffsetIndex < dynamicOffsetCount
                                           ? dynamicOffsets[dynamicOffsetIndex]
                                           : 0;

        if (slot.type == DescriptorType::DYNAMIC_UNIFORM_BUFFER) {
            auto *gfxBuffer = _buffers[descIdx].ptr;
            if (gfxBuffer && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(gfxBuffer);
                auto *rawResource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
                if (rawResource) {
                            const uint64_t totalOffset = static_cast<uint64_t>(d3d12Buffer->getD3D12ResourceOffset()) + dynamicOffset;
                            const uint64_t resourceWidth = rawResource->GetDesc().Width;
                            const uint64_t availableSize = (resourceWidth > totalOffset) ? (resourceWidth - totalOffset) : 0ULL;
                            // The dynamic offset selects an element inside the backing allocation.
                            // It must not shrink the logical size of the bound buffer view.
                            const uint64_t requiredAligned = (static_cast<uint64_t>(gfxBuffer->getSize()) + 255ULL) & ~255ULL;
                            const uint64_t availableAligned = availableSize & ~255ULL;
                            const uint64_t cbvSize = std::min<uint64_t>(requiredAligned, 64ULL * 1024ULL);
                            if (availableAligned < cbvSize) {
                                CC_LOG_ERROR("[D3D12-CBV-DYN] dynamic CBV range is too small: binding=%u descIdx=%u dynOffset=%u required=%llu available=%llu rawWidth=%llu totalOffset=%llu",
                                               slot.binding,
                                               descIdx,
                                               dynamicOffset,
                                               static_cast<unsigned long long>(cbvSize),
                                               static_cast<unsigned long long>(availableAligned),
                                               static_cast<unsigned long long>(resourceWidth),
                                               static_cast<unsigned long long>(totalOffset));
                            }

                            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
                            cbvDesc.BufferLocation = d3d12Buffer->getD3D12GPUVirtualAddress() + dynamicOffset;
                            cbvDesc.SizeInBytes = static_cast<UINT>(cbvSize);

                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            if (cbvDesc.SizeInBytes >= 256U && availableAligned >= cbvSize) {
                                d3dDevice->CreateConstantBufferView(&cbvDesc, handle);
                            } else {
                                writeDummyCBV(handle);
                            }
                } else {
                    D3D12_CPU_DESCRIPTOR_HANDLE handle;
                    handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                    writeDummyCBV(handle);
                }
            } else if (cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                D3D12_CPU_DESCRIPTOR_HANDLE handle;
                handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                writeDummyCBV(handle);
            }
        } else if (slot.type == DescriptorType::DYNAMIC_STORAGE_BUFFER) {
            auto *gfxBuffer = _buffers[descIdx].ptr;
            if (gfxBuffer && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(gfxBuffer);
                auto *rawResource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
                if (rawResource) {
                            const uint64_t firstElement = (static_cast<uint64_t>(d3d12Buffer->getD3D12ResourceOffset()) + dynamicOffset) / 4U;
                            const uint32_t availableSize = gfxBuffer->getSize() > dynamicOffset ? (gfxBuffer->getSize() - dynamicOffset) : 0U;

                            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = makeRawBufferSRVDesc(firstElement, availableSize);

                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            d3dDevice->CreateShaderResourceView(rawResource, &srvDesc, handle);
                }
            }
        }
        ++dynamicOffsetIndex;
    }
}

} // namespace gfx
} // namespace cc
