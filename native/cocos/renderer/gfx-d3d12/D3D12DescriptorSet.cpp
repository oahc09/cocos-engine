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
                        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
                        cbvDesc.BufferLocation = d3d12Buffer->getD3D12GPUVirtualAddress();
                        cbvDesc.SizeInBytes = (gfxBuffer->getSize() + 255) & ~255; // D3D12 CBV size must be 256-byte aligned

                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                        d3dDevice->CreateConstantBufferView(&cbvDesc, handle);
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
                    if (gfxTexture && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Texture = static_cast<CCD3D12Texture *>(gfxTexture);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
                        if (rawResource) {
                            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // default, should use actual format
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                            srvDesc.Texture2D.MipLevels = 1;
                            srvDesc.Texture2D.MostDetailedMip = 0;

                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            d3dDevice->CreateShaderResourceView(rawResource, &srvDesc, handle);
                        }
                    }
                    ++cbvSrvUavOffset;

                    // Sampler part
                    auto *gfxSampler = _samplers[descIdx].ptr;
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
                        auto toAddressMode = [](Address addr) -> D3D12_TEXTURE_ADDRESS_MODE {
                            switch (addr) {
                                case Address::WRAP: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                                case Address::MIRROR: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
                                case Address::CLAMP: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                                case Address::BORDER: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
                                default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                            }
                        };
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
                    if (gfxTexture && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Texture = static_cast<CCD3D12Texture *>(gfxTexture);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
                        if (rawResource) {
                            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                            srvDesc.Texture2D.MipLevels = 1;
                            srvDesc.Texture2D.MostDetailedMip = 0;

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
                    if (gfxSampler && samplerOffset < _impl->samplerDescriptorCount) {
                        D3D12_SAMPLER_DESC samplerDesc{};
                        const auto &samplerInfo = gfxSampler->getInfo();

                        if (samplerInfo.minFilter == Filter::POINT) {
                            samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                        } else {
                            samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                        }
                        auto toAddressMode = [](Address addr) -> D3D12_TEXTURE_ADDRESS_MODE {
                            switch (addr) {
                                case Address::WRAP: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                                case Address::MIRROR: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
                                case Address::CLAMP: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                                case Address::BORDER: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
                                default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                            }
                        };
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
                    if (gfxTexture && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Texture = static_cast<CCD3D12Texture *>(gfxTexture);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Texture->getD3D12ResourceHandle());
                        if (rawResource) {
                            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                            uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

                            D3D12_CPU_DESCRIPTOR_HANDLE handle;
                            handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                            d3dDevice->CreateUnorderedAccessView(rawResource, nullptr, &uavDesc, handle);
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
                            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                            srvDesc.Texture2D.MipLevels = 1;

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

    _isDirty = false;
}

} // namespace gfx
} // namespace cc
