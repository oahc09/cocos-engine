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

#include "D3D12Framebuffer.h"
#include "D3D12DebugOptimization.h"
#include "D3D12Device.h"
#include "D3D12Swapchain.h"
#include "D3D12Texture.h"
#include "D3D12RenderPass.h"
#include "base/Log.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <algorithm>
    #include <d3d12.h>
    #include <dxgiformat.h>
    #include <wrl/client.h>

namespace cc {
namespace gfx {

namespace {

DXGI_FORMAT toD3D12DSVFormat(Format format) {
    switch (format) {
        case Format::DEPTH:
            return DXGI_FORMAT_D32_FLOAT;
        case Format::DEPTH_STENCIL:
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

bool buildAttachmentSnapshot(
    CCD3D12Texture *texture,
    uint64_t deviceEpoch,
    D3D12FramebufferAttachment &attachment) {
    if (!texture) {
        return false;
    }

    const auto backing = texture->getD3D12ResourceBacking();
    if (!backing || !backing->valid || !backing->resource ||
        backing->deviceEpoch != deviceEpoch) {
        return false;
    }

    const auto &info = texture->getInfo();
    const auto &view = texture->getViewInfo();
    attachment = {};
    attachment.backing = backing;
    attachment.format = texture->getFormat();
    attachment.type = texture->isTextureView() ? view.type : info.type;
    attachment.samples = info.samples;
    attachment.baseMip = texture->isTextureView() ? view.baseLevel : 0;
    attachment.mipCount = 1;
    attachment.baseLayer = texture->isTextureView() ? view.baseLayer : 0;
    attachment.layerCount = texture->isTextureView()
                                ? std::max(view.layerCount, 1U)
                                : std::max(info.layerCount, 1U);
    attachment.basePlane = texture->isTextureView() ? view.basePlane : 0;
    attachment.planeCount = texture->isTextureView()
                                ? std::max(view.planeCount, 1U)
                                : backing->planeCount;
    attachment.resourceId = reinterpret_cast<uintptr_t>(texture);
    attachment.backingGeneration = backing->generation;
    attachment.isSwapchain = texture->isSwapchainColorTexture();

    if (texture->isTextureView() && view.levelCount != 1) {
        return false;
    }
    if (attachment.baseMip >= backing->mipLevels ||
        attachment.basePlane >= backing->planeCount ||
        attachment.planeCount > backing->planeCount - attachment.basePlane) {
        return false;
    }

    attachment.width = std::max(info.width >> attachment.baseMip, 1U);
    attachment.height = attachment.type == TextureType::TEX1D ||
                                attachment.type == TextureType::TEX1D_ARRAY
                            ? 1U
                            : std::max(info.height >> attachment.baseMip, 1U);

    if (attachment.type == TextureType::TEX3D) {
        const uint32_t mipDepth = std::max(info.depth >> attachment.baseMip, 1U);
        if (!texture->isTextureView()) {
            attachment.baseLayer = 0;
            attachment.layerCount = mipDepth;
        }
        if (attachment.baseLayer >= mipDepth ||
            attachment.layerCount > mipDepth - attachment.baseLayer) {
            return false;
        }
    } else if (attachment.baseLayer >= backing->arraySize ||
               attachment.layerCount > backing->arraySize - attachment.baseLayer) {
        return false;
    }

    return true;
}

bool makeRenderTargetViewDesc(
    const D3D12FramebufferAttachment &attachment,
    const D3D12_RESOURCE_DESC &resourceDesc,
    D3D12_RENDER_TARGET_VIEW_DESC &desc) {
    desc = {};
    desc.Format = toD3D12Format(attachment.format);
    if (desc.Format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }

    switch (resourceDesc.Dimension) {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            if (resourceDesc.DepthOrArraySize > 1 || attachment.baseLayer > 0 ||
                attachment.layerCount > 1) {
                desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
                desc.Texture1DArray.MipSlice = attachment.baseMip;
                desc.Texture1DArray.FirstArraySlice = attachment.baseLayer;
                desc.Texture1DArray.ArraySize = attachment.layerCount;
            } else {
                desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
                desc.Texture1D.MipSlice = attachment.baseMip;
            }
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            if (resourceDesc.SampleDesc.Count > 1) {
                if (attachment.baseMip != 0 || attachment.basePlane != 0) {
                    return false;
                }
                if (resourceDesc.DepthOrArraySize > 1 || attachment.baseLayer > 0 ||
                    attachment.layerCount > 1) {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                    desc.Texture2DMSArray.FirstArraySlice = attachment.baseLayer;
                    desc.Texture2DMSArray.ArraySize = attachment.layerCount;
                } else {
                    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
                }
            } else if (resourceDesc.DepthOrArraySize > 1 || attachment.baseLayer > 0 ||
                       attachment.layerCount > 1) {
                desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.MipSlice = attachment.baseMip;
                desc.Texture2DArray.FirstArraySlice = attachment.baseLayer;
                desc.Texture2DArray.ArraySize = attachment.layerCount;
                desc.Texture2DArray.PlaneSlice = attachment.basePlane;
            } else {
                desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipSlice = attachment.baseMip;
                desc.Texture2D.PlaneSlice = attachment.basePlane;
            }
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            if (resourceDesc.SampleDesc.Count > 1) {
                return false;
            }
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            desc.Texture3D.MipSlice = attachment.baseMip;
            desc.Texture3D.FirstWSlice = attachment.baseLayer;
            desc.Texture3D.WSize = attachment.layerCount;
            break;
        default:
            return false;
    }
    return true;
}

bool makeDepthStencilViewDesc(
    const D3D12FramebufferAttachment &attachment,
    const D3D12_RESOURCE_DESC &resourceDesc,
    D3D12_DEPTH_STENCIL_VIEW_DESC &desc) {
    desc = {};
    desc.Format = toD3D12DSVFormat(attachment.format);
    if (desc.Format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    desc.Flags = D3D12_DSV_FLAG_NONE;

    switch (resourceDesc.Dimension) {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            if (resourceDesc.DepthOrArraySize > 1 || attachment.baseLayer > 0 ||
                attachment.layerCount > 1) {
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
                desc.Texture1DArray.MipSlice = attachment.baseMip;
                desc.Texture1DArray.FirstArraySlice = attachment.baseLayer;
                desc.Texture1DArray.ArraySize = attachment.layerCount;
            } else {
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
                desc.Texture1D.MipSlice = attachment.baseMip;
            }
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            if (resourceDesc.SampleDesc.Count > 1) {
                if (attachment.baseMip != 0) {
                    return false;
                }
                if (resourceDesc.DepthOrArraySize > 1 || attachment.baseLayer > 0 ||
                    attachment.layerCount > 1) {
                    desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                    desc.Texture2DMSArray.FirstArraySlice = attachment.baseLayer;
                    desc.Texture2DMSArray.ArraySize = attachment.layerCount;
                } else {
                    desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
                }
            } else if (resourceDesc.DepthOrArraySize > 1 || attachment.baseLayer > 0 ||
                       attachment.layerCount > 1) {
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.MipSlice = attachment.baseMip;
                desc.Texture2DArray.FirstArraySlice = attachment.baseLayer;
                desc.Texture2DArray.ArraySize = attachment.layerCount;
            } else {
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipSlice = attachment.baseMip;
            }
            break;
        default:
            return false;
    }

    return true;
}

} // namespace

struct CCD3D12Framebuffer::Impl {
    // RTV descriptor heap (one heap for all render targets)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    // DSV descriptor heap
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;

    // Store CPU descriptor handles
    ccstd::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    ccstd::vector<D3D12FramebufferAttachment> colorAttachments;
    D3D12FramebufferAttachment depthStencilAttachment;
    bool hasDepthStencil{false};

    uint32_t width{0};
    uint32_t height{0};
    uint32_t rtvDescriptorSize{0};
    uint64_t deviceEpoch{0};
    bool valid{false};
};

CCD3D12Framebuffer::CCD3D12Framebuffer() {
    _impl = std::make_unique<Impl>();
}

CCD3D12Framebuffer::~CCD3D12Framebuffer() {
    destroy();
}

void CCD3D12Framebuffer::doInit(const FramebufferInfo &info) {
    if (!_impl) return;
    _swapchain = nullptr;
    _isOffscreen = true;
    _impl->rtvHeap.Reset();
    _impl->dsvHeap.Reset();
    _impl->rtvHandles.clear();
    _impl->dsvHandle = D3D12_CPU_DESCRIPTOR_HANDLE{};
    _impl->colorAttachments.clear();
    _impl->depthStencilAttachment = {};
    _impl->hasDepthStencil = false;
    _impl->width = 0;
    _impl->height = 0;
    _impl->rtvDescriptorSize = 0;
    _impl->deviceEpoch = 0;
    _impl->valid = false;

    auto *device = CCD3D12Device::getInstance();
    auto *d3dDevice = static_cast<ID3D12Device *>(device ? device->getD3D12DeviceHandle() : nullptr);
    if (!d3dDevice) {
        CC_LOG_ERROR("D3D12Framebuffer: device unavailable.");
        return;
    }
    _impl->deviceEpoch = device->getDeviceEpoch();

    const uint32_t colorCount = static_cast<uint32_t>(_colorTextures.size());
    // MAX_ATTACHMENTS is the engine-wide bound used by CommandBuffer's stack
    // arrays. D3D12 supports more RTVs, but accepting them here would make
    // beginRenderPass access those fixed-size arrays out of bounds.
    if (colorCount > MAX_ATTACHMENTS) {
        CC_LOG_ERROR("D3D12Framebuffer: %u color attachments exceed the engine limit of %u.",
                     colorCount, MAX_ATTACHMENTS);
        return;
    }
    const auto *renderPassColors =
        info.renderPass ? &info.renderPass->getColorAttachments() : nullptr;
    if (renderPassColors && colorCount != renderPassColors->size()) {
        CC_LOG_ERROR(
            "D3D12Framebuffer: color attachment count %u does not match render pass count %zu.",
            colorCount, renderPassColors->size());
        return;
    }

    if (colorCount == 0 && !_depthStencilTexture) {
        CC_LOG_ERROR("D3D12Framebuffer: no attachments were provided (deviceEpoch=%llu).",
                     static_cast<unsigned long long>(_impl->deviceEpoch));
        return;
    }

    _impl->colorAttachments.resize(colorCount);
    bool hasSizeReference = false;
    for (uint32_t i = 0; i < colorCount; ++i) {
        auto *texture = static_cast<CCD3D12Texture *>(_colorTextures[i]);
        auto &attachmentInfo = _impl->colorAttachments[i];
        if (!texture || !buildAttachmentSnapshot(texture, _impl->deviceEpoch, attachmentInfo)) {
            CC_LOG_ERROR("D3D12Framebuffer: color[%u] is null, deviceEpoch=%llu.",
                         i, static_cast<unsigned long long>(_impl->deviceEpoch));
            return;
        }
        if (!hasSizeReference) {
            _impl->width = attachmentInfo.width;
            _impl->height = attachmentInfo.height;
            hasSizeReference = true;
        }
        if (attachmentInfo.width != _impl->width ||
            attachmentInfo.height != _impl->height) {
            CC_LOG_ERROR(
                "D3D12Framebuffer: invalid color[%u] resourceId=0x%llx resource=%p "
                "size=%ux%u expected=%ux%u format=%u samples=%u "
                "resourceEpoch=%llu deviceEpoch=%llu.",
                i, static_cast<unsigned long long>(attachmentInfo.resourceId),
                attachmentInfo.backing->resource.Get(),
                attachmentInfo.width, attachmentInfo.height, _impl->width, _impl->height,
                static_cast<unsigned>(attachmentInfo.format),
                static_cast<unsigned>(attachmentInfo.samples),
                static_cast<unsigned long long>(attachmentInfo.backing->deviceEpoch),
                static_cast<unsigned long long>(_impl->deviceEpoch));
            return;
        }
        if (renderPassColors) {
            const auto &attachment = (*renderPassColors)[i];
            const SampleCount expectedSamples = getD3D12EffectiveSampleCount(attachment.sampleCount);
            if ((attachment.format != Format::UNKNOWN && attachment.format != attachmentInfo.format) ||
                expectedSamples != attachmentInfo.samples) {
                CC_LOG_ERROR("D3D12Framebuffer: color[%u] resourceId=0x%llx does not match render pass: "
                             "format=%u expected=%u samples=%u expected=%u, deviceEpoch=%llu.",
                             i, static_cast<unsigned long long>(attachmentInfo.resourceId),
                             static_cast<unsigned>(attachmentInfo.format),
                             static_cast<unsigned>(attachment.format),
                             static_cast<unsigned>(attachmentInfo.samples),
                             static_cast<unsigned>(expectedSamples),
                             static_cast<unsigned long long>(_impl->deviceEpoch));
                return;
            }
        }
        if (attachmentInfo.isSwapchain) {
            auto *swapchain = static_cast<CCD3D12Swapchain *>(texture->getSwapchain());
            if (!swapchain || (_swapchain && _swapchain != swapchain)) {
                CC_LOG_ERROR("D3D12Framebuffer: color[%u] resourceId=0x%llx has an invalid or mismatched swapchain, "
                             "deviceEpoch=%llu.",
                             i, static_cast<unsigned long long>(attachmentInfo.resourceId),
                             static_cast<unsigned long long>(_impl->deviceEpoch));
                return;
            }
            _swapchain = swapchain;
            _isOffscreen = false;
            // Swapchain framebuffers resolve the current backing dynamically.
            // Retaining this snapshot would keep an IDXGISwapChain back buffer
            // alive and make ResizeBuffers fail with DXGI_ERROR_INVALID_CALL.
            attachmentInfo.backing.reset();
        }
    }

    if (_depthStencilTexture) {
        auto *texture = static_cast<CCD3D12Texture *>(_depthStencilTexture);
        auto &attachmentInfo = _impl->depthStencilAttachment;
        if (!buildAttachmentSnapshot(texture, _impl->deviceEpoch, attachmentInfo)) {
            CC_LOG_ERROR("D3D12Framebuffer: invalid depth resourceId=0x%llx deviceEpoch=%llu.",
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(texture)),
                         static_cast<unsigned long long>(_impl->deviceEpoch));
            return;
        }
        if (!hasSizeReference) {
            _impl->width = attachmentInfo.width;
            _impl->height = attachmentInfo.height;
            hasSizeReference = true;
        }
        if (attachmentInfo.width != _impl->width ||
            attachmentInfo.height != _impl->height) {
            CC_LOG_ERROR("D3D12Framebuffer: invalid depth resourceId=0x%llx resource=%p "
                         "size=%ux%u expected=%ux%u format=%u samples=%u "
                         "resourceEpoch=%llu deviceEpoch=%llu.",
                         static_cast<unsigned long long>(attachmentInfo.resourceId),
                         attachmentInfo.backing->resource.Get(),
                         attachmentInfo.width, attachmentInfo.height,
                         _impl->width, _impl->height,
                         static_cast<unsigned>(attachmentInfo.format),
                         static_cast<unsigned>(attachmentInfo.samples),
                         static_cast<unsigned long long>(attachmentInfo.backing->deviceEpoch),
                         static_cast<unsigned long long>(_impl->deviceEpoch));
            return;
        }
        if (info.renderPass) {
            const auto &attachment = info.renderPass->getDepthStencilAttachment();
            const SampleCount expectedSamples = getD3D12EffectiveSampleCount(attachment.sampleCount);
            if ((attachment.format != Format::UNKNOWN && attachment.format != attachmentInfo.format) ||
                expectedSamples != attachmentInfo.samples) {
                CC_LOG_ERROR("D3D12Framebuffer: depth resourceId=0x%llx does not match render pass: "
                             "format=%u expected=%u samples=%u expected=%u, deviceEpoch=%llu.",
                             static_cast<unsigned long long>(attachmentInfo.resourceId),
                             static_cast<unsigned>(attachmentInfo.format),
                             static_cast<unsigned>(attachment.format),
                             static_cast<unsigned>(attachmentInfo.samples),
                             static_cast<unsigned>(expectedSamples),
                             static_cast<unsigned long long>(_impl->deviceEpoch));
                return;
            }
        }
        _impl->hasDepthStencil = true;
    }

    if (!hasSizeReference || _impl->width == 0 || _impl->height == 0) {
        CC_LOG_ERROR("D3D12Framebuffer: attachments have invalid size %ux%u, deviceEpoch=%llu.",
                     _impl->width, _impl->height,
                     static_cast<unsigned long long>(_impl->deviceEpoch));
        return;
    }

    // Create RTV descriptor heap and render target views
    if (colorCount > 0) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = colorCount;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        rtvHeapDesc.NodeMask = 0;

        HRESULT hr = d3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&_impl->rtvHeap));
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12Framebuffer: CreateDescriptorHeap(RTV) failed. HRESULT=0x%08x",
                         static_cast<unsigned>(hr));
            return;
        }

