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
#include "D3D12DescriptorSetLayout.h"
#include "D3D12Device.h"
#include "D3D12Texture.h"
#include "base/Log.h"
#include "gfx-base/GFXDef.h"

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <wrl/client.h>
#endif

namespace cc {
namespace gfx {

namespace {
#if defined(_WIN32)

// Map engine Format to DXGI_FORMAT for SRV/UAV descriptors
DXGI_FORMAT toSRVFormat(Format format) {
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
        case Format::DEPTH:       return DXGI_FORMAT_R32_FLOAT;         // depth-only SRV
        case Format::DEPTH_STENCIL: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS; // needs SRV with R24_UNORM_X8
        default:                  return DXGI_FORMAT_R8G8B8A8_UNORM;     // safe fallback
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

D3D12_TEXTURE_ADDRESS_MODE toAddressMode(Address addr) {
    switch (addr) {
        case Address::WRAP: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case Address::MIRROR: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case Address::CLAMP: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case Address::BORDER: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

#endif // _WIN32
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
#if defined(_WIN32)
    ccstd::vector<DescriptorData> descriptors;

    // CPU-visible descriptor heap (staging area)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cbvSrvUavHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> samplerHeap;
    uint32_t cbvSrvUavDescriptorCount{0};
    uint32_t samplerDescriptorCount{0};

    // Descriptor handles for the start of this set's allocation
    D3D12_CPU_DESCRIPTOR_HANDLE cbvSrvUavCpuStart{};
    D3D12_CPU_DESCRIPTOR_HANDLE samplerCpuStart{};
    uint32_t cbvSrvUavDescriptorSize{0};
    uint32_t samplerDescriptorSize{0};

    bool needsCbvSrvUav{false};
    bool needsSampler{false};
#endif
};

CCD3D12DescriptorSet::CCD3D12DescriptorSet()
: _impl(std::make_unique<Impl>()) {
}

CCD3D12DescriptorSet::~CCD3D12DescriptorSet() {
    destroy();
}

void CCD3D12DescriptorSet::doInit(const DescriptorSetInfo &info) {
    (void)info;

#if defined(_WIN32)
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

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12DescriptorSet: device unavailable.");
        return;
    }

    _impl->cbvSrvUavDescriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    _impl->samplerDescriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    // Create CPU-visible staging heaps for this descriptor set
    if (_impl->needsCbvSrvUav && _impl->cbvSrvUavDescriptorCount > 0) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = _impl->cbvSrvUavDescriptorCount;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-visible only for staging
        heapDesc.NodeMask = 0;

        HRESULT hr = d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_impl->cbvSrvUavHeap));
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12DescriptorSet: failed to create CBV_SRV_UAV heap. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return;
        }
        _impl->cbvSrvUavCpuStart = _impl->cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    }

    if (_impl->needsSampler && _impl->samplerDescriptorCount > 0) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        heapDesc.NumDescriptors = _impl->samplerDescriptorCount;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        heapDesc.NodeMask = 0;

        HRESULT hr = d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_impl->samplerHeap));
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12DescriptorSet: failed to create Sampler heap. HRESULT=0x%08x", static_cast<unsigned>(hr));
            return;
        }
        _impl->samplerCpuStart = _impl->samplerHeap->GetCPUDescriptorHandleForHeapStart();
    }

    // Mark all descriptors as dirty initially
    for (auto &desc : _impl->descriptors) {
        desc.dirty = true;
    }

    CC_LOG_INFO("D3D12 DescriptorSet initialized: %zu descriptors (CBV/SRV/UAV=%u, Sampler=%u)",
                _impl->descriptors.size(), _impl->cbvSrvUavDescriptorCount, _impl->samplerDescriptorCount);
#endif
}

void CCD3D12DescriptorSet::doDestroy() {
#if defined(_WIN32)
    if (_impl) {
        _impl->descriptors.clear();
        if (_impl->cbvSrvUavHeap) {
            _impl->cbvSrvUavHeap.Reset();
        }
        if (_impl->samplerHeap) {
            _impl->samplerHeap.Reset();
        }
        _impl->cbvSrvUavDescriptorCount = 0;
        _impl->samplerDescriptorCount = 0;
    }
#endif
}

