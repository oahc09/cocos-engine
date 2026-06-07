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

#include "D3D12RenderPass.h"
#include "base/Log.h"
#include "gfx-base/GFXDef.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <dxgiformat.h>

namespace cc {
namespace gfx {

namespace {
uint32_t toDXGIFormatUint32(Format format) {
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    switch (format) {
        case Format::A8:             dxgi = DXGI_FORMAT_A8_UNORM; break;
        case Format::L8:             dxgi = DXGI_FORMAT_R8_UNORM; break;
        case Format::R8:             dxgi = DXGI_FORMAT_R8_UNORM; break;
        case Format::R8SN:           dxgi = DXGI_FORMAT_R8_SNORM; break;
        case Format::R8UI:           dxgi = DXGI_FORMAT_R8_UINT; break;
        case Format::R8I:            dxgi = DXGI_FORMAT_R8_SINT; break;
        case Format::R16F:           dxgi = DXGI_FORMAT_R16_FLOAT; break;
        case Format::R16UI:          dxgi = DXGI_FORMAT_R16_UINT; break;
        case Format::R16I:           dxgi = DXGI_FORMAT_R16_SINT; break;
        case Format::R32F:           dxgi = DXGI_FORMAT_R32_FLOAT; break;
        case Format::R32UI:          dxgi = DXGI_FORMAT_R32_UINT; break;
        case Format::R32I:           dxgi = DXGI_FORMAT_R32_SINT; break;
        case Format::RG8:            dxgi = DXGI_FORMAT_R8G8_UNORM; break;
        case Format::RG8SN:          dxgi = DXGI_FORMAT_R8G8_SNORM; break;
        case Format::RG8UI:          dxgi = DXGI_FORMAT_R8G8_UINT; break;
        case Format::RG8I:           dxgi = DXGI_FORMAT_R8G8_SINT; break;
        case Format::RG16F:          dxgi = DXGI_FORMAT_R16G16_FLOAT; break;
        case Format::RG16UI:         dxgi = DXGI_FORMAT_R16G16_UINT; break;
        case Format::RG16I:          dxgi = DXGI_FORMAT_R16G16_SINT; break;
        case Format::RG32F:          dxgi = DXGI_FORMAT_R32G32_FLOAT; break;
        case Format::RG32UI:         dxgi = DXGI_FORMAT_R32G32_UINT; break;
        case Format::RG32I:          dxgi = DXGI_FORMAT_R32G32_SINT; break;
        case Format::RGB8:           dxgi = DXGI_FORMAT_R8G8B8A8_UNORM; break; // padded
        case Format::RGB8SN:         dxgi = DXGI_FORMAT_R8G8B8A8_SNORM; break;
        case Format::RGB8UI:         dxgi = DXGI_FORMAT_R8G8B8A8_UINT; break;
        case Format::RGB8I:          dxgi = DXGI_FORMAT_R8G8B8A8_SINT; break;
        case Format::RGB16F:         dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case Format::RGB16UI:        dxgi = DXGI_FORMAT_R16G16B16A16_UINT; break;
        case Format::RGB16I:         dxgi = DXGI_FORMAT_R16G16B16A16_SINT; break;
        case Format::RGB32F:         dxgi = DXGI_FORMAT_R32G32B32_FLOAT; break;
        case Format::RGB32UI:        dxgi = DXGI_FORMAT_R32G32B32_UINT; break;
        case Format::RGB32I:         dxgi = DXGI_FORMAT_R32G32B32_SINT; break;
        case Format::RGBA8:          dxgi = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case Format::BGRA8:          dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case Format::SRGB8_A8:       dxgi = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
        case Format::RGBA8SN:        dxgi = DXGI_FORMAT_R8G8B8A8_SNORM; break;
        case Format::RGBA8UI:        dxgi = DXGI_FORMAT_R8G8B8A8_UINT; break;
        case Format::RGBA8I:         dxgi = DXGI_FORMAT_R8G8B8A8_SINT; break;
        case Format::RGBA16F:        dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case Format::RGBA16UI:       dxgi = DXGI_FORMAT_R16G16B16A16_UINT; break;
        case Format::RGBA16I:        dxgi = DXGI_FORMAT_R16G16B16A16_SINT; break;
        case Format::RGBA32F:        dxgi = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        case Format::RGBA32UI:       dxgi = DXGI_FORMAT_R32G32B32A32_UINT; break;
        case Format::RGBA32I:        dxgi = DXGI_FORMAT_R32G32B32A32_SINT; break;
        case Format::R5G6B5:         dxgi = DXGI_FORMAT_B5G6R5_UNORM; break;
        case Format::R11G11B10F:     dxgi = DXGI_FORMAT_R11G11B10_FLOAT; break;
        case Format::RGB5A1:         dxgi = DXGI_FORMAT_B5G5R5A1_UNORM; break;
        case Format::RGBA4:          dxgi = DXGI_FORMAT_B4G4R4A4_UNORM; break;
        case Format::RGB10A2:        dxgi = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        case Format::RGB10A2UI:      dxgi = DXGI_FORMAT_R10G10B10A2_UINT; break;
        case Format::RGB9E5:         dxgi = DXGI_FORMAT_R9G9B9E5_SHAREDEXP; break;
        case Format::DEPTH:          dxgi = DXGI_FORMAT_D32_FLOAT; break;
        case Format::DEPTH_STENCIL:  dxgi = DXGI_FORMAT_D24_UNORM_S8_UINT; break;
        case Format::BC1:            dxgi = DXGI_FORMAT_BC1_UNORM; break;
        case Format::BC1_ALPHA:      dxgi = DXGI_FORMAT_BC1_UNORM; break;
        case Format::BC1_SRGB:       dxgi = DXGI_FORMAT_BC1_UNORM_SRGB; break;
        case Format::BC1_SRGB_ALPHA: dxgi = DXGI_FORMAT_BC1_UNORM_SRGB; break;
        case Format::BC2:            dxgi = DXGI_FORMAT_BC2_UNORM; break;
        case Format::BC2_SRGB:       dxgi = DXGI_FORMAT_BC2_UNORM_SRGB; break;
        case Format::BC3:            dxgi = DXGI_FORMAT_BC3_UNORM; break;
        case Format::BC3_SRGB:       dxgi = DXGI_FORMAT_BC3_UNORM_SRGB; break;
        case Format::BC4:            dxgi = DXGI_FORMAT_BC4_UNORM; break;
        case Format::BC4_SNORM:      dxgi = DXGI_FORMAT_BC4_SNORM; break;
        case Format::BC5:            dxgi = DXGI_FORMAT_BC5_UNORM; break;
        case Format::BC5_SNORM:      dxgi = DXGI_FORMAT_BC5_SNORM; break;
        case Format::BC6H_UF16:      dxgi = DXGI_FORMAT_BC6H_UF16; break;
        case Format::BC6H_SF16:      dxgi = DXGI_FORMAT_BC6H_SF16; break;
        case Format::BC7:            dxgi = DXGI_FORMAT_BC7_UNORM; break;
        case Format::BC7_SRGB:       dxgi = DXGI_FORMAT_BC7_UNORM_SRGB; break;
        default:                     dxgi = DXGI_FORMAT_UNKNOWN; break;
    }
    return static_cast<uint32_t>(dxgi);
}
} // namespace

struct CCD3D12RenderPass::Impl {
    // Store DXGI_FORMAT values as uint32_t to avoid including dxgiformat.h in header
    ccstd::vector<uint32_t> rtvFormats;
    uint32_t dsvFormat{0}; // 0 == DXGI_FORMAT_UNKNOWN
    uint32_t colorAttachmentCount{0};
    uint32_t sampleCount{1};
};

CCD3D12RenderPass::CCD3D12RenderPass() {
    _impl = std::make_unique<Impl>();
}

CCD3D12RenderPass::~CCD3D12RenderPass() {
    destroy();
}

void CCD3D12RenderPass::doInit(const RenderPassInfo &info) {
    (void)info;
    if (!_impl) return;

    // Process color attachments — use base class _colorAttachments which is already populated
    _impl->rtvFormats.clear();
    for (const auto &attachment : _colorAttachments) {
        uint32_t dxgiFmt = toDXGIFormatUint32(attachment.format);
        if (dxgiFmt == 0 && attachment.format != Format::UNKNOWN) {
            // 0 is DXGI_FORMAT_UNKNOWN
            CC_LOG_WARNING("D3D12RenderPass: unsupported color attachment format %u",
                           static_cast<unsigned>(attachment.format));
        }
        _impl->rtvFormats.push_back(dxgiFmt);
    }
    _impl->colorAttachmentCount = static_cast<uint32_t>(_impl->rtvFormats.size());

    // Process depth-stencil attachment
    if (_depthStencilAttachment.format != Format::UNKNOWN) {
        _impl->dsvFormat = toDXGIFormatUint32(_depthStencilAttachment.format);
        if (_impl->dsvFormat == 0) {
            CC_LOG_WARNING("D3D12RenderPass: unsupported depth-stencil format %u",
                           static_cast<unsigned>(_depthStencilAttachment.format));
        }
    } else {
        _impl->dsvFormat = 0; // DXGI_FORMAT_UNKNOWN
    }

    // Store sample count from first attachment (or default 1)
    if (!_colorAttachments.empty()) {
        _impl->sampleCount = static_cast<uint32_t>(_colorAttachments[0].sampleCount);
    } else if (_depthStencilAttachment.format != Format::UNKNOWN) {
        _impl->sampleCount = static_cast<uint32_t>(_depthStencilAttachment.sampleCount);
    }

    CC_LOG_INFO("D3D12RenderPass initialized: %u color attachments, DSV format=0x%x",
                _impl->colorAttachmentCount, _impl->dsvFormat);
}

void CCD3D12RenderPass::doDestroy() {
    if (_impl) {
        _impl->rtvFormats.clear();
        _impl->dsvFormat = 0;
        _impl->colorAttachmentCount = 0;
        _impl->sampleCount = 1;
    }
}

const ccstd::vector<uint32_t> &CCD3D12RenderPass::getRTVFormats() const {
    static const ccstd::vector<uint32_t> empty;
    return _impl ? _impl->rtvFormats : empty;
}

ccstd::vector<uint32_t> CCD3D12RenderPass::getRTVFormats(uint32_t subpass) const {
    if (!_impl || _subpasses.empty()) {
        return _impl ? _impl->rtvFormats : ccstd::vector<uint32_t>{};
    }
    if (subpass >= _subpasses.size()) {
        CC_LOG_WARNING("D3D12RenderPass: subpass %u is outside subpass count %zu.",
                       subpass, _subpasses.size());
        return {};
    }

    ccstd::vector<uint32_t> formats;
    const auto &colors = _subpasses[subpass].colors;
    formats.reserve(colors.size());
    for (uint32_t attachment : colors) {
        if (attachment >= _impl->rtvFormats.size()) {
            CC_LOG_WARNING("D3D12RenderPass: color attachment %u is outside attachment count %zu.",
                           attachment, _impl->rtvFormats.size());
            continue;
        }
        formats.emplace_back(_impl->rtvFormats[attachment]);
    }
    return formats;
}

uint32_t CCD3D12RenderPass::getDSVFormat() const {
    return _impl ? _impl->dsvFormat : 0;
}

uint32_t CCD3D12RenderPass::getDSVFormat(uint32_t subpass) const {
    if (!_impl || _subpasses.empty()) {
        return _impl ? _impl->dsvFormat : 0;
    }
    if (subpass >= _subpasses.size() || _subpasses[subpass].depthStencil == INVALID_BINDING) {
        return 0;
    }
    return _impl->dsvFormat;
}

uint32_t CCD3D12RenderPass::getColorAttachmentCount() const {
    return _impl ? _impl->colorAttachmentCount : 0;
}

uint32_t CCD3D12RenderPass::getSampleCount() const {
    return _impl ? _impl->sampleCount : 1;
}

uint32_t CCD3D12RenderPass::getSampleCount(uint32_t subpass) const {
    if (!_impl || _subpasses.empty()) {
        return _impl ? _impl->sampleCount : 1;
    }
    if (subpass >= _subpasses.size()) {
        return 1;
    }

    uint32_t sampleCount = 1;
    const auto &subpassInfo = _subpasses[subpass];
    for (uint32_t attachment : subpassInfo.colors) {
        if (attachment < _colorAttachments.size()) {
            sampleCount = std::max(sampleCount, static_cast<uint32_t>(_colorAttachments[attachment].sampleCount));
        }
    }
    if (subpassInfo.depthStencil != INVALID_BINDING &&
        _depthStencilAttachment.format != Format::UNKNOWN) {
        sampleCount = std::max(sampleCount, static_cast<uint32_t>(_depthStencilAttachment.sampleCount));
    }
    return sampleCount;
}

} // namespace gfx
} // namespace cc