        _impl->rtvDescriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        _impl->rtvHandles.resize(colorCount);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = _impl->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < colorCount; ++i) {
            auto handleSlot = rtvHandle;
            rtvHandle.ptr += _impl->rtvDescriptorSize;

            const auto &attachmentInfo = _impl->colorAttachments[i];

            // Detect swapchain color textures — their RTVs are managed by the swapchain
            if (attachmentInfo.isSwapchain) {
                // Leave placeholder handle; getRTVHandle() returns swapchain's RTV dynamically
                _impl->rtvHandles[i] = D3D12_CPU_DESCRIPTOR_HANDLE{};
                continue;
            }

            auto *resource = attachmentInfo.backing->resource.Get();
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
            if (!makeRenderTargetViewDesc(attachmentInfo, resource->GetDesc(), rtvDesc)) {
                CC_LOG_ERROR("D3D12Framebuffer: unsupported RTV attachment[%u] resourceId=0x%llx "
                             "format=%u type=%u mip=%u layer=%u+%u.",
                             i, static_cast<unsigned long long>(attachmentInfo.resourceId),
                             static_cast<unsigned>(attachmentInfo.format),
                             static_cast<unsigned>(attachmentInfo.type),
                             attachmentInfo.baseMip, attachmentInfo.baseLayer,
                             attachmentInfo.layerCount);
                return;
            }
            d3dDevice->CreateRenderTargetView(resource, &rtvDesc, handleSlot);
            _impl->rtvHandles[i] = handleSlot;
        }
    }

    // Create DSV descriptor heap and depth-stencil view
    if (_impl->hasDepthStencil) {
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        dsvHeapDesc.NodeMask = 0;

        HRESULT hr = d3dDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&_impl->dsvHeap));
        if (FAILED(hr)) {
            CC_LOG_ERROR("D3D12Framebuffer: CreateDescriptorHeap(DSV) failed. HRESULT=0x%08x",
                         static_cast<unsigned>(hr));
            return;
        }

        const auto &attachmentInfo = _impl->depthStencilAttachment;
        auto *resource = attachmentInfo.backing->resource.Get();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        if (!makeDepthStencilViewDesc(attachmentInfo, resource->GetDesc(), dsvDesc)) {
            CC_LOG_ERROR("D3D12Framebuffer: unsupported DSV attachment resourceId=0x%llx "
                         "format=%u type=%u mip=%u layer=%u+%u.",
                         static_cast<unsigned long long>(attachmentInfo.resourceId),
                         static_cast<unsigned>(attachmentInfo.format),
                         static_cast<unsigned>(attachmentInfo.type),
                         attachmentInfo.baseMip, attachmentInfo.baseLayer,
                         attachmentInfo.layerCount);
            return;
        }
        _impl->dsvHandle = _impl->dsvHeap->GetCPUDescriptorHandleForHeapStart();
        d3dDevice->CreateDepthStencilView(resource, &dsvDesc, _impl->dsvHandle);
    }

    _impl->valid = true;
    CC_D3D12_DIAGNOSTIC_LOG("D3D12Framebuffer initialized: %u color attachments, size=%ux%u, swapchain=%s",
                colorCount, _impl->width, _impl->height, _swapchain ? "yes" : "no");
}