void CCD3D12DescriptorSet::update() {
    if (!_isDirty) return;
    forceUpdate();
}

void CCD3D12DescriptorSet::forceUpdate() {
#if defined(_WIN32)
    if (!_impl || _impl->descriptors.empty()) return;

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) return;

    // Walk through the base class _buffers/_textures/_samplers arrays
    // and write D3D12 descriptors to the CPU staging heap
    const auto &bindings = _layout->getBindings();
    const auto &descriptorIndices = _layout->getDescriptorIndices();

    uint32_t cbvSrvUavOffset = 0;
    uint32_t samplerOffset = 0;
    static uint32_t s_diagDescriptorSetLogCount = 0;
    const bool diagLog = s_diagDescriptorSetLogCount < 24 && _layout && _layout->getDescriptorCount() > 0;

    for (const auto &binding : bindings) {
        const uint32_t baseDescIdx = descriptorIndices[binding.binding];

        for (uint32_t i = 0; i < binding.count; ++i) {
            const uint32_t descIdx = baseDescIdx + i;

            switch (binding.descriptorType) {
                case DescriptorType::UNIFORM_BUFFER:
                case DescriptorType::DYNAMIC_UNIFORM_BUFFER: {
                    auto *gfxBuffer = _buffers[descIdx].ptr;
                    if (diagLog) {
                        auto *d3d12Buffer = gfxBuffer ? static_cast<CCD3D12Buffer *>(gfxBuffer) : nullptr;
                        CC_LOG_INFO("[D3D12-SET] CBV binding=%u idx=%u hasBuffer=%s size=%u",
                                    binding.binding, descIdx, gfxBuffer ? "Y" : "N", gfxBuffer ? gfxBuffer->getSize() : 0);
                        if (d3d12Buffer) {
                            CC_LOG_INFO("[D3D12-SET]   CBV offset=%u gpuVA=0x%llx",
                                        d3d12Buffer->getD3D12ResourceOffset(),
                                        static_cast<unsigned long long>(d3d12Buffer->getD3D12GPUVirtualAddress()));
                            auto *rawResource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
                            if (rawResource) {
                                void *mappedData = nullptr;
                                D3D12_RANGE readRange{d3d12Buffer->getD3D12ResourceOffset(), d3d12Buffer->getD3D12ResourceOffset() + 16};
                                if (SUCCEEDED(rawResource->Map(0, &readRange, &mappedData)) && mappedData) {
                                    const auto *floats = reinterpret_cast<const float *>(
                                        static_cast<const uint8_t *>(mappedData) + d3d12Buffer->getD3D12ResourceOffset());
                                    CC_LOG_INFO("[D3D12-SET]   CBV data=%.3f %.3f %.3f %.3f",
                                                floats[0], floats[1], floats[2], floats[3]);
                                    rawResource->Unmap(0, nullptr);
                                }
                            }
                        }
                    }
                    if (gfxBuffer && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(gfxBuffer);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
                        if (rawResource) {
                            const UINT64 resourceWidth = rawResource->GetDesc().Width;
                            const UINT64 requestedSize = static_cast<UINT64>((gfxBuffer->getSize() + 255U) & ~255U);
                            const UINT64 cbvSize = resourceWidth < requestedSize ? resourceWidth : requestedSize;
                            // D3D12 requires CBV SizeInBytes >= 256 and 256-byte aligned.
                            // Buffer creation already aligns to 256, so resourceWidth >= 256.
                            if (cbvSize >= 256U) {
                                D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
                                cbvDesc.BufferLocation = d3d12Buffer->getD3D12GPUVirtualAddress();
                                cbvDesc.SizeInBytes = static_cast<UINT>(cbvSize);

                                D3D12_CPU_DESCRIPTOR_HANDLE handle;
                                handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                                d3dDevice->CreateConstantBufferView(&cbvDesc, handle);
                            } else {
                                // Buffer too small for CBV (shouldn't happen with 256-byte aligned creation).
                                // Write a null descriptor to keep heap layout consistent.
                                D3D12_CPU_DESCRIPTOR_HANDLE handle;
                                handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                                d3dDevice->CreateConstantBufferView(nullptr, handle);
                            }
                        }
                    }
                    // Null buffer/resource bindings: skip writing descriptor (offset still increments).
                    // D3D12 null SRV/UAV descriptors require careful desc setup; leaving slot unwritten
                    // is safer than writing potentially invalid null descriptors.
                    ++cbvSrvUavOffset;
                    break;
                }
                case DescriptorType::STORAGE_BUFFER:
                case DescriptorType::DYNAMIC_STORAGE_BUFFER: {
                    auto *gfxBuffer = _buffers[descIdx].ptr;
                    if (diagLog) {
                        CC_LOG_INFO("[D3D12-SET] SRV-BUF binding=%u idx=%u hasBuffer=%s size=%u",
                                    binding.binding, descIdx, gfxBuffer ? "Y" : "N", gfxBuffer ? gfxBuffer->getSize() : 0);
                    }
                    if (gfxBuffer && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(gfxBuffer);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
                        if (rawResource) {
                            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                            srvDesc.Buffer.FirstElement = 0;
                            srvDesc.Buffer.NumElements = gfxBuffer->getSize() / 4;
                            srvDesc.Buffer.StructureByteStride = 0;
                            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            d3dDevice->CreateShaderResourceView(rawResource, &srvDesc, handle);
                        }
                    }
                    ++cbvSrvUavOffset;
                    break;
                }
                case DescriptorType::SAMPLER_TEXTURE: {
                    // Texture (SRV) part
                    auto *gfxTexture = _textures[descIdx].ptr;
                    auto *gfxSampler = _samplers[descIdx].ptr;
                    if (diagLog) {
                        const auto format = gfxTexture ? static_cast<uint32_t>(gfxTexture->getFormat()) : 0U;
                        CC_LOG_INFO("[D3D12-SET] TEX+SAMP binding=%u idx=%u hasTex=%s fmt=%u hasSampler=%s",
                                    binding.binding, descIdx, gfxTexture ? "Y" : "N", format, gfxSampler ? "Y" : "N");
                    }
                    if (gfxTexture && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Texture = static_cast<CCD3D12Texture *>(gfxTexture);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
                        if (rawResource) {
                            const auto &texInfo = gfxTexture->getInfo();
                            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                            srvDesc.Format = toSRVFormat(texInfo.format);
                            srvDesc.ViewDimension = toSRVDimension(texInfo.type, texInfo.layerCount, texInfo.samples != SampleCount::X1);
                            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                            // Fill dimension-specific fields
                            if (srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
                                srvDesc.Texture2D.MipLevels = texInfo.levelCount;
                                srvDesc.Texture2D.MostDetailedMip = 0;
                            } else if (srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
                                srvDesc.Texture2DArray.MipLevels = texInfo.levelCount;
                                srvDesc.Texture2DArray.MostDetailedMip = 0;
                                srvDesc.Texture2DArray.FirstArraySlice = 0;
                                srvDesc.Texture2DArray.ArraySize = texInfo.layerCount;
                            } else if (srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE) {
                                srvDesc.TextureCube.MipLevels = texInfo.levelCount;
                                srvDesc.TextureCube.MostDetailedMip = 0;
                            } else if (srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D) {
                                srvDesc.Texture3D.MipLevels = texInfo.levelCount;
                                srvDesc.Texture3D.MostDetailedMip = 0;
                            }

                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            d3dDevice->CreateShaderResourceView(rawResource, &srvDesc, handle);
                        }
                    }
                    ++cbvSrvUavOffset;

                    // Sampler part
                    if (gfxSampler && samplerOffset < _impl->samplerDescriptorCount) {
                        D3D12_SAMPLER_DESC samplerDesc{};
                        const auto &samplerInfo = gfxSampler->getInfo();

                        // Filter
                        if (samplerInfo.minFilter == Filter::POINT) {
                            samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                        } else if (samplerInfo.minFilter == Filter::ANISOTROPIC) {
                            samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
                        } else {
                            samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                        }

                        // Address modes
                        samplerDesc.AddressU = toAddressMode(samplerInfo.addressU);
                        samplerDesc.AddressV = toAddressMode(samplerInfo.addressV);
                        samplerDesc.AddressW = toAddressMode(samplerInfo.addressW);
                        samplerDesc.MaxAnisotropy = static_cast<UINT>(samplerInfo.maxAnisotropy);
                        samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                        samplerDesc.MinLOD = 0.0f;
                        samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
                        samplerDesc.MipLODBias = 0.0f;

                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->samplerCpuStart.ptr + samplerOffset * _impl->samplerDescriptorSize;
                        d3dDevice->CreateSampler(&samplerDesc, handle);
                    }
                    ++samplerOffset;
                    break;
                }
                case DescriptorType::TEXTURE: {
                    auto *gfxTexture = _textures[descIdx].ptr;
                    if (diagLog) {
                        const auto format = gfxTexture ? static_cast<uint32_t>(gfxTexture->getFormat()) : 0U;
                        CC_LOG_INFO("[D3D12-SET] TEX binding=%u idx=%u hasTex=%s fmt=%u",
                                    binding.binding, descIdx, gfxTexture ? "Y" : "N", format);
                    }
                    if (gfxTexture && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Texture = static_cast<CCD3D12Texture *>(gfxTexture);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
                        if (rawResource) {
                            const auto &texInfo = gfxTexture->getInfo();
                            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                            srvDesc.Format = toSRVFormat(texInfo.format);
                            srvDesc.ViewDimension = toSRVDimension(texInfo.type, texInfo.layerCount, texInfo.samples != SampleCount::X1);
                            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                            if (srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
                                srvDesc.Texture2D.MipLevels = texInfo.levelCount;
                                srvDesc.Texture2D.MostDetailedMip = 0;
                            } else if (srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
                                srvDesc.Texture2DArray.MipLevels = texInfo.levelCount;
                                srvDesc.Texture2DArray.MostDetailedMip = 0;
                                srvDesc.Texture2DArray.FirstArraySlice = 0;
                                srvDesc.Texture2DArray.ArraySize = texInfo.layerCount;
                            } else if (srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE) {
                                srvDesc.TextureCube.MipLevels = texInfo.levelCount;
                                srvDesc.TextureCube.MostDetailedMip = 0;
                            } else if (srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE3D) {
                                srvDesc.Texture3D.MipLevels = texInfo.levelCount;
                                srvDesc.Texture3D.MostDetailedMip = 0;
                            }

                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            d3dDevice->CreateShaderResourceView(rawResource, &srvDesc, handle);
                        }
                    }
                    ++cbvSrvUavOffset;
                    break;
                }
                case DescriptorType::SAMPLER: {
                    auto *gfxSampler = _samplers[descIdx].ptr;
                    if (diagLog) {
                        CC_LOG_INFO("[D3D12-SET] SAMP binding=%u idx=%u hasSampler=%s",
                                    binding.binding, descIdx, gfxSampler ? "Y" : "N");
                    }
                    if (gfxSampler && samplerOffset < _impl->samplerDescriptorCount) {
                        D3D12_SAMPLER_DESC samplerDesc{};
                        const auto &samplerInfo = gfxSampler->getInfo();

                        if (samplerInfo.minFilter == Filter::POINT) {
                            samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                        } else {
                            samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                        }
                        samplerDesc.AddressU = toAddressMode(samplerInfo.addressU);
                        samplerDesc.AddressV = toAddressMode(samplerInfo.addressV);
                        samplerDesc.AddressW = toAddressMode(samplerInfo.addressW);
                        samplerDesc.MaxAnisotropy = static_cast<UINT>(samplerInfo.maxAnisotropy);
                        samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                        samplerDesc.MinLOD = 0.0f;
                        samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;

                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->samplerCpuStart.ptr + samplerOffset * _impl->samplerDescriptorSize;
                        d3dDevice->CreateSampler(&samplerDesc, handle);
                    }
                    ++samplerOffset;
                    break;
                }
                case DescriptorType::STORAGE_IMAGE: {
                    auto *gfxTexture = _textures[descIdx].ptr;
                    if (diagLog) {
                        const auto format = gfxTexture ? static_cast<uint32_t>(gfxTexture->getFormat()) : 0U;
                        CC_LOG_INFO("[D3D12-SET] UAV binding=%u idx=%u hasTex=%s fmt=%u",
                                    binding.binding, descIdx, gfxTexture ? "Y" : "N", format);
                    }
                    if (cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                        if (gfxTexture) {
                            auto *d3d12Texture = static_cast<CCD3D12Texture *>(gfxTexture);
                            auto *rawResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
                            if (rawResource) {
                                const auto &texInfo = gfxTexture->getInfo();
                                D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                                uavDesc.Format = toSRVFormat(texInfo.format);
                                uavDesc.ViewDimension = toUAVDimension(texInfo.type, texInfo.layerCount);
                                d3dDevice->CreateUnorderedAccessView(rawResource, nullptr, &uavDesc, handle);
                            }
                            // Null resource — skip writing (descriptor heap slot remains undefined,
                            // but shader should not access unbound UAV slots)
                        }
                        // Null texture binding — skip writing descriptor
                    }
                    ++cbvSrvUavOffset;
                    break;
                }
                case DescriptorType::INPUT_ATTACHMENT: {
                    // Input attachment is treated as SRV in D3D12
                    auto *gfxTexture = _textures[descIdx].ptr;
                    if (diagLog) {
                        const auto format = gfxTexture ? static_cast<uint32_t>(gfxTexture->getFormat()) : 0U;
                        CC_LOG_INFO("[D3D12-SET] INPUT binding=%u idx=%u hasTex=%s fmt=%u",
                                    binding.binding, descIdx, gfxTexture ? "Y" : "N", format);
                    }
                    if (gfxTexture && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Texture = static_cast<CCD3D12Texture *>(gfxTexture);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
                        if (rawResource) {
                            const auto &texInfo = gfxTexture->getInfo();
                            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                            srvDesc.Format = toSRVFormat(texInfo.format);
                            srvDesc.ViewDimension = toSRVDimension(texInfo.type, texInfo.layerCount, texInfo.samples != SampleCount::X1);
                            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                            if (srvDesc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
                                srvDesc.Texture2D.MipLevels = texInfo.levelCount;
                            }

                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            d3dDevice->CreateShaderResourceView(rawResource, &srvDesc, handle);
                        }
                    }
                    ++cbvSrvUavOffset;
                    break;
                }
                default:
                    break;
            }
        }
    }
