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
#include "D3D12DebugOptimization.h"
#include "D3D12Texture.h"
#include "base/Log.h"
#include "gfx-base/GFXDef.h"

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <dxgiformat.h>

namespace cc {
namespace gfx {

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
        const uint32_t dxgiFmt = static_cast<uint32_t>(toD3D12Format(attachment.format));
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
        _impl->dsvFormat = static_cast<uint32_t>(toD3D12Format(_depthStencilAttachment.format));
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

    CC_D3D12_DIAGNOSTIC_LOG("D3D12RenderPass initialized: %u color attachments, DSV format=0x%x",
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