void CCD3D12Framebuffer::doDestroy() {
    if (_impl) {
        _impl->rtvHeap.Reset();
        _impl->dsvHeap.Reset();
        _impl->rtvHandles.clear();
        _impl->colorAttachments.clear();
        _impl->depthStencilAttachment = {};
        _impl->hasDepthStencil = false;
        _impl->dsvHandle = D3D12_CPU_DESCRIPTOR_HANDLE{};
        _impl->width = 0;
        _impl->height = 0;
        _impl->rtvDescriptorSize = 0;
        _impl->deviceEpoch = 0;
        _impl->valid = false;
    }
    _swapchain = nullptr;
    _isOffscreen = true;
}

CCD3D12Framebuffer::DescriptorPair CCD3D12Framebuffer::getRTVHandle(uint32_t index) const {
    if (!_impl) return {};
    if (index >= _impl->rtvHandles.size()) return {};

    if (index < _impl->colorAttachments.size() &&
        _impl->colorAttachments[index].isSwapchain) {
        if (_swapchain) {
            uintptr_t rtvPtr = _swapchain->getCurrentRTVHandle();
            return {static_cast<uint64_t>(rtvPtr), 0};
        }
        return {};
    }

    const auto &handle = _impl->rtvHandles[index];
    return {static_cast<uint64_t>(handle.ptr), 0};
}

