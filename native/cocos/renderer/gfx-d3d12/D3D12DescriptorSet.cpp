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

// File diagnostic for descriptor set binding
#include <cstdio>
#include <cstdarg>
namespace {
void dsDiagLog(const char *fmt, ...) {
    static FILE *s_file = nullptr;
    if (!s_file) {
        s_file = fopen("C:\\temp\\d3d12-ds-diag.log", "a");
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
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.Buffer.FirstElement = 0;
                srvDesc.Buffer.NumElements = 1;
                srvDesc.Buffer.StructureByteStride = 0;
                srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
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
    static uint32_t s_diagDescriptorSetLogCount = 0;
    const bool diagLog = s_diagDescriptorSetLogCount < 24 && _layout && _layout->getDescriptorCount() > 0;

    // File diagnostic: keep a larger startup window so we can capture
    // descriptor writes after splash-screen and into real scene draws.
    static uint32_t s_fileDiagCount = 0;
    const bool fileDiag = s_fileDiagCount < 2000;
    if (fileDiag) {
        dsDiagLog("[FORCE-UPDATE] #%u: set=%p layout=%p bindings=%zu cbvSrvUavCount=%u samplerCount=%u descriptorCount=%u\n",
                  s_fileDiagCount, this, _layout, bindings.size(),
                  _impl->cbvSrvUavDescriptorCount, _impl->samplerDescriptorCount,
                  _layout->getDescriptorCount());
    }

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
                        }
                    }
                    if (fileDiag) {
                        auto *d3d12Buf = gfxBuffer ? static_cast<CCD3D12Buffer *>(gfxBuffer) : nullptr;
                        void *rawRes = d3d12Buf ? d3d12Buf->getD3D12ResourceHandle() : nullptr;
                        uint64_t gpuVA = d3d12Buf ? d3d12Buf->getD3D12GPUVirtualAddress() : 0;
                        dsDiagLog("  CBV binding=%u descIdx=%u hasBuf=%s rawRes=%p gpuVA=0x%llx heapOffset=%u\n",
                                  binding.binding, descIdx, gfxBuffer ? "Y" : "N", rawRes,
                                  static_cast<unsigned long long>(gpuVA), cbvSrvUavOffset);
                    }
                    if (gfxBuffer && cbvSrvUavOffset < _impl->cbvSrvUavDescriptorCount) {
                        auto *d3d12Buffer = static_cast<CCD3D12Buffer *>(gfxBuffer);
                        auto *rawResource = static_cast<ID3D12Resource *>(d3d12Buffer->getD3D12ResourceHandle());
                        if (rawResource) {
                            const UINT64 resourceWidth = rawResource->GetDesc().Width;
                            const UINT64 resourceOffset = static_cast<UINT64>(d3d12Buffer->getD3D12ResourceOffset());
                            const UINT64 availableSize = (resourceWidth > resourceOffset) ? (resourceWidth - resourceOffset) : 0ULL;
                            const UINT64 logicalAligned = static_cast<UINT64>(gfxBuffer->getSize()) & ~255ULL;
                            // For CBV, prefer full available range in the underlying allocation
                            // (bounded by D3D12's 64KB CBV limit) instead of gfxBuffer->getSize(),
                            // which can be smaller than actual shader block usage in some paths.
                            const UINT64 availableAligned = availableSize & ~255ULL;
                            const UINT64 cbvSize = std::min<UINT64>(availableAligned, 64ULL * 1024ULL);
                            if (logicalAligned >= 256ULL && availableAligned < logicalAligned) {
                                CC_LOG_WARNING("[D3D12-CBV] available range smaller than logical buffer size: binding=%u descIdx=%u logical=%llu available=%llu rawWidth=%llu rawOffset=%llu",
                                               binding.binding,
                                               descIdx,
                                               static_cast<unsigned long long>(logicalAligned),
                                               static_cast<unsigned long long>(availableAligned),
                                               static_cast<unsigned long long>(resourceWidth),
                                               static_cast<unsigned long long>(resourceOffset));
                            }
                            if (cbvSize >= 256U) {
                                D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
                                cbvDesc.BufferLocation = d3d12Buffer->getD3D12GPUVirtualAddress();
                                cbvDesc.SizeInBytes = static_cast<UINT>(cbvSize);

                                D3D12_CPU_DESCRIPTOR_HANDLE handle;
                                handle.ptr = _impl->cbvSrvUavCpuStart.ptr + cbvSrvUavOffset * _impl->cbvSrvUavDescriptorSize;
                                d3dDevice->CreateConstantBufferView(&cbvDesc, handle);

                                // Verify: read back the CBV desc we just wrote
                                if (fileDiag && cbvSrvUavOffset < 4) {
                                    D3D12_CONSTANT_BUFFER_VIEW_DESC verifyDesc{};
                                    UINT verifySize = 0;
                                    d3dDevice->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, nullptr, 0); // no-op
                                    // Just log what we wrote
                                    dsDiagLog("    CBV WRITE: heapOffset=%u gpuVA=0x%llx sizeInBytes=%u avail=%llu handle=0x%llx\n",
                                              cbvSrvUavOffset,
                                              static_cast<unsigned long long>(cbvDesc.BufferLocation),
                                              cbvDesc.SizeInBytes,
                                              static_cast<unsigned long long>(availableAligned),
                                              static_cast<unsigned long long>(handle.ptr));
                                }
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
                        D3D12_SAMPLER_DESC samplerDesc{};
                        const auto &samplerInfo = gfxSampler->getInfo();

                        if (samplerInfo.minFilter == Filter::POINT) {
                            samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                        } else if (samplerInfo.minFilter == Filter::ANISOTROPIC) {
                            samplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
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
                        samplerDesc.MipLODBias = 0.0f;

                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->samplerCpuStart.ptr + samplerOffset * _impl->samplerDescriptorSize;
                        d3dDevice->CreateSampler(&samplerDesc, handle);
                    } else if (samplerOffset < _impl->samplerDescriptorCount) {
                        // Null sampler binding: write default sampler to keep heap slot valid.
                        D3D12_SAMPLER_DESC defaultSampler{};
                        defaultSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                        defaultSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                        defaultSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                        defaultSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                        defaultSampler.MaxAnisotropy = 1;
                        defaultSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                        defaultSampler.MinLOD = 0.0f;
                        defaultSampler.MaxLOD = D3D12_FLOAT32_MAX;
                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->samplerCpuStart.ptr + samplerOffset * _impl->samplerDescriptorSize;
                        d3dDevice->CreateSampler(&defaultSampler, handle);
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
                    } else if (samplerOffset < _impl->samplerDescriptorCount) {
                        // Null sampler binding: write default sampler to keep heap slot valid.
                        D3D12_SAMPLER_DESC defaultSampler{};
                        defaultSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                        defaultSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                        defaultSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                        defaultSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                        defaultSampler.MaxAnisotropy = 1;
                        defaultSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                        defaultSampler.MinLOD = 0.0f;
                        defaultSampler.MaxLOD = D3D12_FLOAT32_MAX;
                        D3D12_CPU_DESCRIPTOR_HANDLE handle;
                        handle.ptr = _impl->samplerCpuStart.ptr + samplerOffset * _impl->samplerDescriptorSize;
                        d3dDevice->CreateSampler(&defaultSampler, handle);
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
#endif

    if (diagLog) {
        ++s_diagDescriptorSetLogCount;
    }
    if (fileDiag) {
        dsDiagLog("[FORCE-UPDATE] #%u done: final cbvSrvUavOffset=%u samplerOffset=%u\n",
                  s_fileDiagCount, cbvSrvUavOffset, samplerOffset);
        ++s_fileDiagCount;
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
                    // Keep offset traversal consistent with forceUpdate():
                    // SAMPLER has no CBV/SRV/UAV slot; others here consume one.
                    if (binding.descriptorType != DescriptorType::SAMPLER) {
                        ++cbvSrvUavOffset;
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
                            const uint64_t availableSize = (resourceWidth > totalOffset) ? (resourceWidth - totalOffset) : 0ULL;
                            // The dynamic offset selects an element inside the backing allocation.
                            // It must not shrink the logical size of the bound buffer view.
                            const uint64_t requiredAligned = (static_cast<uint64_t>(gfxBuffer->getSize()) + 255ULL) & ~255ULL;
                            const uint64_t availableAligned = availableSize & ~255ULL;
                            const uint64_t cbvSize = std::min<uint64_t>(requiredAligned, 64ULL * 1024ULL);
                            if (availableAligned < cbvSize) {
                                CC_LOG_ERROR("[D3D12-CBV-DYN] dynamic CBV range is too small: binding=%u descIdx=%u dynOffset=%u required=%llu available=%llu rawWidth=%llu totalOffset=%llu",
                                               binding.binding,
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
