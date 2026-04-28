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

#include "D3D12Shader.h"
#include "base/Log.h"

// File diagnostic for shader bytecode detection
#include <cstdio>
#include <cstdarg>
namespace {
void shaderDiagLog(const char *fmt, ...) {
    static FILE *s_file = nullptr;
    if (!s_file) {
        s_file = fopen("C:\\temp\\d3d12-render-diag.log", "a");
        if (!s_file) return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(s_file, fmt, args);
    fflush(s_file);
    va_end(args);
}
} // anonymous namespace

namespace cc {
namespace gfx {

struct CCD3D12Shader::Impl {
    // Per-stage bytecode storage (non-owning pointers into ShaderStage::source)
    BytecodeBlob vertexBytecode;
    BytecodeBlob fragmentBytecode;
    BytecodeBlob geometryBytecode;
    BytecodeBlob computeBytecode;
    BytecodeBlob hullBytecode;
    BytecodeBlob domainBytecode;

    // Entry point names (defaulted to standard names)
    ccstd::string vertexEntry{"VS"};
    ccstd::string fragmentEntry{"PS"};
};

CCD3D12Shader::CCD3D12Shader() {
    _impl = std::make_unique<Impl>();
}

CCD3D12Shader::~CCD3D12Shader() {
    destroy();
}

void CCD3D12Shader::doInit(const ShaderInfo &info) {
    // Store stages information and attempt to capture bytecode references.
    // At this stage we do not implement GLSL->DXIL compilation.
    // If the source field already contains compiled bytecode data, store it.
    // Otherwise, mark as "needs compilation" and log a warning.
    for (const auto &stage : _stages) {
        BytecodeBlob *targetBlob = nullptr;
        ccstd::string *entryName = nullptr;

        if (stage.stage == ShaderStageFlagBit::VERTEX) {
            targetBlob = &_impl->vertexBytecode;
            entryName = &_impl->vertexEntry;
        } else if (stage.stage == ShaderStageFlagBit::FRAGMENT) {
            targetBlob = &_impl->fragmentBytecode;
            entryName = &_impl->fragmentEntry;
        } else if (stage.stage == ShaderStageFlagBit::GEOMETRY) {
            targetBlob = &_impl->geometryBytecode;
        } else if (stage.stage == ShaderStageFlagBit::COMPUTE) {
            targetBlob = &_impl->computeBytecode;
        } else if (stage.stage == ShaderStageFlagBit::CONTROL) {
            targetBlob = &_impl->hullBytecode;
        } else if (stage.stage == ShaderStageFlagBit::EVALUATION) {
            targetBlob = &_impl->domainBytecode;
        }

        if (targetBlob) {
            if (!stage.source.empty()) {
                // Check if this is valid D3D12 DXBC/DXIL bytecode.
                // DXBC bytecode starts with the 4-byte magic "DXBC" (0x44,0x58,0x42,0x43).
                // GLSL source and SPIR-V bytecode won't have this signature.
                const bool isDXBC = (stage.source.size() >= 4 &&
                                     stage.source[0] == 0x44 && stage.source[1] == 0x58 &&
                                     stage.source[2] == 0x42 && stage.source[3] == 0x43);

                if (isDXBC) {
                    targetBlob->data = stage.source.data();
                    targetBlob->size = stage.source.size();
                    shaderDiagLog("[SHADER] '%s' stage 0x%x: DXBC bytecode (%zu bytes)\n",
                                  info.name.c_str(), static_cast<unsigned>(stage.stage), stage.source.size());
                } else {
                    // Not valid D3D12 bytecode (likely GLSL source or SPIR-V).
                    // Leave bytecode empty so PSO falls back to built-in HLSL shader.
                    targetBlob->data = nullptr;
                    targetBlob->size = 0;
                    shaderDiagLog("[SHADER] '%s' stage 0x%x: NOT DXBC (%zu bytes, first4: %c%c%c%c) -> fallback\n",
                                  info.name.c_str(), static_cast<unsigned>(stage.stage), stage.source.size(),
                                  stage.source.size() > 0 ? stage.source[0] : '?',
                                  stage.source.size() > 1 ? stage.source[1] : '?',
                                  stage.source.size() > 2 ? stage.source[2] : '?',
                                  stage.source.size() > 3 ? stage.source[3] : '?');
                    CC_LOG_WARNING("D3D12Shader '%s': stage 0x%x source is not DXBC bytecode "
                                   "(%zu bytes, first4: 0x%02x 0x%02x 0x%02x 0x%02x). "
                                   "Using built-in fallback shader.",
                                   info.name.c_str(),
                                   static_cast<unsigned>(stage.stage),
                                   stage.source.size(),
                                   static_cast<unsigned char>(stage.source[0]),
                                   static_cast<unsigned char>(stage.source.size() > 1 ? stage.source[1] : 0),
                                   static_cast<unsigned char>(stage.source.size() > 2 ? stage.source[2] : 0),
                                   static_cast<unsigned char>(stage.source.size() > 3 ? stage.source[3] : 0));
                }
            } else {
                targetBlob->data = nullptr;
                targetBlob->size = 0;
                CC_LOG_WARNING("D3D12Shader '%s': stage 0x%x has no source/bytecode. "
                               "GLSL->DXIL compilation is not yet implemented.",
                               info.name.c_str(),
                               static_cast<unsigned>(stage.stage));
            }
        }
    }

    CC_LOG_INFO("D3D12Shader '%s' initialized with %u stages.",
                info.name.c_str(),
                static_cast<unsigned>(_stages.size()));
}

void CCD3D12Shader::doDestroy() {
    if (_impl) {
        _impl->vertexBytecode = {};
        _impl->fragmentBytecode = {};
        _impl->geometryBytecode = {};
        _impl->computeBytecode = {};
        _impl->hullBytecode = {};
        _impl->domainBytecode = {};
    }
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getVertexBytecode() const {
    return _impl ? _impl->vertexBytecode : BytecodeBlob{};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getFragmentBytecode() const {
    return _impl ? _impl->fragmentBytecode : BytecodeBlob{};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getGeometryBytecode() const {
    return _impl ? _impl->geometryBytecode : BytecodeBlob{};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getComputeBytecode() const {
    return _impl ? _impl->computeBytecode : BytecodeBlob{};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getHullBytecode() const {
    return _impl ? _impl->hullBytecode : BytecodeBlob{};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getDomainBytecode() const {
    return _impl ? _impl->domainBytecode : BytecodeBlob{};
}

const ccstd::string &CCD3D12Shader::getVertexEntry() const {
    static const ccstd::string empty;
    return _impl ? _impl->vertexEntry : empty;
}

const ccstd::string &CCD3D12Shader::getFragmentEntry() const {
    static const ccstd::string empty;
    return _impl ? _impl->fragmentEntry : empty;
}

bool CCD3D12Shader::hasBytecode(ShaderStageFlagBit stage) const {
    if (!_impl) return false;
    auto blob = getStageBytecode(stage);
    return blob.data != nullptr && blob.size > 0;
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getStageBytecode(ShaderStageFlagBit stage) const {
    if (!_impl) return {};
    if (stage == ShaderStageFlagBit::VERTEX) return _impl->vertexBytecode;
    if (stage == ShaderStageFlagBit::FRAGMENT) return _impl->fragmentBytecode;
    if (stage == ShaderStageFlagBit::GEOMETRY) return _impl->geometryBytecode;
    if (stage == ShaderStageFlagBit::COMPUTE) return _impl->computeBytecode;
    if (stage == ShaderStageFlagBit::CONTROL) return _impl->hullBytecode;
    if (stage == ShaderStageFlagBit::EVALUATION) return _impl->domainBytecode;
    return {};
}

} // namespace gfx
} // namespace cc