CCD3D12Framebuffer::DescriptorPair CCD3D12Framebuffer::getDSVHandle() const {
    if (!_impl) return {};
    return {static_cast<uint64_t>(_impl->dsvHandle.ptr), 0};
}

uint32_t CCD3D12Framebuffer::getWidth() const {
    return _impl ? _impl->width : 0;
}

uint32_t CCD3D12Framebuffer::getHeight() const {
    return _impl ? _impl->height : 0;
}

uint32_t CCD3D12Framebuffer::getColorTextureCount() const {
    return _impl ? static_cast<uint32_t>(_impl->colorAttachments.size()) : 0;
}

const D3D12FramebufferAttachment *CCD3D12Framebuffer::getColorAttachment(uint32_t index) const {
    if (!_impl || index >= _impl->colorAttachments.size()) return nullptr;
    return &_impl->colorAttachments[index];
}

const D3D12FramebufferAttachment *CCD3D12Framebuffer::getDepthStencilAttachment() const {
    return _impl && _impl->hasDepthStencil ? &_impl->depthStencilAttachment : nullptr;
}

void *CCD3D12Framebuffer::getColorResource(uint32_t index) const {
    const auto backing = getColorBacking(index);
    return backing ? backing->resource.Get() : nullptr;
}

void *CCD3D12Framebuffer::getDepthStencilResource() const {
    const auto backing = getDepthStencilBacking();
    return backing ? backing->resource.Get() : nullptr;
}