#endif

    if (diagLog) {
        ++s_diagDescriptorSetLogCount;
    }
    _isDirty = false;
}

void *CCD3D12DescriptorSet::getCbvSrvUavDescriptorHeap() const {
#if defined(_WIN32)
    return (_impl && _impl->cbvSrvUavHeap) ? _impl->cbvSrvUavHeap.Get() : nullptr;
#else
    return nullptr;
#endif
}

void *CCD3D12DescriptorSet::getSamplerDescriptorHeap() const {
#if defined(_WIN32)
    return (_impl && _impl->samplerHeap) ? _impl->samplerHeap.Get() : nullptr;
#else
    return nullptr;
#endif
}

uint32_t CCD3D12DescriptorSet::getCbvSrvUavDescriptorCount() const {
#if defined(_WIN32)
    return _impl ? _impl->cbvSrvUavDescriptorCount : 0;
#else
    return 0;
#endif
}

uint32_t CCD3D12DescriptorSet::getSamplerDescriptorCount() const {
#if defined(_WIN32)
    return _impl ? _impl->samplerDescriptorCount : 0;
#else
    return 0;
#endif
}

void CCD3D12DescriptorSet::applyDynamicOffsets(uint32_t dynamicOffsetCount, const uint32_t *dynamicOffsets) {
#if defined(_WIN32)
    if (!_impl || !_layout || dynamicOffsetCount == 0 || !dynamicOffsets) {
        return;
    }

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        return;
    }

    const auto &bindings = _layout->getBindings();
    const auto &descriptorIndices = _layout->getDescriptorIndices();

    uint32_t cbvSrvUavOffset = 0;
    uint32_t dynamicOffsetIndex = 0;

    for (const auto &binding : bindings) {
        const uint32_t baseDescIdx = descriptorIndices[binding.binding];

        for (uint32_t i = 0; i < binding.count; ++i) {
            const uint32_t descIdx = baseDescIdx + i;

            switch (binding.descriptorType) {
                case DescriptorType::UNIFORM_BUFFER:
                case DescriptorType::SAMPLER_TEXTURE:
                case DescriptorType::TEXTURE:
                case DescriptorType::SAMPLER:
                case DescriptorType::STORAGE_IMAGE:
                case DescriptorType::INPUT_ATTACHMENT:
                    if (binding.descriptorType != DescriptorType::SAMPLER &&
                        binding.descriptorType != DescriptorType::SAMPLER_TEXTURE) {
                        ++cbvSrvUavOffset;
                    } else {
                        ++cbvSrvUavOffset;
                    }
                    if (binding.descriptorType == DescriptorType::SAMPLER_TEXTURE) {
                        // sampler part is stored in the separate sampler heap; no CBV/SRV/UAV rewrite needed here
                    }
                    break;
                case DescriptorType::DYNAMIC_UNIFORM_BUFFER: {
                    auto *gfxBuffer = _buffers[descIdx].ptr;
                    if (gfxBuffer && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(gfxBuffer);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
                        if (rawResource) {
                            const uint32_t dynamicOffset = dynamicOffsetIndex < dynamicOffsetCount ? dynamicOffsets[dynamicOffsetIndex] : 0;
                            const uint64_t totalOffset = static_cast<uint64_t>(d3d12Buffer->getD3D12ResourceOffset()) + dynamicOffset;
                            const uint64_t resourceWidth = rawResource->GetDesc().Width;
                            const uint64_t bufferSize = gfxBuffer->getSize();
                            const uint64_t availableSize = (bufferSize > dynamicOffset) ? (bufferSize - dynamicOffset) : 0;
                            const uint64_t cbvSize = (availableSize + 255ULL) & ~255ULL;

                            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
                            cbvDesc.BufferLocation = d3d12Buffer->getD3D12GPUVirtualAddress() + dynamicOffset;
                            cbvDesc.SizeInBytes = static_cast<UINT>(std::min<uint64_t>(cbvSize, resourceWidth > totalOffset ? resourceWidth - totalOffset : 0));

                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            if (cbvDesc.SizeInBytes >= 256U) {
                                d3dDevice->CreateConstantBufferView(&cbvDesc, handle);
                            } else {
                                d3dDevice->CreateConstantBufferView(nullptr, handle);
                            }
                        }
                    }
                    ++dynamicOffsetIndex;
                    ++cbvSrvUavOffset;
                    break;
                }
                case DescriptorType::DYNAMIC_STORAGE_BUFFER: {
                    auto *gfxBuffer = _buffers[descIdx].ptr;
                    if (gfxBuffer && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(gfxBuffer);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
                        if (rawResource) {
                            const uint32_t dynamicOffset = dynamicOffsetIndex < dynamicOffsetCount ? dynamicOffsets[dynamicOffsetIndex] : 0;
                            const uint32_t firstElement = dynamicOffset / 4U;
                            const uint32_t availableSize = gfxBuffer->getSize() > dynamicOffset ? (gfxBuffer->getSize() - dynamicOffset) : 0U;

                            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                            srvDesc.Buffer.FirstElement = firstElement;
                            srvDesc.Buffer.NumElements = availableSize / 4U;
                            srvDesc.Buffer.StructureByteStride = 0;
                            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            d3dDevice->CreateShaderResourceView(rawResource, &srvDesc, handle);
                        }
                    }
                    ++dynamicOffsetIndex;
                    ++cbvSrvUavOffset;
                    break;
                }
                case DescriptorType::STORAGE_BUFFER:
                    ++cbvSrvUavOffset;
                    break;
                default:
                    break;
            }
        }
    }
#else
    (void)dynamicOffsetCount;
    (void)dynamicOffsets;
#endif
}

} // namespace gfx
} // namespace cc