D3D12ResourceBackingPtr CCD3D12Framebuffer::getColorBacking(uint32_t index) const {
    if (!_impl || index >= _impl->colorAttachments.size()) return {};
    auto *device = CCD3D12Device::getInstance();
    if (!device || _impl->deviceEpoch != device->getDeviceEpoch()) return {};
    if (_impl->colorAttachments[index].isSwapchain) {
        const auto backing = _swapchain
                                 ? _swapchain->getCurrentBackBufferBacking()
                                 : D3D12ResourceBackingPtr{};
        return backing && backing->valid ? backing : D3D12ResourceBackingPtr{};
    }
    const auto &attachment = _impl->colorAttachments[index];
    return attachment.backing && attachment.backing->valid &&
                   attachment.backing->generation == attachment.backingGeneration
               ? attachment.backing
               : D3D12ResourceBackingPtr{};
}

D3D12ResourceBackingPtr CCD3D12Framebuffer::getDepthStencilBacking() const {
    if (!_impl) return {};
    auto *device = CCD3D12Device::getInstance();
    if (!device || _impl->deviceEpoch != device->getDeviceEpoch()) return {};
    if (!_impl->hasDepthStencil) {
        return {};
    }
    const auto &attachment = _impl->depthStencilAttachment;
    return attachment.backing && attachment.backing->valid &&
                   attachment.backing->generation == attachment.backingGeneration
               ? attachment.backing
               : D3D12ResourceBackingPtr{};
}

bool CCD3D12Framebuffer::hasColorTextureState(uint32_t index) const {
    return static_cast<bool>(getColorBacking(index));
}

bool CCD3D12Framebuffer::isValid() const {
    if (!_impl || !_impl->valid) return false;
    auto *device = CCD3D12Device::getInstance();
    if (!device || _impl->deviceEpoch != device->getDeviceEpoch()) return false;
    for (uint32_t i = 0; i < _impl->colorAttachments.size(); ++i) {
        const auto backing = getColorBacking(i);
        if (!backing || !backing->resource || backing->deviceEpoch != _impl->deviceEpoch) {
            return false;
        }
    }
    if (_impl->hasDepthStencil) {
        const auto backing = getDepthStencilBacking();
        if (!backing || !backing->resource || backing->deviceEpoch != _impl->deviceEpoch) {
            return false;
        }
    }
    return true;
}

CCD3D12Swapchain *CCD3D12Framebuffer::getSwapchain() const {
    return _isOffscreen ? nullptr : _swapchain;
}

bool CCD3D12Framebuffer::isOffscreen() const {
    return _isOffscreen;
}

} // namespace gfx
} // namespace cc
