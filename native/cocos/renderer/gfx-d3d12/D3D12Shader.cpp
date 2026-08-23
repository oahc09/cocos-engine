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
#include "D3D12DebugOptimization.h"
#include "D3D12ShaderCacheScheduler.h"
#include "D3D12ShaderCompileScheduler.h"
#include "D3D12Device.h"
#include "base/Data.h"
#include "base/Log.h"
#include "platform/FileUtils.h"
#include "platform/win32/Utils-win32.h"

// glslang for GLSL -> SPIR-V
#include "glslang/Public/ShaderLang.h"
#include "glslang/SPIRV/GlslangToSpv.h"
#include "glslang/StandAlone/ResourceLimits.h"

// SPIRV-Cross for SPIR-V -> HLSL
#include "spirv_hlsl.hpp"
// spv::ExecutionModel is available through spirv_hlsl.hpp -> spirv_glsl.hpp -> spirv_common.hpp -> spirv.hpp

// D3D12 shader compiler
#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>
#include <wrl/client.h>

namespace cc {
namespace gfx {

namespace {
using D3D12PerfClock = std::chrono::steady_clock;

constexpr uint32_t CC_D3D12_DXBC_CACHE_VERSION = 8;
constexpr uint32_t CC_D3D12_MIGRATABLE_DXBC_CACHE_VERSION = 3;
constexpr uint32_t CC_D3D12_BACKGROUND_SHADER_PRECOMPILE_WORKERS = 1;
constexpr uint32_t CC_D3D12_MAX_CONCURRENT_SHADER_COMPILES =
    CC_D3D12_BACKGROUND_SHADER_PRECOMPILE_WORKERS + 1;
constexpr uint64_t FNV1A64_OFFSET = 14695981039346656037ULL;
constexpr uint64_t FNV1A64_PRIME = 1099511628211ULL;

std::atomic<uint32_t> s_activeShaderPrecompiles{0};
std::atomic<uint32_t> s_peakShaderPrecompiles{0};
std::mutex s_shaderCompilerLimitMutex;
std::condition_variable s_shaderCompilerLimitCondition;
uint32_t s_shaderCompilersInUse{0};

detail::D3D12ShaderCompileScheduler &getD3D12ShaderCompileScheduler() {
    static detail::D3D12ShaderCompileScheduler scheduler(CC_D3D12_BACKGROUND_SHADER_PRECOMPILE_WORKERS);
    return scheduler;
}

detail::D3D12ShaderCachePersistenceQueue &getD3D12ShaderCachePersistenceQueue() {
    static detail::D3D12ShaderCachePersistenceQueue queue(1);
    return queue;
}

uint32_t recordShaderPrecompileStart() {
    const uint32_t active = s_activeShaderPrecompiles.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t peak = s_peakShaderPrecompiles.load(std::memory_order_relaxed);
    while (peak < active &&
           !s_peakShaderPrecompiles.compare_exchange_weak(peak, active, std::memory_order_relaxed)) {
    }
    return active;
}

class ScopedShaderPrecompileCounter final {
public:
    ScopedShaderPrecompileCounter()
    : _active(recordShaderPrecompileStart()) {}

    ~ScopedShaderPrecompileCounter() {
        if (!_finished) {
            s_activeShaderPrecompiles.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    uint32_t active() const {
        return _active;
    }

    uint32_t finish() {
        _finished = true;
        return s_activeShaderPrecompiles.fetch_sub(1, std::memory_order_relaxed) - 1;
    }

private:
    uint32_t _active{0};
    bool _finished{false};
};

class ScopedShaderCompilerPermit final {
public:
    ScopedShaderCompilerPermit() {
        const auto waitStart = D3D12PerfClock::now();
        std::unique_lock<std::mutex> lock(s_shaderCompilerLimitMutex);
        s_shaderCompilerLimitCondition.wait(lock, []() {
            return s_shaderCompilersInUse < CC_D3D12_MAX_CONCURRENT_SHADER_COMPILES;
        });
        ++s_shaderCompilersInUse;
        _waitMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(D3D12PerfClock::now() - waitStart).count());
    }

    ~ScopedShaderCompilerPermit() {
        {
            std::lock_guard<std::mutex> lock(s_shaderCompilerLimitMutex);
            CC_ASSERT(s_shaderCompilersInUse > 0);
            --s_shaderCompilersInUse;
        }
        s_shaderCompilerLimitCondition.notify_one();
    }

    uint64_t waitMs() const { return _waitMs; }

private:
    uint64_t _waitMs{0};
};

uint64_t elapsedMs(D3D12PerfClock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(D3D12PerfClock::now() - start).count());
}

const char *getShaderStageName(ShaderStageFlagBit stage) {
    switch (stage) {
        case ShaderStageFlagBit::VERTEX: return "vertex";
        case ShaderStageFlagBit::FRAGMENT: return "fragment";
        case ShaderStageFlagBit::COMPUTE: return "compute";
        case ShaderStageFlagBit::GEOMETRY: return "geometry";
        case ShaderStageFlagBit::CONTROL: return "hull";
        case ShaderStageFlagBit::EVALUATION: return "domain";
        default: return "unknown";
    }
}

EShLanguage toEShLanguage(ShaderStageFlagBit stage) {
    switch (stage) {
        case ShaderStageFlagBit::VERTEX: return EShLangVertex;
        case ShaderStageFlagBit::FRAGMENT: return EShLangFragment;
        case ShaderStageFlagBit::GEOMETRY: return EShLangGeometry;
        case ShaderStageFlagBit::COMPUTE: return EShLangCompute;
        case ShaderStageFlagBit::CONTROL: return EShLangTessControl;
        case ShaderStageFlagBit::EVALUATION: return EShLangTessEvaluation;
        default: return EShLangVertex;
    }
}

const char *getHLSLProfile(ShaderStageFlagBit stage) {
    // SM 5.1 required for register(bN, spaceS) syntax matching Root Signature RegisterSpace
    switch (stage) {
        case ShaderStageFlagBit::VERTEX: return "vs_5_1";
        case ShaderStageFlagBit::FRAGMENT: return "ps_5_1";
        case ShaderStageFlagBit::COMPUTE: return "cs_5_1";
        case ShaderStageFlagBit::GEOMETRY: return "gs_5_1";
        case ShaderStageFlagBit::CONTROL: return "hs_5_1";
        case ShaderStageFlagBit::EVALUATION: return "ds_5_1";
        default: return "vs_5_1";
    }
}

void appendHashBytes(uint64_t &hash, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= FNV1A64_PRIME;
    }
}

void appendHashString(uint64_t &hash, const ccstd::string &value) {
    appendHashBytes(hash, value.data(), value.size());
    const uint8_t terminator = 0;
    appendHashBytes(hash, &terminator, 1);
}

ccstd::string toHex(uint64_t value) {
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

ccstd::string makeDXBCHash(const std::vector<uint8_t> &dxbc) {
    uint64_t hash = FNV1A64_OFFSET;
    appendHashBytes(hash, dxbc.data(), dxbc.size());
    return toHex(hash);
}

bool inspectDXBCContainer(const std::vector<uint8_t> &dxbc, bool &hasUnstableChunks);

bool makeStableDXBC(const std::vector<uint8_t> &dxbc, std::vector<uint8_t> &stableDXBC) {
    stableDXBC.clear();
    bool hasUnstableChunks = false;
    if (!inspectDXBCContainer(dxbc, hasUnstableChunks)) {
        return false;
    }
    if (!hasUnstableChunks) {
        stableDXBC = dxbc;
        return true;
    }

    ID3DBlob *strippedBlob = nullptr;
    const UINT stripFlags = D3DCOMPILER_STRIP_DEBUG_INFO |
                            D3DCOMPILER_STRIP_TEST_BLOBS |
                            D3DCOMPILER_STRIP_PRIVATE_DATA;
    const HRESULT hr = D3DStripShader(dxbc.data(), dxbc.size(), stripFlags, &strippedBlob);
    if (FAILED(hr) || !strippedBlob) {
        return false;
    }

    stableDXBC.resize(strippedBlob->GetBufferSize());
    memcpy(stableDXBC.data(), strippedBlob->GetBufferPointer(), strippedBlob->GetBufferSize());
    strippedBlob->Release();
    bool strippedHasUnstableChunks = false;
    if (!inspectDXBCContainer(stableDXBC, strippedHasUnstableChunks) || strippedHasUnstableChunks) {
        stableDXBC.clear();
        return false;
    }
    return true;
}

struct DXBCHashReference {
    ccstd::string fullSourceKey;
    ccstd::string rawHash;
    ccstd::string stableHash;
    std::vector<uint8_t> stableDXBC;
};

std::mutex s_dxbcHashReferencesMutex;
std::unordered_map<ccstd::string, DXBCHashReference> s_dxbcHashReferences;
std::atomic<uint64_t> s_dxbcCacheWriteSerial{0};

void recordDXBCHashComparison(const ccstd::string &shaderName,
                              ShaderStageFlagBit stage,
                              const ccstd::string &normalizedKey,
                              const ccstd::string &fullSourceKey,
                              const std::vector<uint8_t> &dxbc,
                              const char *backend,
                              const std::vector<uint8_t> *knownStableDXBC = nullptr) {
    // The hash comparison and the global stableDXBC reference map exist solely
    // to feed CC_D3D12_DIAGNOSTIC_LOG outputs. When diagnostics are compiled
    // out (default), skip the entire body so no shader bytecode is retained,
    // no D3DStripShader call is made, and the global map stays empty.
    if (!CC_D3D12_DIAGNOSTICS_ENABLED) {
        return;
    }

    // Hash comparison is useful only when the diagnostic grouping actually
    // collapses multiple complete source identities.
    if (normalizedKey == fullSourceKey) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_dxbcHashReferencesMutex);
        const auto iter = s_dxbcHashReferences.find(normalizedKey);
        if (iter != s_dxbcHashReferences.end() && iter->second.fullSourceKey == fullSourceKey) {
            return;
        }
    }

    const ccstd::string rawHash = makeDXBCHash(dxbc);
    std::vector<uint8_t> stableDXBC;
    const bool stripped = knownStableDXBC || makeStableDXBC(dxbc, stableDXBC);
    const auto &comparisonDXBC = knownStableDXBC ? *knownStableDXBC : (stripped ? stableDXBC : dxbc);
    const ccstd::string stableHash = makeDXBCHash(comparisonDXBC);

    bool createdReference = false;
    bool stableMatch = false;
    bool rawHashMatch = false;
    bool stableHashMatch = false;
    ccstd::string referenceFullSourceKey;
    ccstd::string referenceRawHash;
    ccstd::string referenceStableHash;
    {
        std::lock_guard<std::mutex> lock(s_dxbcHashReferencesMutex);
        auto iter = s_dxbcHashReferences.find(normalizedKey);
        if (iter == s_dxbcHashReferences.end()) {
            s_dxbcHashReferences.emplace(
                normalizedKey,
                DXBCHashReference{fullSourceKey, rawHash, stableHash, comparisonDXBC});
            createdReference = true;
        } else {
            if (iter->second.fullSourceKey == fullSourceKey) {
                return;
            }
            referenceFullSourceKey = iter->second.fullSourceKey;
            referenceRawHash = iter->second.rawHash;
            referenceStableHash = iter->second.stableHash;
            rawHashMatch = referenceRawHash == rawHash;
            stableHashMatch = referenceStableHash == stableHash;
            stableMatch = iter->second.stableDXBC == comparisonDXBC;
        }
    }

    if (createdReference) {
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderDXBCHashReference name='%s' stage=%s normalizedKey=%s fullSourceKey=%s rawHash=%s stableHash=%s rawBytes=%u stableBytes=%u backend=%s",
                    shaderName.c_str(), getShaderStageName(stage), normalizedKey.c_str(), fullSourceKey.c_str(),
                    rawHash.c_str(), stableHash.c_str(), static_cast<unsigned>(dxbc.size()),
                    static_cast<unsigned>(comparisonDXBC.size()), backend);
        return;
    }

    CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderDXBCHashCompare name='%s' stage=%s normalizedKey=%s referenceFullSourceKey=%s fullSourceKey=%s referenceRawHash=%s rawHash=%s rawHashMatch=%u referenceStableHash=%s stableHash=%s stableHashMatch=%u stableMatch=%u rawBytes=%u stableBytes=%u backend=%s",
                shaderName.c_str(), getShaderStageName(stage), normalizedKey.c_str(),
                referenceFullSourceKey.c_str(), fullSourceKey.c_str(), referenceRawHash.c_str(),
                rawHash.c_str(), rawHashMatch ? 1U : 0U,
                referenceStableHash.c_str(), stableHash.c_str(),
                stableHashMatch ? 1U : 0U, stableMatch ? 1U : 0U,
                static_cast<unsigned>(dxbc.size()),
                static_cast<unsigned>(comparisonDXBC.size()), backend);
}

ccstd::string joinCachePath(const ccstd::string &base, const ccstd::string &child) {
    if (base.empty()) {
        return child;
    }
    const char last = base[base.size() - 1];
    if (last == '/' || last == '\\') {
        return base + child;
    }
    return base + "/" + child;
}

bool hasDXBCMagic(const uint8_t *data, size_t size) {
    return size >= 4 &&
           data[0] == 0x44 &&
           data[1] == 0x58 &&
           data[2] == 0x42 &&
           data[3] == 0x43;
}

uint32_t readDXBCUInt32(const uint8_t *data) {
    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    return value;
}

constexpr uint32_t makeFourCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8U) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16U) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24U);
}

bool inspectDXBCContainer(const std::vector<uint8_t> &dxbc, bool &hasUnstableChunks) {
    hasUnstableChunks = false;
    constexpr size_t DXBC_HEADER_SIZE = 32;
    if (dxbc.size() < DXBC_HEADER_SIZE || !hasDXBCMagic(dxbc.data(), dxbc.size())) {
        return false;
    }

    const uint32_t containerSize = readDXBCUInt32(dxbc.data() + 24);
    const uint32_t chunkCount = readDXBCUInt32(dxbc.data() + 28);
    if (containerSize != dxbc.size() || chunkCount > (dxbc.size() - DXBC_HEADER_SIZE) / sizeof(uint32_t)) {
        return false;
    }

    for (uint32_t i = 0; i < chunkCount; ++i) {
        const uint32_t chunkOffset = readDXBCUInt32(dxbc.data() + DXBC_HEADER_SIZE + i * sizeof(uint32_t));
        if (chunkOffset > dxbc.size() || dxbc.size() - chunkOffset < 8) {
            return false;
        }
        const uint32_t chunkFourCC = readDXBCUInt32(dxbc.data() + chunkOffset);
        const uint32_t chunkSize = readDXBCUInt32(dxbc.data() + chunkOffset + 4);
        if (chunkSize > dxbc.size() - chunkOffset - 8) {
            return false;
        }
        hasUnstableChunks = hasUnstableChunks ||
                            chunkFourCC == makeFourCC('S', 'P', 'D', 'B') ||
                            chunkFourCC == makeFourCC('S', 'D', 'B', 'G') ||
                            chunkFourCC == makeFourCC('I', 'L', 'D', 'B') ||
                            chunkFourCC == makeFourCC('I', 'L', 'D', 'N') ||
                            chunkFourCC == makeFourCC('P', 'R', 'I', 'V');
    }
    return true;
}

bool isIdentifierChar(char ch) {
    const auto value = static_cast<unsigned char>(ch);
    return std::isalnum(value) || ch == '_';
}

bool startsWith(const ccstd::string &value, const char *prefix) {
    const size_t prefixLen = std::strlen(prefix);
    return value.size() >= prefixLen && value.compare(0, prefixLen, prefix) == 0;
}

bool parseDefineName(const ccstd::string &source, size_t lineStart, size_t lineEnd, ccstd::string &name) {
    size_t pos = lineStart;
    while (pos < lineEnd && (source[pos] == ' ' || source[pos] == '\t')) {
        ++pos;
    }
    if (pos >= lineEnd || source[pos] != '#') {
        return false;
    }
    ++pos;
    while (pos < lineEnd && (source[pos] == ' ' || source[pos] == '\t')) {
        ++pos;
    }

    constexpr const char *DEFINE_TOKEN = "define";
    constexpr size_t DEFINE_TOKEN_LEN = 6;
    if (pos + DEFINE_TOKEN_LEN > lineEnd || source.compare(pos, DEFINE_TOKEN_LEN, DEFINE_TOKEN) != 0) {
        return false;
    }
    pos += DEFINE_TOKEN_LEN;
    if (pos < lineEnd && isIdentifierChar(source[pos])) {
        return false;
    }
    while (pos < lineEnd && (source[pos] == ' ' || source[pos] == '\t')) {
        ++pos;
    }
    if (pos >= lineEnd || !(std::isalpha(static_cast<unsigned char>(source[pos])) || source[pos] == '_')) {
        return false;
    }

    const size_t nameStart = pos;
    ++pos;
    while (pos < lineEnd && isIdentifierChar(source[pos])) {
        ++pos;
    }
    name.assign(source.data() + nameStart, pos - nameStart);
    return true;
}

bool isD3D12PrunableVariantDefine(const ccstd::string &name) {
    if (name == "CC_USE_D3D12") {
        return false;
    }
    return startsWith(name, "CC_") || startsWith(name, "USE_");
}

// Diagnostic grouping only. These defines are not universally vertex-neutral
// (for example CC_FORWARD_ADD affects lightmap varyings), so this list must
// never participate in cache identity or in-flight deduplication.
bool isD3D12VertexDiagnosticGroupingDefine(const ccstd::string &name) {
    return name == "CC_FORWARD_ADD" ||
           name == "CC_USE_IBL" ||
           name == "CC_USE_DIFFUSEMAP" ||
           name == "CC_USE_HDR" ||
           name == "CC_USE_DEBUG_VIEW" ||
           name == "CC_TONE_MAPPING_TYPE" ||
           name == "CC_IBL_CONVOLUTED" ||
           name == "CC_SHADOWMAP_FORMAT" ||
           name == "CC_SHADOWMAP_USE_LINEAR_DEPTH" ||
           name == "CC_DIR_SHADOW_PCF_TYPE" ||
           name == "CC_CASCADED_LAYERS_TRANSITION" ||
           name == "CC_SHADOW_TYPE" ||
           name == "CC_DIR_LIGHT_SHADOW_TYPE";
}

bool shouldWarmD3D12ShaderStage(ShaderStageFlagBit stage) {
    switch (stage) {
        case ShaderStageFlagBit::VERTEX:
        case ShaderStageFlagBit::FRAGMENT:
        case ShaderStageFlagBit::GEOMETRY:
        case ShaderStageFlagBit::COMPUTE:
        case ShaderStageFlagBit::CONTROL:
        case ShaderStageFlagBit::EVALUATION:
            return true;
        default:
            return false;
    }
}

bool sourceReferencesIdentifierOutsideLine(const ccstd::string &source,
                                           const ccstd::string &name,
                                           size_t lineStart,
                                           size_t lineEnd) {
    size_t pos = source.find(name);
    while (pos != ccstd::string::npos) {
        const bool outsideCurrentLine = pos < lineStart || pos >= lineEnd;
        const bool leftBoundary = pos == 0 || !isIdentifierChar(source[pos - 1]);
        const size_t nameEnd = pos + name.size();
        const bool rightBoundary = nameEnd >= source.size() || !isIdentifierChar(source[nameEnd]);
        if (outsideCurrentLine && leftBoundary && rightBoundary) {
            return true;
        }
        pos = source.find(name, pos + name.size());
    }
    return false;
}

bool lineHasPreprocessorContinuation(const ccstd::string &source, size_t lineStart, size_t lineEnd) {
    size_t pos = lineEnd;
    while (pos > lineStart) {
        const char ch = source[pos - 1];
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            --pos;
            continue;
        }
        return ch == '\\';
    }
    return false;
}

bool sourceUsesLineSensitiveMacros(const ccstd::string &source) {
    if (source.find("__LINE__") != ccstd::string::npos) {
        return true;
    }
    static const std::regex lineDirective(R"((^|\n)[ \t]*#[ \t]*line\b)");
    return std::regex_search(source.begin(), source.end(), lineDirective);
}

struct D3D12ShaderSourceOptimization {
    ccstd::string source;
    uint32_t removedDefines{0};
    bool changed{false};
};

uint32_t replaceD3D12ShaderSourceAll(ccstd::string &source,
                                     const char *legacyExpression,
                                     const char *d3d12Expression) {
    uint32_t replacements = 0;
    size_t position = 0;
    const size_t legacyLength = std::strlen(legacyExpression);
    const size_t replacementLength = std::strlen(d3d12Expression);
    while ((position = source.find(legacyExpression, position)) != ccstd::string::npos) {
        source.replace(position, legacyLength, d3d12Expression);
        position += replacementLength;
        ++replacements;
    }
    return replacements;
}

struct D3D12LegacyShadowSourceRewrite {
    ccstd::string source;
    uint32_t replacements{0};
};

D3D12LegacyShadowSourceRewrite rewriteD3D12LegacyShadowClipDepth(const ccstd::string &source) {
    D3D12LegacyShadowSourceRewrite result{source};

    // Effects compiled before D3D12 support baked OpenGL's [-1, 1] shadow Z
    // conversion directly into GLSL. D3D12 matrices already produce [0, 1],
    // so preserve the XY viewport transform while leaving Z untouched.
    result.replacements += replaceD3D12ShaderSourceAll(
        result.source,
        "shadowNDCPos = shadowPosWithDepthBias.xyz / shadowPosWithDepthBias.w * 0.5 + 0.5;",
        "shadowNDCPos = shadowPosWithDepthBias.xyz / shadowPosWithDepthBias.w;\n"
        "  shadowNDCPos.xy = shadowNDCPos.xy * 0.5 + 0.5;");
    result.replacements += replaceD3D12ShaderSourceAll(
        result.source,
        "vec3 clipPos = shadowPos.xyz / shadowPos.w * 0.5 + 0.5;",
        "vec3 clipPos = shadowPos.xyz / shadowPos.w;\n"
        "          clipPos.xy = clipPos.xy * 0.5 + 0.5;");
    result.replacements += replaceD3D12ShaderSourceAll(
        result.source,
        "shadowPos.z = CCGetLinearDepth(worldPos, viewspaceDepthBias) * 2.0 - 1.0;",
        "shadowPos.z = CCGetLinearDepth(worldPos, viewspaceDepthBias);");
    result.replacements += replaceD3D12ShaderSourceAll(
        result.source,
        "highp float clipDepth = v_clip_depth.x / v_clip_depth.y * 0.5 + 0.5;",
        "highp float clipDepth = v_clip_depth.x / v_clip_depth.y;");
    result.replacements += replaceD3D12ShaderSourceAll(
        result.source,
        "v_clip_depth = clipPos.z / clipPos.w * 0.5 + 0.5;",
        "v_clip_depth = clipPos.z / clipPos.w;");

    return result;
}

D3D12ShaderSourceOptimization optimizeD3D12ShaderSource(ShaderStageFlagBit stage,
                                                         const ccstd::string &shaderName,
                                                         const ccstd::string &source,
                                                         bool preserveSourcePositions = true) {
    D3D12ShaderSourceOptimization result;
    // Token pasting can synthesize a variant identifier that a textual
    // reference scan cannot see (for example CC_##suffix). Stay conservative.
    if (source.find("##") != ccstd::string::npos) {
        result.source = source;
        if (preserveSourcePositions) {
            CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderSourceOptimizeSkipped name='%s' stage=%s reason=token-paste sourceBytes=%u",
                        shaderName.c_str(), getShaderStageName(stage), static_cast<unsigned>(source.size()));
        }
        return result;
    }
    result.source.reserve(source.size());

    size_t lineStart = 0;
    while (lineStart < source.size()) {
        size_t lineEnd = source.find('\n', lineStart);
        size_t nextLineStart = source.size();
        if (lineEnd == ccstd::string::npos) {
            lineEnd = source.size();
        } else {
            nextLineStart = lineEnd + 1;
        }

        ccstd::string defineName;
        const bool isDefine = parseDefineName(source, lineStart, lineEnd, defineName);
        const bool canDropDefine = isDefine &&
                                   isD3D12PrunableVariantDefine(defineName) &&
                                   !lineHasPreprocessorContinuation(source, lineStart, lineEnd) &&
                                   !sourceReferencesIdentifierOutsideLine(source, defineName, lineStart, lineEnd);
        if (canDropDefine) {
            ++result.removedDefines;
            result.changed = true;
            if (preserveSourcePositions) {
                // Preserve byte positions and line numbering for __LINE__ and debug maps.
                result.source.append(lineEnd - lineStart, ' ');
                if (nextLineStart > lineEnd) {
                    result.source.append(source.data() + lineEnd, nextLineStart - lineEnd);
                }
            }
        } else {
            result.source.append(source.data() + lineStart, nextLineStart - lineStart);
        }

        lineStart = nextLineStart;
    }

    if (!result.changed) {
        result.source = source;
    } else if (preserveSourcePositions) {
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderSourceOptimize name='%s' stage=%s removedDefines=%u originalBytes=%u optimizedBytes=%u",
                    shaderName.c_str(), getShaderStageName(stage),
                    result.removedDefines,
                    static_cast<unsigned>(source.size()),
                    static_cast<unsigned>(result.source.size()));
    }
    return result;
}

ccstd::string makeStageLinkageKey(const CCD3D12Shader::StageLinkageParameter &parameter) {
    ccstd::string key = parameter.semanticName;
    for (char &c : key) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    key += std::to_string(parameter.semanticIndex);
    return key;
}

ccstd::string makeStageLinkageCacheSalt(
    const ccstd::vector<CCD3D12Shader::StageLinkageParameter> &linkage) {
    ccstd::string salt = "\n// CC_D3D12_FRAGMENT_LINKAGE";
    for (const auto &parameter : linkage) {
        salt += " " + makeStageLinkageKey(parameter) + ":" +
                std::to_string(parameter.registerIndex) + ":" +
                std::to_string(parameter.componentMask) + ":" +
                std::to_string(parameter.componentType);
    }
    return salt;
}

ccstd::string makeLinkageDummyType(const CCD3D12Shader::StageLinkageParameter &parameter) {
    uint32_t componentCount = 0;
    for (uint32_t mask = parameter.componentMask; mask != 0; mask >>= 1U) {
        componentCount += mask & 1U;
    }
    componentCount = std::max(1U, componentCount);

    ccstd::string type;
    switch (static_cast<D3D_REGISTER_COMPONENT_TYPE>(parameter.componentType)) {
        case D3D_REGISTER_COMPONENT_SINT32: type = "int"; break;
        case D3D_REGISTER_COMPONENT_UINT32: type = "uint"; break;
        default: type = "float"; break;
    }
    if (componentCount > 1) {
        type += std::to_string(componentCount);
    }
    return type;
}

bool patchD3D12FragmentInputLinkage(
    std::string &hlslSource,
    const ccstd::vector<CCD3D12Shader::StageLinkageParameter> &fragmentLinkage) {
    if (fragmentLinkage.empty()) {
        return false;
    }
    const size_t inputStruct = hlslSource.find("struct SPIRV_Cross_Input");
    if (inputStruct == std::string::npos) {
        return false;
    }
    const size_t bodyBegin = hlslSource.find('{', inputStruct);
    const size_t bodyEnd = bodyBegin == std::string::npos ? std::string::npos : hlslSource.find('}', bodyBegin);
    if (bodyBegin == std::string::npos || bodyEnd == std::string::npos) {
        return false;
    }

    struct InputField {
        std::string declaration;
        ccstd::string linkageKey;
        bool consumed{false};
    };
    std::vector<InputField> fields;
    const std::string inputBody = hlslSource.substr(bodyBegin + 1, bodyEnd - bodyBegin - 1);
    const std::regex semanticRegex(R"(:\s*([A-Za-z_][A-Za-z0-9_]*?)([0-9]+)\s*;)");
    size_t lineStart = 0;
    while (lineStart < inputBody.size()) {
        size_t lineEnd = inputBody.find('\n', lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = inputBody.size();
        } else {
            ++lineEnd;
        }
        std::string declaration = inputBody.substr(lineStart, lineEnd - lineStart);
        std::smatch match;
        ccstd::string linkageKey;
        if (std::regex_search(declaration, match, semanticRegex)) {
            linkageKey = match[1].str();
            for (char &c : linkageKey) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            linkageKey += match[2].str();
        }
        fields.push_back({std::move(declaration), std::move(linkageKey), false});
        lineStart = lineEnd;
    }

    std::string rebuiltBody = "\n";
    uint32_t dummyIndex = 0;
    for (const auto &parameter : fragmentLinkage) {
        const ccstd::string linkageKey = makeStageLinkageKey(parameter);
        auto field = std::find_if(fields.begin(), fields.end(), [&linkageKey](const InputField &candidate) {
            return !candidate.consumed && candidate.linkageKey == linkageKey;
        });
        if (field != fields.end()) {
            rebuiltBody += field->declaration;
            field->consumed = true;
            continue;
        }

        const bool integerInput = parameter.componentType == D3D_REGISTER_COMPONENT_SINT32 ||
                                  parameter.componentType == D3D_REGISTER_COMPONENT_UINT32;
        rebuiltBody += "    ";
        if (integerInput) {
            rebuiltBody += "nointerpolation ";
        }
        rebuiltBody += makeLinkageDummyType(parameter) + " _cc_d3d12_linkage_" +
                       std::to_string(dummyIndex++) + " : " + parameter.semanticName +
                       std::to_string(parameter.semanticIndex) + ";\n";
    }
    for (const auto &field : fields) {
        if (!field.consumed) {
            rebuiltBody += field.declaration;
        }
    }

    hlslSource.replace(bodyBegin + 1, bodyEnd - bodyBegin - 1, rebuiltBody);
    return true;
}

ccstd::string buildD3D12StageDiagnosticGroupingSource(ShaderStageFlagBit stage,
                                                       const ccstd::string &shaderName,
                                                       const ccstd::string &source) {
    if (stage != ShaderStageFlagBit::VERTEX) {
        return source;
    }

    ccstd::string result;
    result.reserve(source.size());
    uint32_t removedDefines = 0;

    size_t lineStart = 0;
    while (lineStart < source.size()) {
        size_t lineEnd = source.find('\n', lineStart);
        size_t nextLineStart = source.size();
        if (lineEnd == ccstd::string::npos) {
            lineEnd = source.size();
        } else {
            nextLineStart = lineEnd + 1;
        }

        ccstd::string defineName;
        const bool dropDefine = parseDefineName(source, lineStart, lineEnd, defineName) &&
                                isD3D12VertexDiagnosticGroupingDefine(defineName);
        if (dropDefine) {
            ++removedDefines;
        } else {
            result.append(source.data() + lineStart, nextLineStart - lineStart);
        }

        lineStart = nextLineStart;
    }

    if (removedDefines > 0) {
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderDiagnosticKeyNormalize name='%s' stage=%s removedDefines=%u sourceBytes=%u keyBytes=%u",
                    shaderName.c_str(), getShaderStageName(stage),
                    removedDefines,
                    static_cast<unsigned>(source.size()),
                    static_cast<unsigned>(result.size()));
        return result;
    }

    return source;
}

ccstd::string buildD3D12ProcessedSource(const ccstd::string &source) {
    ccstd::string processedSource = "#version 450\n";
    if (source.find("#define CC_USE_D3D12") == ccstd::string::npos) {
        processedSource += "#define CC_USE_D3D12 1\n";
    }
    processedSource += source;
    return processedSource;
}

ccstd::string makeDXBCCacheKey(ShaderStageFlagBit stage,
                               const char *profile,
                               const ccstd::string &entryName,
                               UINT compileFlags,
                               const ccstd::string &processedSource,
                               uint32_t cacheVersion = CC_D3D12_DXBC_CACHE_VERSION) {
    uint64_t hashA = FNV1A64_OFFSET;
    uint64_t hashB = FNV1A64_OFFSET ^ 0x9e3779b97f4a7c15ULL;
    const uint32_t stageValue = static_cast<uint32_t>(stage);

    appendHashBytes(hashA, &cacheVersion, sizeof(cacheVersion));
    appendHashBytes(hashA, &stageValue, sizeof(stageValue));
    appendHashString(hashA, profile);
    appendHashString(hashA, entryName);
    appendHashBytes(hashA, &compileFlags, sizeof(compileFlags));
    appendHashString(hashA, processedSource);

    appendHashString(hashB, processedSource);
    appendHashBytes(hashB, &compileFlags, sizeof(compileFlags));
    appendHashString(hashB, entryName);
    appendHashString(hashB, profile);
    appendHashBytes(hashB, &stageValue, sizeof(stageValue));
    appendHashBytes(hashB, &cacheVersion, sizeof(cacheVersion));

    return toHex(hashA) + toHex(hashB);
}

ccstd::string getDXBCCacheDirectory(cc::FileUtils *fileUtils,
                                    uint32_t cacheVersion = CC_D3D12_DXBC_CACHE_VERSION) {
    if (!fileUtils) {
        return "";
    }
    ccstd::string root = fileUtils->getWritablePath();
    if (root.empty()) {
        return "";
    }
    root = joinCachePath(root, "d3d12-dxbc-cache");
    return joinCachePath(root, "v" + std::to_string(cacheVersion));
}

bool loadD3D12ShaderCacheDXBC(const ccstd::string &cacheKey, std::vector<uint8_t> &outDXBC) {
    auto *device = CCD3D12Device::getInstance();
    if (!device || !device->loadShaderCacheValue(cacheKey.data(), static_cast<uint32_t>(cacheKey.size()), outDXBC)) {
        return false;
    }

    if (!hasDXBCMagic(outDXBC.data(), outDXBC.size())) {
        CC_LOG_WARNING("D3D12Shader: ignoring corrupt D3D12 Shader Cache DXBC entry key=%s.", cacheKey.c_str());
        outDXBC.clear();
        return false;
    }
    std::vector<uint8_t> stableDXBC;
    if (!makeStableDXBC(outDXBC, stableDXBC)) {
        CC_LOG_WARNING("D3D12Shader: ignoring invalid D3D12 Shader Cache DXBC entry key=%s.", cacheKey.c_str());
        outDXBC.clear();
        return false;
    }
    outDXBC = std::move(stableDXBC);
    return true;
}

bool storeD3D12ShaderCacheDXBC(const ccstd::string &cacheKey, const std::vector<uint8_t> &dxbc) {
    auto *device = CCD3D12Device::getInstance();
    return device && device->storeShaderCacheValue(cacheKey.data(), static_cast<uint32_t>(cacheKey.size()), dxbc);
}

bool loadFileCachedDXBC(const ccstd::string &cacheKey,
                        std::vector<uint8_t> &outDXBC,
                        uint32_t cacheVersion = CC_D3D12_DXBC_CACHE_VERSION) {
    auto *fileUtils = cc::FileUtils::getInstance();
    const ccstd::string cacheDir = getDXBCCacheDirectory(fileUtils, cacheVersion);
    if (cacheDir.empty()) {
        return false;
    }

    const ccstd::string cachePath = joinCachePath(cacheDir, cacheKey + ".dxbc");
    cc::Data cachedData = fileUtils->getDataFromFile(cachePath);
    if (cachedData.isNull() || cachedData.getSize() == 0) {
        return false;
    }

    const uint8_t *bytes = cachedData.getBytes();
    const uint32_t size = cachedData.getSize();
    if (!hasDXBCMagic(bytes, size)) {
        CC_LOG_WARNING("D3D12Shader: ignoring corrupt DXBC cache entry '%s'.", cachePath.c_str());
        return false;
    }

    std::vector<uint8_t> loadedDXBC(bytes, bytes + size);
    if (!makeStableDXBC(loadedDXBC, outDXBC)) {
        CC_LOG_WARNING("D3D12Shader: ignoring invalid DXBC cache entry '%s'.", cachePath.c_str());
        outDXBC.clear();
        return false;
    }
    return true;
}

bool storeFileCachedDXBC(const ccstd::string &cacheKey, const std::vector<uint8_t> &dxbc) {
    if (dxbc.empty()) {
        return false;
    }

    auto *fileUtils = cc::FileUtils::getInstance();
    const ccstd::string cacheDir = getDXBCCacheDirectory(fileUtils);
    if (cacheDir.empty()) {
        return false;
    }
    if (!fileUtils->createDirectory(cacheDir)) {
        CC_LOG_WARNING("D3D12Shader: failed to create DXBC cache directory '%s'.", cacheDir.c_str());
        return false;
    }

    const ccstd::string cachePath = joinCachePath(cacheDir, cacheKey + ".dxbc");
    const uint64_t writeSerial = s_dxbcCacheWriteSerial.fetch_add(1, std::memory_order_relaxed);
    const ccstd::string tempPath = cachePath + ".tmp-" +
                                   std::to_string(static_cast<uint64_t>(GetCurrentProcessId())) + "-" +
                                   std::to_string(writeSerial);
    cc::Data data;
    data.copy(dxbc.data(), static_cast<uint32_t>(dxbc.size()));
    if (!fileUtils->writeDataToFile(data, tempPath)) {
        fileUtils->removeFile(tempPath);
        CC_LOG_WARNING("D3D12Shader: failed to write temporary DXBC cache entry '%s'.", tempPath.c_str());
        return false;
    }
    const std::wstring wideTempPath = cc::StringUtf8ToWideChar(tempPath);
    const std::wstring wideCachePath = cc::StringUtf8ToWideChar(cachePath);
    if (!MoveFileExW(wideTempPath.c_str(), wideCachePath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        fileUtils->removeFile(tempPath);
        CC_LOG_WARNING("D3D12Shader: failed to atomically commit DXBC cache entry '%s' (error=%lu).",
                       cachePath.c_str(), static_cast<unsigned long>(error));
        return false;
    }
    return true;
}

bool scheduleFileCachedDXBCPersistence(const ccstd::string &cacheKey, const std::vector<uint8_t> &dxbc) {
    if (dxbc.empty()) {
        return false;
    }

    try {
        ccstd::string queuedKey = cacheKey;
        std::vector<uint8_t> queuedDXBC = dxbc;
        const auto queuedAt = D3D12PerfClock::now();
        return getD3D12ShaderCachePersistenceQueue().enqueue(
            [cacheKey = std::move(queuedKey), dxbc = std::move(queuedDXBC), queuedAt]() {
                const auto writeStart = D3D12PerfClock::now();
                const uint64_t queueWaitMs = elapsedMs(queuedAt);
                const bool stored = storeFileCachedDXBC(cacheKey, dxbc);
                CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderCacheFilePersist cacheKey=%s result=%s bytes=%u queueWaitMs=%llu writeMs=%llu",
                            cacheKey.c_str(), stored ? "stored" : "failed",
                            static_cast<unsigned>(dxbc.size()),
                            static_cast<unsigned long long>(queueWaitMs),
                            static_cast<unsigned long long>(elapsedMs(writeStart)));
            });
    } catch (...) {
        return false;
    }
}

const char *storeCachedDXBC(const ccstd::string &cacheKey, const std::vector<uint8_t> &dxbc) {
    std::vector<uint8_t> stableDXBC;
    if (!makeStableDXBC(dxbc, stableDXBC)) {
        CC_LOG_WARNING("D3D12Shader: refusing to cache invalid or non-canonical DXBC key=%s.", cacheKey.c_str());
        return "invalid";
    }
    const auto &cacheDXBC = stableDXBC;
    const bool storedSession = storeD3D12ShaderCacheDXBC(cacheKey, cacheDXBC);
    const bool storedFile = storeFileCachedDXBC(cacheKey, cacheDXBC);
    if (stableDXBC.size() != dxbc.size()) {
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderCacheCanonicalize key=%s rawBytes=%u stableBytes=%u",
                    cacheKey.c_str(), static_cast<unsigned>(dxbc.size()),
                    static_cast<unsigned>(stableDXBC.size()));
    }
    if (storedSession && storedFile) {
        return "d3d12-session+file";
    }
    if (storedSession) {
        return "d3d12-session";
    }
    if (storedFile) {
        return "file";
    }
    return "none";
}

struct InFlightDXBCCompile {
    std::mutex mutex;
    std::condition_variable completedCondition;
    bool completed{false};
    bool ok{false};
    std::vector<uint8_t> dxbc;
};

struct InFlightDXBCCompileTicket {
    std::shared_ptr<InFlightDXBCCompile> compile;
    bool owner{false};
};

std::mutex s_inFlightDXBCMutex;
std::unordered_map<ccstd::string, std::shared_ptr<InFlightDXBCCompile>> s_inFlightDXBCCompiles;

// Upper bound for waiting on another thread's in-flight DXBC compile. A
// healthy compile finishes well below this; exceeding it means the owner
// stalled or died before finishing, and the waiter falls back to compiling
// locally instead of hanging forever.
constexpr std::chrono::seconds D3D12_INFLIGHT_DXBC_COMPILE_WAIT_TIMEOUT{10};

enum class InFlightDXBCWaitResult {
    COMPLETED_OK,
    COMPLETED_FAILED,
    TIMED_OUT,
};

InFlightDXBCCompileTicket beginInFlightDXBCCompile(const ccstd::string &cacheKey) {
    std::lock_guard<std::mutex> lock(s_inFlightDXBCMutex);
    auto iter = s_inFlightDXBCCompiles.find(cacheKey);
    if (iter != s_inFlightDXBCCompiles.end()) {
        return {iter->second, false};
    }

    auto compile = std::make_shared<InFlightDXBCCompile>();
    s_inFlightDXBCCompiles.emplace(cacheKey, compile);
    return {compile, true};
}

InFlightDXBCWaitResult waitForInFlightDXBCCompile(const InFlightDXBCCompileTicket &ticket, std::vector<uint8_t> &outDXBC) {
    if (!ticket.compile) {
        return InFlightDXBCWaitResult::COMPLETED_FAILED;
    }

    std::unique_lock<std::mutex> lock(ticket.compile->mutex);
    const bool completed = ticket.compile->completedCondition.wait_for(
        lock, D3D12_INFLIGHT_DXBC_COMPILE_WAIT_TIMEOUT, [&ticket]() {
            return ticket.compile->completed;
        });
    if (!completed) {
        return InFlightDXBCWaitResult::TIMED_OUT;
    }
    if (ticket.compile->ok) {
        outDXBC = ticket.compile->dxbc;
    } else {
        outDXBC.clear();
    }
    return ticket.compile->ok ? InFlightDXBCWaitResult::COMPLETED_OK
                              : InFlightDXBCWaitResult::COMPLETED_FAILED;
}

InFlightDXBCCompileTicket replaceInFlightDXBCCompile(const ccstd::string &cacheKey) {
    // Take over a stalled in-flight record. The previous owner's finish()
    // only erases pointer-equal entries, so its late completion cannot
    // remove this replacement.
    std::lock_guard<std::mutex> lock(s_inFlightDXBCMutex);
    auto compile = std::make_shared<InFlightDXBCCompile>();
    s_inFlightDXBCCompiles[cacheKey] = compile;
    return {compile, true};
}

bool finishInFlightDXBCCompile(const ccstd::string &cacheKey,
                               const InFlightDXBCCompileTicket &ticket,
                               bool ok,
                               const std::vector<uint8_t> &dxbc) {
    if (ticket.compile) {
        {
            std::lock_guard<std::mutex> lock(ticket.compile->mutex);
            ticket.compile->ok = false;
            ticket.compile->dxbc.clear();
            if (ok) {
                try {
                    ticket.compile->dxbc = dxbc;
                    ticket.compile->ok = true;
                } catch (...) {
                    ticket.compile->dxbc.clear();
                }
            }
            ticket.compile->completed = true;
            ok = ticket.compile->ok;
        }
        ticket.compile->completedCondition.notify_all();
    }

    std::lock_guard<std::mutex> lock(s_inFlightDXBCMutex);
    auto iter = s_inFlightDXBCCompiles.find(cacheKey);
    if (iter != s_inFlightDXBCCompiles.end() && iter->second == ticket.compile) {
        s_inFlightDXBCCompiles.erase(iter);
    }
    return ok;
}

class InFlightDXBCCompletion final {
public:
    InFlightDXBCCompletion(const ccstd::string &cacheKey,
                           const InFlightDXBCCompileTicket &ticket,
                           std::vector<uint8_t> &dxbc)
    : _cacheKey(cacheKey), _ticket(ticket), _dxbc(dxbc) {}

    ~InFlightDXBCCompletion() {
        if (!_finished) {
            finishInFlightDXBCCompile(_cacheKey, _ticket, false, _dxbc);
        }
    }

    bool finish(bool ok) {
        _finished = true;
        return finishInFlightDXBCCompile(_cacheKey, _ticket, ok, _dxbc);
    }

private:
    const ccstd::string &_cacheKey;
    const InFlightDXBCCompileTicket &_ticket;
    std::vector<uint8_t> &_dxbc;
    bool _finished{false};
};

void appendUniqueCandidate(ccstd::vector<ccstd::string> &candidates, const ccstd::string &candidate) {
    if (candidate.empty()) {
        return;
    }
    for (const auto &existing : candidates) {
        if (existing == candidate) {
            return;
        }
    }
    candidates.emplace_back(candidate);
}

ccstd::vector<ccstd::string> collectHLSLEntryCandidates(const ccstd::string &hlslSource, const ccstd::string &preferredEntry) {
    ccstd::vector<ccstd::string> candidates;
    appendUniqueCandidate(candidates, "main");
    appendUniqueCandidate(candidates, preferredEntry);

    const std::regex outputRegex(R"(\bSPIRV_Cross_Output\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()");
    const std::regex voidRegex(R"(\bvoid\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()");
    const std::regex float4Regex(R"(\bfloat4\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()");

    auto collect = [&](const std::regex &pattern) {
        for (std::sregex_iterator it(hlslSource.begin(), hlslSource.end(), pattern), end; it != end; ++it) {
            appendUniqueCandidate(candidates, (*it)[1].str());
        }
    };

    collect(outputRegex);
    collect(voidRegex);
    collect(float4Regex);
    return candidates;
}

ccstd::vector<ccstd::string> getPrimaryHLSLEntryCandidates(const ccstd::string &preferredEntry) {
    ccstd::vector<ccstd::string> candidates;
    appendUniqueCandidate(candidates, "main");
    appendUniqueCandidate(candidates, preferredEntry);
    return candidates;
}

ccstd::string getSPIRVCrossEntryName(ShaderStageFlagBit stage) {
    // SPIRV-Cross HLSL backend uses stage-prefixed _main names
    switch (stage) {
        case ShaderStageFlagBit::VERTEX: return "vert_main";
        case ShaderStageFlagBit::FRAGMENT: return "frag_main";
        case ShaderStageFlagBit::COMPUTE: return "comp_main";
        case ShaderStageFlagBit::GEOMETRY: return "geom_main";
        case ShaderStageFlagBit::CONTROL: return "hull_main";
        case ShaderStageFlagBit::EVALUATION: return "domain_main";
        default: return "main";
    }
}

// Map engine ShaderStageFlagBit to SPIRV-Cross spv::ExecutionModel.
// This is CRITICAL for HLSLResourceBinding — the stage field must match
// the execution model that SPIRV-Cross uses internally to look up bindings
// in remap_hlsl_resource_binding(). If left at the default (ExecutionModelMax),
// the binding override won't be found and HLSL will use wrong register spaces.
spv::ExecutionModel toSPIRVExecutionModel(ShaderStageFlagBit stage) {
    switch (stage) {
        case ShaderStageFlagBit::VERTEX:   return spv::ExecutionModelVertex;
        case ShaderStageFlagBit::FRAGMENT: return spv::ExecutionModelFragment;
        case ShaderStageFlagBit::COMPUTE:  return spv::ExecutionModelGLCompute;
        case ShaderStageFlagBit::GEOMETRY: return spv::ExecutionModelGeometry;
        case ShaderStageFlagBit::CONTROL:  return spv::ExecutionModelTessellationControl;
        case ShaderStageFlagBit::EVALUATION: return spv::ExecutionModelTessellationEvaluation;
        default: return spv::ExecutionModelMax;
    }
}

// Ensure glslang process-level init happens exactly once.
std::once_flag s_glslangInitOnce;
void ensureGlslangInit() {
    std::call_once(s_glslangInitOnce, []() {
        glslang::InitializeProcess();
    });
}

bool isSystemValueSemantic(const char *semanticName) {
    return semanticName && _strnicmp(semanticName, "SV_", 3) == 0;
}

bool reflectStageLinkage(const void *bytecode,
                         size_t bytecodeSize,
                         bool output,
                         ccstd::vector<CCD3D12Shader::StageLinkageParameter> &linkage) {
    linkage.clear();
    if (!bytecode || bytecodeSize == 0) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection;
    if (FAILED(D3DReflect(bytecode, bytecodeSize, IID_PPV_ARGS(&reflection))) || !reflection) {
        return false;
    }
    D3D12_SHADER_DESC shaderDesc{};
    if (FAILED(reflection->GetDesc(&shaderDesc))) {
        return false;
    }
    const UINT parameterCount = output ? shaderDesc.OutputParameters : shaderDesc.InputParameters;
    linkage.reserve(parameterCount);
    for (UINT i = 0; i < parameterCount; ++i) {
        D3D12_SIGNATURE_PARAMETER_DESC parameter{};
        const HRESULT hr = output ? reflection->GetOutputParameterDesc(i, &parameter)
                                  : reflection->GetInputParameterDesc(i, &parameter);
        if (FAILED(hr) || isSystemValueSemantic(parameter.SemanticName)) {
            continue;
        }
        linkage.push_back({
            parameter.SemanticName ? parameter.SemanticName : "",
            parameter.SemanticIndex,
            parameter.Register,
            parameter.Mask,
            static_cast<uint32_t>(parameter.ComponentType),
        });
    }
    return true;
}

bool stageLinkageIsCompatible(
    const ccstd::vector<CCD3D12Shader::StageLinkageParameter> &vertexOutputs,
    const ccstd::vector<CCD3D12Shader::StageLinkageParameter> &fragmentInputs) {
    for (const auto &fragmentInput : fragmentInputs) {
        const ccstd::string fragmentKey = makeStageLinkageKey(fragmentInput);
        const auto vertexOutput = std::find_if(
            vertexOutputs.begin(), vertexOutputs.end(), [&fragmentKey](const auto &candidate) {
                return makeStageLinkageKey(candidate) == fragmentKey;
            });
        if (vertexOutput == vertexOutputs.end() ||
            vertexOutput->registerIndex != fragmentInput.registerIndex ||
            vertexOutput->componentMask != fragmentInput.componentMask ||
            vertexOutput->componentType != fragmentInput.componentType) {
            return false;
        }
    }
    return true;
}

void reflectVertexInputSignature(const std::vector<uint8_t> &dxbc,
                                 std::vector<CCD3D12Shader::VertexInputSignature> &signature) {
    signature.clear();
    if (dxbc.empty()) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection;
    HRESULT hr = D3DReflect(dxbc.data(), dxbc.size(), IID_PPV_ARGS(&reflection));
    if (FAILED(hr) || !reflection) {
        CC_LOG_WARNING("D3D12Shader: D3DReflect failed for vertex shader. HRESULT=0x%08x",
                       static_cast<unsigned>(hr));
        return;
    }

    D3D12_SHADER_DESC shaderDesc{};
    hr = reflection->GetDesc(&shaderDesc);
    if (FAILED(hr)) {
        CC_LOG_WARNING("D3D12Shader: vertex shader reflection GetDesc failed. HRESULT=0x%08x",
                       static_cast<unsigned>(hr));
        return;
    }

    signature.reserve(shaderDesc.InputParameters);
    for (UINT i = 0; i < shaderDesc.InputParameters; ++i) {
        D3D12_SIGNATURE_PARAMETER_DESC paramDesc{};
        hr = reflection->GetInputParameterDesc(i, &paramDesc);
        if (FAILED(hr) || isSystemValueSemantic(paramDesc.SemanticName)) {
            continue;
        }
        signature.push_back({
            paramDesc.SemanticName ? paramDesc.SemanticName : "",
            paramDesc.SemanticIndex,
        });
    }
}

} // anonymous namespace

void reopenD3D12ShaderCachePersistence() {
    getD3D12ShaderCachePersistenceQueue().reopen();
}

void drainD3D12ShaderCachePersistence() {
    const auto drainStart = D3D12PerfClock::now();
    getD3D12ShaderCachePersistenceQueue().closeAndDrain();
    CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderCacheFilePersistDrain waitMs=%llu",
                static_cast<unsigned long long>(elapsedMs(drainStart)));
}

struct CCD3D12Shader::Impl {
    struct AsyncCompileResult {
        std::vector<uint8_t> dxbc;
        bool ok{false};
        bool cacheMiss{false};
    };

    struct StageRecord {
        ccstd::string source;
        ccstd::string entry;
        ccstd::string shaderName;
        detail::D3D12ShaderCompileScheduler::Handle precompileTask;
        std::shared_ptr<AsyncCompileResult> asyncResult;
        bool failed{false};
        bool precompileStarted{false};
        bool foregroundCompileRunning{false};
    };

    // Per-stage bytecode storage (self-owned)
    std::vector<uint8_t> vertexDXBC;
    std::vector<uint8_t> fragmentDXBC;
    std::vector<uint8_t> linkedFragmentDXBC;
    ccstd::string linkedFragmentKey;
    std::vector<uint8_t> geometryDXBC;
    std::vector<uint8_t> computeDXBC;
    std::vector<uint8_t> hullDXBC;
    std::vector<uint8_t> domainDXBC;

    // Entry point names (SPIRV-Cross HLSL convention)
    ccstd::string vertexEntry{"vert_main"};
    ccstd::string fragmentEntry{"frag_main"};

    std::vector<VertexInputSignature> vertexInputSignature;
    StageRecord vertexSource;
    StageRecord fragmentSource;
    StageRecord geometrySource;
    StageRecord computeSource;
    StageRecord hullSource;
    StageRecord domainSource;
    std::mutex compileMutex;
    std::condition_variable compileCondition;
    uint64_t generation{0};
    uint32_t activeStageRequests{0};
    bool acceptingStageRequests{false};

    struct StageRequestLease final {
        StageRequestLease() = default;
        StageRequestLease(const StageRequestLease &) = delete;
        StageRequestLease &operator=(const StageRequestLease &) = delete;

        ~StageRequestLease() {
            if (!owner) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(owner->compileMutex);
                CC_ASSERT(owner->activeStageRequests > 0);
                --owner->activeStageRequests;
            }
            owner->compileCondition.notify_all();
        }

        void arm(Impl *ownerIn) {
            owner = ownerIn;
        }

        Impl *owner{nullptr};
    };

    struct ForegroundCompileLease final {
        ForegroundCompileLease() = default;
        ForegroundCompileLease(const ForegroundCompileLease &) = delete;
        ForegroundCompileLease &operator=(const ForegroundCompileLease &) = delete;

        ~ForegroundCompileLease() {
            if (!owner) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(owner->compileMutex);
                if (owner->generation == generation) {
                    stageRecord->foregroundCompileRunning = false;
                }
            }
            owner->compileCondition.notify_all();
        }

        void arm(Impl *ownerIn, StageRecord *stageRecordIn, uint64_t generationIn) {
            owner = ownerIn;
            stageRecord = stageRecordIn;
            generation = generationIn;
        }

        Impl *owner{nullptr};
        StageRecord *stageRecord{nullptr};
        uint64_t generation{0};
    };

    void stopAcceptingStageRequests() {
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            acceptingStageRequests = false;
            ++generation;
        }
        compileCondition.notify_all();
    }

    void waitForActiveStageRequests() {
        std::unique_lock<std::mutex> lock(compileMutex);
        compileCondition.wait(lock, [this]() {
            return activeStageRequests == 0;
        });
    }

    void startAcceptingStageRequests() {
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            acceptingStageRequests = true;
        }
        compileCondition.notify_all();
    }

    std::vector<uint8_t> *getDXBCBuffer(ShaderStageFlagBit stage) {
        switch (stage) {
            case ShaderStageFlagBit::VERTEX: return &vertexDXBC;
            case ShaderStageFlagBit::FRAGMENT: return &fragmentDXBC;
            case ShaderStageFlagBit::GEOMETRY: return &geometryDXBC;
            case ShaderStageFlagBit::COMPUTE: return &computeDXBC;
            case ShaderStageFlagBit::CONTROL: return &hullDXBC;
            case ShaderStageFlagBit::EVALUATION: return &domainDXBC;
            default: return nullptr;
        }
    }

    StageRecord *getStageRecord(ShaderStageFlagBit stage) {
        switch (stage) {
            case ShaderStageFlagBit::VERTEX: return &vertexSource;
            case ShaderStageFlagBit::FRAGMENT: return &fragmentSource;
            case ShaderStageFlagBit::GEOMETRY: return &geometrySource;
            case ShaderStageFlagBit::COMPUTE: return &computeSource;
            case ShaderStageFlagBit::CONTROL: return &hullSource;
            case ShaderStageFlagBit::EVALUATION: return &domainSource;
            default: return nullptr;
        }
    }
};

CCD3D12Shader::CCD3D12Shader() {
    _impl = std::make_unique<Impl>();
}

CCD3D12Shader::~CCD3D12Shader() {
    destroy();
}

bool CCD3D12Shader::compileGLSLToDXBC(ShaderStageFlagBit stage,
                                       const ccstd::string &glslSource,
                                       const ccstd::string &entryName,
                                       const ccstd::string &shaderName,
                                       std::vector<uint8_t> &outDXBC,
                                       bool cacheOnly,
                                       bool *cacheMiss,
                                       const ccstd::vector<StageLinkageParameter> *fragmentLinkage) {
    const auto compileStart = D3D12PerfClock::now();
    if (cacheMiss) {
        *cacheMiss = false;
    }
    uint64_t glslToSpirvMs = 0;
    uint64_t spirvToHlslMs = 0;
    uint64_t hlslToDxbcMs = 0;
    const char *profile = getHLSLProfile(stage);
    UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if !defined(NDEBUG)
    // Keep symbols for source-level diagnostics, but retain /O3: the Debug
    // executable is also the engine's performance-validation configuration.
    compileFlags |= D3DCOMPILE_DEBUG;
#endif

    // ============================================================
    // Step 0: Prepend #version 450 (same as Vulkan/Metal/WGPU backends)
    // The glsl4 source from EffectAsset does NOT contain #version;
    // each desktop backend must prepend it at runtime.
    // ============================================================
    const auto legacyShadowRewrite = rewriteD3D12LegacyShadowClipDepth(glslSource);
    const auto optimizedSource = optimizeD3D12ShaderSource(
        stage, shaderName, legacyShadowRewrite.source);
    const ccstd::string &compileSource = optimizedSource.source;
    const ccstd::string diagnosticGroupingSource =
        buildD3D12StageDiagnosticGroupingSource(stage, shaderName, compileSource);
    const ccstd::string processedSource = buildD3D12ProcessedSource(compileSource);
    const ccstd::string processedDiagnosticGroupingSource = buildD3D12ProcessedSource(diagnosticGroupingSource);
    ccstd::string cacheSource = processedSource;
    ccstd::string diagnosticCacheSource = processedDiagnosticGroupingSource;
    if (fragmentLinkage && !fragmentLinkage->empty()) {
        const ccstd::string linkageSalt = makeStageLinkageCacheSalt(*fragmentLinkage);
        cacheSource += linkageSalt;
        diagnosticCacheSource += linkageSalt;
    }

    const ccstd::string diagnosticGroupingKey = makeDXBCCacheKey(
        stage, profile, entryName, compileFlags, diagnosticCacheSource);
    const ccstd::string fullSourceKey = makeDXBCCacheKey(stage, profile, entryName, compileFlags, cacheSource);
    // A normalized key may group variants for diagnostics, but it is not safe as a bytecode cache identity.
    // Cache and in-flight compile ownership must always use the complete processed source.
    const ccstd::string &cacheKey = fullSourceKey;
    const auto cacheLookupPolicy = detail::getD3D12ShaderCacheLookupPolicy(cacheOnly);
    const auto sessionLookupStart = D3D12PerfClock::now();
    const bool sessionCacheHit = loadD3D12ShaderCacheDXBC(cacheKey, outDXBC);
    const uint64_t sessionLookupMs = elapsedMs(sessionLookupStart);
    uint64_t fileLookupMs = 0;
    uint64_t v4LookupMs = 0;
    uint64_t legacyLookupMs = 0;
    uint64_t migrationStoreMs = 0;
    bool fileCacheHit = false;
    bool legacyFileCacheEligible = false;
    bool legacyFileCacheChecked = false;
    bool legacyFileCacheHit = false;
    bool migrationSessionStored = false;
    bool migrationFilePersistenceQueued = false;
    bool migrationFilePersistenceRejected = false;
    const char *cacheHitBackend = nullptr;
    if (sessionCacheHit) {
        cacheHitBackend = "d3d12-session";
    } else {
        const auto fileLookupStart = D3D12PerfClock::now();
        const auto v4LookupStart = D3D12PerfClock::now();
        fileCacheHit = loadFileCachedDXBC(cacheKey, outDXBC);
        v4LookupMs = elapsedMs(v4LookupStart);
        if (fileCacheHit) {
            const bool sessionHydrated = storeD3D12ShaderCacheDXBC(cacheKey, outDXBC);
            cacheHitBackend = sessionHydrated ? "file+session-hydrate" : "file";
        } else if (cacheLookupPolicy.probeLegacyV3) {
            const auto legacyLookupStart = D3D12PerfClock::now();
            const auto legacyOptimizedSource = optimizeD3D12ShaderSource(
                stage, shaderName, legacyShadowRewrite.source, false);
            const bool legacySourceIsSafe = !fragmentLinkage &&
                (!legacyOptimizedSource.changed || !sourceUsesLineSensitiveMacros(glslSource));
            legacyFileCacheEligible = legacySourceIsSafe;
            if (legacySourceIsSafe) {
                const ccstd::string legacyProcessedSource = buildD3D12ProcessedSource(legacyOptimizedSource.source);
                const ccstd::string legacyFullSourceKey = makeDXBCCacheKey(
                    stage, profile, entryName, compileFlags, legacyProcessedSource,
                    CC_D3D12_MIGRATABLE_DXBC_CACHE_VERSION);
                legacyFileCacheChecked = true;
                legacyFileCacheHit = loadFileCachedDXBC(
                    legacyFullSourceKey, outDXBC, CC_D3D12_MIGRATABLE_DXBC_CACHE_VERSION);
                legacyLookupMs = elapsedMs(legacyLookupStart);
                if (legacyFileCacheHit) {
                    // loadFileCachedDXBC already returned canonical bytecode.
                    // Keep the session immediately visible for same-process
                    // consumers, but move write-through file I/O off demand.
                    const auto migrationStoreStart = D3D12PerfClock::now();
                    migrationSessionStored = storeD3D12ShaderCacheDXBC(cacheKey, outDXBC);
                    if (cacheLookupPolicy.deferLegacyFilePersistence) {
                        migrationFilePersistenceQueued = scheduleFileCachedDXBCPersistence(cacheKey, outDXBC);
                    }
                    if (!migrationFilePersistenceQueued) {
                        // A closed queue means Device teardown has started. Do
                        // not bypass that barrier with synchronous file I/O;
                        // v3 remains available for a later migration attempt.
                        migrationFilePersistenceRejected = true;
                    }
                    migrationStoreMs = elapsedMs(migrationStoreStart);
                    cacheHitBackend = "file-v3-migrated";
                }
            } else {
                legacyLookupMs = elapsedMs(legacyLookupStart);
            }
        }
        fileLookupMs = elapsedMs(fileLookupStart);
    }
    CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageCacheLookup name='%s' stage=%s mode=%s sessionHit=%u sessionMs=%llu fileChecked=%u fileHit=%u v4Ms=%llu legacyV3Eligible=%u legacyV3Checked=%u legacyV3Hit=%u legacyMs=%llu migrationSessionStored=%u migrationFileQueued=%u migrationFileRejected=%u migrationStoreMs=%llu fileMs=%llu normalizedKey=%s fullSourceKey=%s normalized=%u",
                shaderName.c_str(), getShaderStageName(stage), cacheOnly ? "cache-only" : "compile-on-miss",
                sessionCacheHit ? 1U : 0U,
                static_cast<unsigned long long>(sessionLookupMs), sessionCacheHit ? 0U : 1U,
                fileCacheHit ? 1U : 0U, static_cast<unsigned long long>(v4LookupMs),
                legacyFileCacheEligible ? 1U : 0U, legacyFileCacheChecked ? 1U : 0U,
                legacyFileCacheHit ? 1U : 0U,
                static_cast<unsigned long long>(legacyLookupMs), migrationSessionStored ? 1U : 0U,
                migrationFilePersistenceQueued ? 1U : 0U, migrationFilePersistenceRejected ? 1U : 0U,
                static_cast<unsigned long long>(migrationStoreMs),
                static_cast<unsigned long long>(fileLookupMs), diagnosticGroupingKey.c_str(),
                fullSourceKey.c_str(), diagnosticGroupingKey == fullSourceKey ? 0U : 1U);
    if (cacheHitBackend) {
        recordDXBCHashComparison(shaderName, stage, diagnosticGroupingKey, fullSourceKey, outDXBC, cacheHitBackend);
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageCompileCacheHit name='%s' stage=%s profile=%s backend=%s totalMs=%llu glslBytes=%u dxbcBytes=%u cacheKey=%s",
                    shaderName.c_str(), getShaderStageName(stage), profile, cacheHitBackend,
                    static_cast<unsigned long long>(elapsedMs(compileStart)),
                    static_cast<unsigned>(compileSource.size()),
                    static_cast<unsigned>(outDXBC.size()),
                    cacheKey.c_str());
        return true;
    }

    if (cacheOnly) {
        if (cacheMiss) {
            *cacheMiss = true;
        }
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageCacheWarmMiss name='%s' stage=%s fullSourceKey=%s totalMs=%llu",
                    shaderName.c_str(), getShaderStageName(stage), fullSourceKey.c_str(),
                    static_cast<unsigned long long>(elapsedMs(compileStart)));
        return false;
    }

    auto inFlightCompile = beginInFlightDXBCCompile(cacheKey);
    if (!inFlightCompile.owner) {
        const auto waitResult = waitForInFlightDXBCCompile(inFlightCompile, outDXBC);
        if (waitResult != InFlightDXBCWaitResult::TIMED_OUT) {
            const bool ok = waitResult == InFlightDXBCWaitResult::COMPLETED_OK;
            if (ok) {
                recordDXBCHashComparison(shaderName, stage, diagnosticGroupingKey, fullSourceKey, outDXBC, "in-flight");
            }
            CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageCompileInFlightHit name='%s' stage=%s profile=%s ok=%u totalMs=%llu glslBytes=%u dxbcBytes=%u cacheKey=%s",
                        shaderName.c_str(), getShaderStageName(stage), profile, ok ? 1U : 0U,
                        static_cast<unsigned long long>(elapsedMs(compileStart)),
                        static_cast<unsigned>(compileSource.size()),
                        static_cast<unsigned>(outDXBC.size()),
                        cacheKey.c_str());
            return ok;
        }
        CC_LOG_WARNING(
            "D3D12Shader: in-flight compile wait timed out after %lld ms for '%s' (%s); compiling locally.",
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    D3D12_INFLIGHT_DXBC_COMPILE_WAIT_TIMEOUT)
                    .count()),
            shaderName.c_str(), getShaderStageName(stage));
        inFlightCompile = replaceInFlightDXBCCompile(cacheKey);
    }
    InFlightDXBCCompletion inFlightCompletion(cacheKey, inFlightCompile, outDXBC);

    // Close the cache-miss/register race: another owner may have completed and
    // removed its in-flight record between our first lookup and registration.
    bool lateCacheHit = loadD3D12ShaderCacheDXBC(cacheKey, outDXBC);
    const char *lateCacheBackend = "d3d12-session";
    if (!lateCacheHit) {
        lateCacheHit = loadFileCachedDXBC(cacheKey, outDXBC);
        lateCacheBackend = "file";
        if (lateCacheHit) {
            storeD3D12ShaderCacheDXBC(cacheKey, outDXBC);
        }
    }
    if (lateCacheHit) {
        recordDXBCHashComparison(shaderName, stage, diagnosticGroupingKey, fullSourceKey, outDXBC, lateCacheBackend);
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageCompileLateCacheHit name='%s' stage=%s backend=%s fullSourceKey=%s totalMs=%llu",
                    shaderName.c_str(), getShaderStageName(stage), lateCacheBackend, fullSourceKey.c_str(),
                    static_cast<unsigned long long>(elapsedMs(compileStart)));
        return inFlightCompletion.finish(true);
    }

    ScopedShaderCompilerPermit compilerPermit;
    if (compilerPermit.waitMs() > 0) {
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderCompilerDemandWait name='%s' stage=%s waitMs=%llu limit=%u",
                    shaderName.c_str(), getShaderStageName(stage),
                    static_cast<unsigned long long>(compilerPermit.waitMs()),
                    CC_D3D12_MAX_CONCURRENT_SHADER_COMPILES);
    }
    ensureGlslangInit();

    // ============================================================
    // Step 1: GLSL -> SPIR-V (using glslang directly, with error checks)
    // ============================================================

    const auto glslToSpirvStart = D3D12PerfClock::now();
    EShLanguage eshStage = toEShLanguage(stage);
    const char *sourcePtr = processedSource.c_str();

    glslang::TShader shader(eshStage);
    shader.setStrings(&sourcePtr, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, eshStage, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);

    auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

    bool parseOK = shader.parse(&glslang::DefaultTBuiltInResource, 450, false, messages);
    if (!parseOK) {
        CC_LOG_ERROR("D3D12Shader: GLSL parse failed:\n%s\n%s",
                     shader.getInfoLog(), shader.getInfoDebugLog());
        return inFlightCompletion.finish(false);
    }

    glslang::TProgram program;
    program.addShader(&shader);

    bool linkOK = program.link(messages);
    if (!linkOK) {
        CC_LOG_ERROR("D3D12Shader: GLSL link failed:\n%s\n%s",
                     program.getInfoLog(), program.getInfoDebugLog());
        return inFlightCompletion.finish(false);
    }

    auto *intermediate = program.getIntermediate(eshStage);
    if (!intermediate) {
        CC_LOG_ERROR("D3D12Shader: glslang getIntermediate() returned null.");
        return inFlightCompletion.finish(false);
    }

    std::vector<uint32_t> spirvOutput;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions spvOptions;
    spvOptions.disableOptimizer = false;
    spvOptions.optimizeSize = true;
#if !defined(NDEBUG)
    // keep debug info in debug builds
#else
    spvOptions.stripDebugInfo = true;
#endif
    glslang::GlslangToSpv(*intermediate, spirvOutput, &logger, &spvOptions);

    if (spirvOutput.empty()) {
        CC_LOG_ERROR("D3D12Shader: GlslangToSpv produced no output.");
        return inFlightCompletion.finish(false);
    }
    glslToSpirvMs = elapsedMs(glslToSpirvStart);

    // ============================================================
    // Step 2: SPIR-V -> HLSL (using SPIRV-Cross)
    // ============================================================
    const auto spirvToHlslStart = D3D12PerfClock::now();
    spirv_cross::CompilerHLSL hlslCompiler(spirvOutput.data(), spirvOutput.size());

    spirv_cross::CompilerHLSL::Options hlslOptions;
    hlslOptions.shader_model = 51; // SM 5.1 — required for register(bN, spaceS) syntax
    hlslOptions.point_size_compat = true;
    hlslOptions.point_coord_compat = true;
    hlslOptions.nonwritable_uav_texture_as_srv = true;
    hlslCompiler.set_hlsl_options(hlslOptions);

    // Note: SPIRV-Cross HLSL backend automatically names entry points as
    // vert_main/frag_main/comp_main etc. We use those names for D3DCompile.
    // rename_entry_point() does not affect HLSL function names, so we skip it.

    // Configure resource bindings so HLSL register assignments match the engine's
    // descriptor set layout model.
    //
    // IMPORTANT: The engine's PipelineLayout creates Root Signature descriptor ranges
    // where each descriptor set maps to its own RegisterSpace (set=0 → space=0,
    // set=1 → space=1, set=2 → space=2). SPIRV-Cross's default behavior also maps
    // GLSL layout(set=N) → HLSL register(bN, spaceN), which naturally matches.
    //
    // We still use add_hlsl_resource_binding to ensure register binding numbers
    // are consistent, and to set the stage field correctly so SPIRV-Cross can
    // find our overrides during remap_hlsl_resource_binding().
    {
        auto resources = hlslCompiler.get_shader_resources();

        // Helper: add HLSL resource binding for a SPIR-V resource
        // Register space = descriptor set (set=N → space=N) to match PipelineLayout's
        // Root Signature, which uses one RegisterSpace per descriptor set.
        // CRITICAL: set stage field so SPIRV-Cross can find this binding during remap
        auto addBinding = [&](const spirv_cross::Resource &res, spirv_cross::CompilerHLSL &compiler) {
            uint32_t set = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
            uint32_t binding = compiler.get_decoration(res.id, spv::DecorationBinding);

            spirv_cross::HLSLResourceBinding hlslBinding;
            hlslBinding.stage = toSPIRVExecutionModel(stage);  // MUST match remap lookup key!
            hlslBinding.desc_set = set;
            hlslBinding.binding = binding;
            // Register space matches descriptor set — the engine's PipelineLayout
            // creates one RegisterSpace per descriptor set (set=0→space=0, etc.)
            hlslBinding.cbv.register_space = set;
            hlslBinding.cbv.register_binding = binding;
            hlslBinding.uav.register_space = set;
            hlslBinding.uav.register_binding = binding;
            hlslBinding.srv.register_space = set;
            hlslBinding.srv.register_binding = binding;
            hlslBinding.sampler.register_space = set;
            hlslBinding.sampler.register_binding = binding;

            compiler.add_hlsl_resource_binding(hlslBinding);
        };

        for (auto &res : resources.uniform_buffers) addBinding(res, hlslCompiler);
        for (auto &res : resources.storage_buffers) addBinding(res, hlslCompiler);
        for (auto &res : resources.sampled_images) addBinding(res, hlslCompiler);
        for (auto &res : resources.separate_images) addBinding(res, hlslCompiler);
        for (auto &res : resources.separate_samplers) addBinding(res, hlslCompiler);
        for (auto &res : resources.storage_images) addBinding(res, hlslCompiler);
        for (auto &res : resources.subpass_inputs) addBinding(res, hlslCompiler);
    }

    std::string hlslSource;
    try {
        hlslSource = hlslCompiler.compile();
    } catch (const spirv_cross::CompilerError &e) {
        CC_LOG_ERROR("D3D12Shader: SPIR-V -> HLSL failed: %s", e.what());
        return inFlightCompletion.finish(false);
    }
    if (stage == ShaderStageFlagBit::FRAGMENT && fragmentLinkage &&
        !patchD3D12FragmentInputLinkage(hlslSource, *fragmentLinkage)) {
        CC_LOG_ERROR("D3D12Shader '%s': failed to align fragment input linkage.", shaderName.c_str());
        return inFlightCompletion.finish(false);
    }
    spirvToHlslMs = elapsedMs(spirvToHlslStart);

    // ============================================================
    // Step 3: HLSL -> DXBC (using D3DCompile)
    // ============================================================
    const auto hlslToDxbcStart = D3D12PerfClock::now();
    auto entryCandidates = getPrimaryHLSLEntryCandidates(entryName);

    HRESULT hr = E_FAIL;
    ccstd::string compileErrors;
    uint32_t entryAttemptCount = 0;
    uint64_t failedEntryCompileMs = 0;
    ccstd::string selectedEntry;
    auto compileCandidates = [&](const ccstd::vector<ccstd::string> &candidates) -> bool {
        for (const auto &candidate : candidates) {
            const auto entryAttemptStart = D3D12PerfClock::now();
            ID3DBlob *codeBlob = nullptr;
            ID3DBlob *errorBlob = nullptr;
            hr = D3DCompile(
                hlslSource.c_str(),
                hlslSource.size(),
                nullptr, nullptr, nullptr,
                candidate.c_str(), profile,
                compileFlags, 0,
                &codeBlob, &errorBlob);

            const uint64_t entryAttemptMs = elapsedMs(entryAttemptStart);
            ++entryAttemptCount;

            if (SUCCEEDED(hr) && codeBlob) {
                selectedEntry = candidate;
                outDXBC.resize(codeBlob->GetBufferSize());
                memcpy(outDXBC.data(), codeBlob->GetBufferPointer(), codeBlob->GetBufferSize());
                codeBlob->Release();
                if (errorBlob) {
                    errorBlob->Release();
                }
                const uint32_t rawDXBCBytes = static_cast<uint32_t>(outDXBC.size());
                std::vector<uint8_t> stableDXBC;
                const bool canonicalized = makeStableDXBC(outDXBC, stableDXBC);
                if (canonicalized) {
                    recordDXBCHashComparison(shaderName, stage, diagnosticGroupingKey, fullSourceKey,
                                             outDXBC, "d3dcompile-raw", &stableDXBC);
                    outDXBC = std::move(stableDXBC);
                } else {
                    recordDXBCHashComparison(shaderName, stage, diagnosticGroupingKey, fullSourceKey,
                                             outDXBC, "d3dcompile-raw");
                }
                CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderEntryCompileAttempt name='%s' stage=%s profile=%s candidate='%s' attempt=%u result=success hr=0x%08x durationMs=%llu",
                            shaderName.c_str(), getShaderStageName(stage), profile, candidate.c_str(), entryAttemptCount,
                            static_cast<unsigned>(hr), static_cast<unsigned long long>(entryAttemptMs));
                CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderDXBCCanonicalize name='%s' stage=%s canonicalized=%u rawBytes=%u stableBytes=%u",
                            shaderName.c_str(), getShaderStageName(stage), canonicalized ? 1U : 0U, rawDXBCBytes,
                            static_cast<unsigned>(outDXBC.size()));
                return true;
            }

            failedEntryCompileMs += entryAttemptMs;
            CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderEntryCompileAttempt name='%s' stage=%s profile=%s candidate='%s' attempt=%u result=failed hr=0x%08x durationMs=%llu",
                        shaderName.c_str(), getShaderStageName(stage), profile, candidate.c_str(), entryAttemptCount,
                        static_cast<unsigned>(hr), static_cast<unsigned long long>(entryAttemptMs));

            if (errorBlob) {
                compileErrors = static_cast<const char *>(errorBlob->GetBufferPointer());
                errorBlob->Release();
            } else {
                compileErrors.clear();
            }
            if (codeBlob) {
                codeBlob->Release();
            }
        }
        return false;
    };

    if (compileCandidates(entryCandidates)) {
        hlslToDxbcMs = elapsedMs(hlslToDxbcStart);
        const char *cacheStoreBackend = storeCachedDXBC(cacheKey, outDXBC);
        recordDXBCHashComparison(shaderName, stage, diagnosticGroupingKey, fullSourceKey, outDXBC, cacheStoreBackend);
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageCompileCacheStore name='%s' stage=%s profile=%s backend=%s dxbcBytes=%u cacheKey=%s",
                    shaderName.c_str(), getShaderStageName(stage), profile, cacheStoreBackend,
                    static_cast<unsigned>(outDXBC.size()), cacheKey.c_str());
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageCompile name='%s' stage=%s profile=%s glslToSpirvMs=%llu spirvToHlslMs=%llu hlslToDxbcMs=%llu totalMs=%llu glslBytes=%u hlslBytes=%u dxbcBytes=%u entryAttempts=%u failedEntryMs=%llu selectedEntry='%s'",
                    shaderName.c_str(), getShaderStageName(stage), profile,
                    static_cast<unsigned long long>(glslToSpirvMs),
                    static_cast<unsigned long long>(spirvToHlslMs),
                    static_cast<unsigned long long>(hlslToDxbcMs),
                    static_cast<unsigned long long>(elapsedMs(compileStart)),
                    static_cast<unsigned>(compileSource.size()),
                    static_cast<unsigned>(hlslSource.size()),
                    static_cast<unsigned>(outDXBC.size()), entryAttemptCount,
                    static_cast<unsigned long long>(failedEntryCompileMs), selectedEntry.c_str());
        return inFlightCompletion.finish(true);
    }

    const auto fallbackEntryCandidates = collectHLSLEntryCandidates(hlslSource, entryName);
    ccstd::vector<ccstd::string> extraFallbackEntryCandidates;
    for (const auto &candidate : fallbackEntryCandidates) {
        bool alreadyTried = false;
        for (const auto &tried : entryCandidates) {
            if (candidate == tried) {
                alreadyTried = true;
                break;
            }
        }
        if (!alreadyTried) {
            extraFallbackEntryCandidates.emplace_back(candidate);
        }
    }

    if (compileCandidates(extraFallbackEntryCandidates)) {
        hlslToDxbcMs = elapsedMs(hlslToDxbcStart);
        const char *cacheStoreBackend = storeCachedDXBC(cacheKey, outDXBC);
        recordDXBCHashComparison(shaderName, stage, diagnosticGroupingKey, fullSourceKey, outDXBC, cacheStoreBackend);
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageCompileCacheStore name='%s' stage=%s profile=%s backend=%s dxbcBytes=%u cacheKey=%s",
                    shaderName.c_str(), getShaderStageName(stage), profile, cacheStoreBackend,
                    static_cast<unsigned>(outDXBC.size()), cacheKey.c_str());
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageCompile name='%s' stage=%s profile=%s glslToSpirvMs=%llu spirvToHlslMs=%llu hlslToDxbcMs=%llu totalMs=%llu glslBytes=%u hlslBytes=%u dxbcBytes=%u fallbackEntries=%u entryAttempts=%u failedEntryMs=%llu selectedEntry='%s'",
                    shaderName.c_str(), getShaderStageName(stage), profile,
                    static_cast<unsigned long long>(glslToSpirvMs),
                    static_cast<unsigned long long>(spirvToHlslMs),
                    static_cast<unsigned long long>(hlslToDxbcMs),
                    static_cast<unsigned long long>(elapsedMs(compileStart)),
                    static_cast<unsigned>(compileSource.size()),
                    static_cast<unsigned>(hlslSource.size()),
                    static_cast<unsigned>(outDXBC.size()),
                    static_cast<unsigned>(extraFallbackEntryCandidates.size()), entryAttemptCount,
                    static_cast<unsigned long long>(failedEntryCompileMs), selectedEntry.c_str());
        return inFlightCompletion.finish(true);
    }

    hlslToDxbcMs = elapsedMs(hlslToDxbcStart);
    CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageCompileFailed name='%s' stage=%s profile=%s glslToSpirvMs=%llu spirvToHlslMs=%llu hlslToDxbcMs=%llu totalMs=%llu glslBytes=%u hlslBytes=%u",
                shaderName.c_str(), getShaderStageName(stage), profile,
                static_cast<unsigned long long>(glslToSpirvMs),
                static_cast<unsigned long long>(spirvToHlslMs),
                static_cast<unsigned long long>(hlslToDxbcMs),
                static_cast<unsigned long long>(elapsedMs(compileStart)),
                static_cast<unsigned>(compileSource.size()),
                static_cast<unsigned>(hlslSource.size()));

    if (!compileErrors.empty()) {
        CC_LOG_ERROR("D3D12Shader: HLSL -> DXBC failed: %s", compileErrors.c_str());
    } else {
        CC_LOG_ERROR("D3D12Shader: HLSL -> DXBC failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
    }
    return inFlightCompletion.finish(false);
}

void CCD3D12Shader::scheduleStagePrecompile(ShaderStageFlagBit stage) {
    if (!_impl) {
        return;
    }

    auto *stageRecord = _impl->getStageRecord(stage);
    if (!stageRecord || stageRecord->source.empty() || stageRecord->failed || stageRecord->precompileStarted) {
        return;
    }
    if (!shouldWarmD3D12ShaderStage(stage)) {
        return;
    }

    auto *dxbcBuffer = _impl->getDXBCBuffer(stage);
    if (!dxbcBuffer || !dxbcBuffer->empty()) {
        return;
    }

    stageRecord->precompileStarted = true;
    const ccstd::string source = stageRecord->source;
    const ccstd::string entry = stageRecord->entry;
    const ccstd::string shaderName = stageRecord->shaderName;
    const auto scheduledAt = D3D12PerfClock::now();
    auto asyncResult = std::make_shared<Impl::AsyncCompileResult>();
    stageRecord->asyncResult = asyncResult;

    stageRecord->precompileTask = getD3D12ShaderCompileScheduler().submit(
        [stage, source, entry, shaderName, scheduledAt, asyncResult](bool demanded) {
            const auto executionStart = D3D12PerfClock::now();
            const uint64_t queueWaitMs = elapsedMs(scheduledAt);
            ScopedShaderPrecompileCounter precompileCounter;
            const uint32_t activePrecompiles = precompileCounter.active();
            CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStagePrecompileBegin name='%s' stage=%s dispatch=%s queueWaitMs=%llu backgroundWorkers=%u concurrencyLimit=%u active=%u peak=%u",
                        shaderName.c_str(), getShaderStageName(stage), demanded ? "demand" : "background",
                        static_cast<unsigned long long>(queueWaitMs),
                        CC_D3D12_BACKGROUND_SHADER_PRECOMPILE_WORKERS,
                        CC_D3D12_MAX_CONCURRENT_SHADER_COMPILES,
                        activePrecompiles,
                        s_peakShaderPrecompiles.load(std::memory_order_relaxed));
            std::vector<uint8_t> compiledDXBC;
            bool cacheMiss = false;
            const bool ok = compileGLSLToDXBC(stage, source, entry, shaderName, compiledDXBC,
                                               !demanded, &cacheMiss);
            asyncResult->ok = ok;
            asyncResult->cacheMiss = cacheMiss;
            asyncResult->dxbc = std::move(compiledDXBC);

            const uint32_t remainingPrecompiles = precompileCounter.finish();
            CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStagePrecompile name='%s' stage=%s dispatch=%s outcome=%s sourceBytes=%u queueWaitMs=%llu executionMs=%llu totalMs=%llu activeAfter=%u peak=%u",
                        shaderName.c_str(), getShaderStageName(stage), demanded ? "demand" : "background",
                        ok ? "ready" : (cacheMiss ? "cache-miss" : "failed"),
                        static_cast<unsigned>(source.size()),
                        static_cast<unsigned long long>(queueWaitMs),
                        static_cast<unsigned long long>(elapsedMs(executionStart)),
                        static_cast<unsigned long long>(elapsedMs(scheduledAt)),
                        remainingPrecompiles,
                        s_peakShaderPrecompiles.load(std::memory_order_relaxed));
            return ok;
        });
}

void CCD3D12Shader::waitForBackgroundCompiles() {
    if (!_impl) {
        return;
    }

    ccstd::vector<detail::D3D12ShaderCompileScheduler::Handle> tasks;
    for (ShaderStageFlagBit stage : {
             ShaderStageFlagBit::VERTEX,
             ShaderStageFlagBit::FRAGMENT,
             ShaderStageFlagBit::GEOMETRY,
             ShaderStageFlagBit::COMPUTE,
             ShaderStageFlagBit::CONTROL,
             ShaderStageFlagBit::EVALUATION,
         }) {
        auto *record = _impl->getStageRecord(stage);
        if (record && record->precompileTask) {
            tasks.emplace_back(record->precompileTask);
        }
    }

    for (const auto &task : tasks) {
        // Destruction should not drain speculative work. Cancel queued tasks and
        // wait only for work that already owns a worker or foreground caller.
        getD3D12ShaderCompileScheduler().cancelAndWait(task);
    }
}

void CCD3D12Shader::doInit(const ShaderInfo &info) {
    const auto initStart = D3D12PerfClock::now();
    // Close the previous generation before replacing StageRecords. This also
    // covers cache-miss work that left the scheduler and is compiling on a
    // caller thread.
    _impl->stopAcceptingStageRequests();
    waitForBackgroundCompiles();
    _impl->waitForActiveStageRequests();
    {
        std::lock_guard<std::mutex> lock(_impl->compileMutex);
        _impl->vertexDXBC.clear();
        _impl->fragmentDXBC.clear();
        _impl->linkedFragmentDXBC.clear();
        _impl->linkedFragmentKey.clear();
        _impl->geometryDXBC.clear();
        _impl->computeDXBC.clear();
        _impl->hullDXBC.clear();
        _impl->domainDXBC.clear();
        _impl->vertexInputSignature.clear();
        _impl->vertexSource = {};
        _impl->fragmentSource = {};
        _impl->geometrySource = {};
        _impl->computeSource = {};
        _impl->hullSource = {};
        _impl->domainSource = {};
    }

    for (const auto &stage : _stages) {
        const auto stageInitStart = D3D12PerfClock::now();
        std::vector<uint8_t> *dxbcBuffer = _impl->getDXBCBuffer(stage.stage);
        auto *stageRecord = _impl->getStageRecord(stage.stage);
        ccstd::string *entryName = nullptr;

        if (stage.stage == ShaderStageFlagBit::VERTEX) {
            entryName = &_impl->vertexEntry;
        } else if (stage.stage == ShaderStageFlagBit::FRAGMENT) {
            entryName = &_impl->fragmentEntry;
        }

        if (!dxbcBuffer) continue;
        dxbcBuffer->clear();
        if (stageRecord) {
            *stageRecord = {};
        }

        if (stage.source.empty()) {
            CC_LOG_WARNING("D3D12Shader '%s': stage 0x%x has no source.", info.name.c_str(), static_cast<unsigned>(stage.stage));
            continue;
        }

        // Check if already DXBC bytecode
        const bool isDXBC = (stage.source.size() >= 4 &&
                             static_cast<unsigned char>(stage.source[0]) == 0x44 &&
                             static_cast<unsigned char>(stage.source[1]) == 0x58 &&
                             static_cast<unsigned char>(stage.source[2]) == 0x42 &&
                             static_cast<unsigned char>(stage.source[3]) == 0x43);

        if (isDXBC) {
            std::vector<uint8_t> sourceDXBC(stage.source.begin(), stage.source.end());
            if (!makeStableDXBC(sourceDXBC, *dxbcBuffer)) {
                *dxbcBuffer = std::move(sourceDXBC);
            }

            uint64_t reflectMs = 0;
            if (stage.stage == ShaderStageFlagBit::VERTEX && !dxbcBuffer->empty()) {
                const auto reflectStart = D3D12PerfClock::now();
                reflectVertexInputSignature(*dxbcBuffer, _impl->vertexInputSignature);
                reflectMs = elapsedMs(reflectStart);
            }

            CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageInit name='%s' stage=%s source=DXBC sourceBytes=%u dxbcBytes=%u reflectMs=%llu totalMs=%llu",
                        info.name.c_str(), getShaderStageName(stage.stage),
                        static_cast<unsigned>(stage.source.size()),
                        static_cast<unsigned>(dxbcBuffer->size()),
                        static_cast<unsigned long long>(reflectMs),
                        static_cast<unsigned long long>(elapsedMs(stageInitStart)));
        } else {
            ccstd::string entry = entryName ? *entryName : getSPIRVCrossEntryName(stage.stage);
            if (stageRecord) {
                stageRecord->source = stage.source;
                stageRecord->entry = entry;
                stageRecord->shaderName = info.name;
                stageRecord->failed = false;
            }
            scheduleStagePrecompile(stage.stage);
            CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageInit name='%s' stage=%s source=GLSL-LAZY precompile=%s sourceBytes=%u dxbcBytes=0 reflectMs=0 totalMs=%llu",
                        info.name.c_str(), getShaderStageName(stage.stage),
                        stageRecord && stageRecord->precompileStarted ? "scheduled" : "not-scheduled",
                        static_cast<unsigned>(stage.source.size()),
                        static_cast<unsigned long long>(elapsedMs(stageInitStart)));
        }
    }

    CC_D3D12_DIAGNOSTIC_LOG("D3D12Shader '%s' initialized with %u stages.",
                info.name.c_str(), static_cast<unsigned>(_stages.size()));
    CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderInit name='%s' stages=%u totalMs=%llu",
                info.name.c_str(), static_cast<unsigned>(_stages.size()),
                static_cast<unsigned long long>(elapsedMs(initStart)));
    _impl->startAcceptingStageRequests();
}

void CCD3D12Shader::doDestroy() {
    if (_impl) {
        _impl->stopAcceptingStageRequests();
        waitForBackgroundCompiles();
        _impl->waitForActiveStageRequests();
        std::lock_guard<std::mutex> lock(_impl->compileMutex);
        _impl->vertexDXBC.clear();
        _impl->fragmentDXBC.clear();
        _impl->linkedFragmentDXBC.clear();
        _impl->linkedFragmentKey.clear();
        _impl->geometryDXBC.clear();
        _impl->computeDXBC.clear();
        _impl->hullDXBC.clear();
        _impl->domainDXBC.clear();
        _impl->vertexInputSignature.clear();
        _impl->vertexSource = {};
        _impl->fragmentSource = {};
        _impl->geometrySource = {};
        _impl->computeSource = {};
        _impl->hullSource = {};
        _impl->domainSource = {};
    }
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::ensureStageBytecode(ShaderStageFlagBit stage) const {
    if (!_impl) {
        return {};
    }

    auto *impl = _impl.get();
    auto *dxbcBuffer = impl->getDXBCBuffer(stage);
    auto *stageRecord = impl->getStageRecord(stage);
    if (!dxbcBuffer || !stageRecord) {
        return {};
    }

    Impl::StageRequestLease stageRequestLease;
    Impl::ForegroundCompileLease foregroundCompileLease;
    detail::D3D12ShaderCompileScheduler::Handle precompileTask;
    std::shared_ptr<Impl::AsyncCompileResult> asyncResult;
    ccstd::string source;
    ccstd::string entry;
    ccstd::string shaderName;
    uint64_t stageGeneration = 0;
    {
        std::unique_lock<std::mutex> lock(impl->compileMutex);
        if (!impl->acceptingStageRequests) {
            return {};
        }
        ++impl->activeStageRequests;
        stageRequestLease.arm(impl);
        stageGeneration = impl->generation;
        if (!dxbcBuffer->empty()) {
            return {dxbcBuffer->data(), dxbcBuffer->size()};
        }
        if (stageRecord->source.empty() || stageRecord->failed) {
            return {};
        }
        precompileTask = stageRecord->precompileTask;
        asyncResult = stageRecord->asyncResult;
        shaderName = stageRecord->shaderName;

        if (!precompileTask) {
            impl->compileCondition.wait(lock, [impl, stageRecord, stageGeneration]() {
                return !impl->acceptingStageRequests ||
                       impl->generation != stageGeneration ||
                       !stageRecord->foregroundCompileRunning;
            });
            if (!impl->acceptingStageRequests || impl->generation != stageGeneration) {
                return {};
            }
            if (!dxbcBuffer->empty()) {
                return {dxbcBuffer->data(), dxbcBuffer->size()};
            }
            if (stageRecord->failed) {
                return {};
            }
            stageRecord->foregroundCompileRunning = true;
            foregroundCompileLease.arm(impl, stageRecord, stageGeneration);
            source = stageRecord->source;
            entry = stageRecord->entry;
        }
    }

    if (precompileTask) {
        const auto demandStart = D3D12PerfClock::now();
        bool ranInline = false;
        const bool taskOK = getD3D12ShaderCompileScheduler().runNowOrWait(precompileTask, ranInline);
        bool hasBytecode = false;
        bool cacheMiss = false;
        uint64_t reflectMs = 0;
        {
            std::lock_guard<std::mutex> lock(impl->compileMutex);
            if (!impl->acceptingStageRequests || impl->generation != stageGeneration) {
                return {};
            }
            if (asyncResult) {
                cacheMiss = asyncResult->cacheMiss;
                if (taskOK && asyncResult->ok && !asyncResult->dxbc.empty() && dxbcBuffer->empty()) {
                    *dxbcBuffer = std::move(asyncResult->dxbc);
                    // DXBC is now installed; the HLSL source string in the
                    // stage record is no longer referenced (subsequent
                    // ensureStageBytecode calls return early when the DXBC
                    // buffer is non-empty). Release it to avoid retaining the
                    // full per-stage HLSL source for the shader's lifetime.
                    // The fragment source must be kept: VS/PS linkage repair
                    // (getFragmentBytecodeForVertexLinkage) recompiles it.
                    if (stage != ShaderStageFlagBit::FRAGMENT) {
                        stageRecord->source.clear();
                        stageRecord->source.shrink_to_fit();
                    }
                    if (stage == ShaderStageFlagBit::VERTEX) {
                        const auto reflectStart = D3D12PerfClock::now();
                        impl->vertexInputSignature.clear();
                        reflectVertexInputSignature(*dxbcBuffer, impl->vertexInputSignature);
                        reflectMs = elapsedMs(reflectStart);
                    }
                } else if (!taskOK && !cacheMiss) {
                    stageRecord->failed = true;
                }
            }
            hasBytecode = !dxbcBuffer->empty();
        }
        CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStagePrecompileDemand name='%s' stage=%s dispatch=%s outcome=%s reflectMs=%llu waitMs=%llu",
                    shaderName.c_str(), getShaderStageName(stage), ranInline ? "inline" : "wait-running",
                    hasBytecode ? "ready" : (cacheMiss ? "cache-miss" : "failed"),
                    static_cast<unsigned long long>(reflectMs),
                    static_cast<unsigned long long>(elapsedMs(demandStart)));
        if (hasBytecode) {
            return {dxbcBuffer->data(), dxbcBuffer->size()};
        }
        if (!cacheMiss) {
            return {};
        }

        std::unique_lock<std::mutex> lock(impl->compileMutex);
        impl->compileCondition.wait(lock, [impl, stageRecord, stageGeneration]() {
            return !impl->acceptingStageRequests ||
                   impl->generation != stageGeneration ||
                   !stageRecord->foregroundCompileRunning;
        });
        if (!impl->acceptingStageRequests || impl->generation != stageGeneration) {
            return {};
        }
        if (!dxbcBuffer->empty()) {
            return {dxbcBuffer->data(), dxbcBuffer->size()};
        }
        if (stageRecord->failed) {
            return {};
        }
        stageRecord->foregroundCompileRunning = true;
        foregroundCompileLease.arm(impl, stageRecord, stageGeneration);
        source = stageRecord->source;
        entry = stageRecord->entry;
    }

    const auto lazyStart = D3D12PerfClock::now();
    std::vector<uint8_t> compiledDXBC;
    bool ok = false;
    try {
        ok = compileGLSLToDXBC(stage,
                               source,
                               entry,
                               shaderName,
                               compiledDXBC);
    } catch (const std::exception &exception) {
        CC_LOG_ERROR("D3D12Shader '%s': lazy GLSL->DXBC threw for stage 0x%x: %s",
                     shaderName.c_str(), static_cast<unsigned>(stage), exception.what());
    } catch (...) {
        CC_LOG_ERROR("D3D12Shader '%s': lazy GLSL->DXBC threw for stage 0x%x.",
                     shaderName.c_str(), static_cast<unsigned>(stage));
    }
    if (!ok) {
        {
            std::lock_guard<std::mutex> lock(impl->compileMutex);
            if (impl->acceptingStageRequests && impl->generation == stageGeneration) {
                stageRecord->failed = true;
                dxbcBuffer->clear();
            }
        }
        CC_LOG_ERROR("D3D12Shader '%s': lazy GLSL->DXBC failed for stage 0x%x.",
                     shaderName.c_str(), static_cast<unsigned>(stage));
        return {};
    }

    uint64_t reflectMs = 0;
    bool installed = false;
    {
        std::lock_guard<std::mutex> lock(impl->compileMutex);
        if (impl->acceptingStageRequests && impl->generation == stageGeneration) {
            *dxbcBuffer = std::move(compiledDXBC);
            // DXBC installed: release the HLSL source retained in the stage
            // record. The bytecode is the authoritative artifact from here on.
            // Keep the fragment source for possible VS/PS linkage repair.
            if (stage != ShaderStageFlagBit::FRAGMENT) {
                stageRecord->source.clear();
                stageRecord->source.shrink_to_fit();
            }
            if (stage == ShaderStageFlagBit::VERTEX && !dxbcBuffer->empty()) {
                const auto reflectStart = D3D12PerfClock::now();
                impl->vertexInputSignature.clear();
                reflectVertexInputSignature(*dxbcBuffer, impl->vertexInputSignature);
                reflectMs = elapsedMs(reflectStart);
            }
            installed = !dxbcBuffer->empty();
        }
    }
    if (!installed) {
        return {};
    }

    CC_D3D12_DIAGNOSTIC_LOG("[D3D12-PERF] ShaderStageLazyCompile name='%s' stage=%s sourceBytes=%u dxbcBytes=%u reflectMs=%llu totalMs=%llu",
                shaderName.c_str(), getShaderStageName(stage),
                static_cast<unsigned>(source.size()),
                static_cast<unsigned>(dxbcBuffer->size()),
                static_cast<unsigned long long>(reflectMs),
                static_cast<unsigned long long>(elapsedMs(lazyStart)));
    return {dxbcBuffer->data(), dxbcBuffer->size()};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getVertexBytecode() const {
    return ensureStageBytecode(ShaderStageFlagBit::VERTEX);
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getFragmentBytecode() const {
    return ensureStageBytecode(ShaderStageFlagBit::FRAGMENT);
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getFragmentBytecodeForVertexLinkage() const {
    const BytecodeBlob vertexBytecode = ensureStageBytecode(ShaderStageFlagBit::VERTEX);
    const BytecodeBlob fragmentBytecode = ensureStageBytecode(ShaderStageFlagBit::FRAGMENT);
    if (!vertexBytecode.data || !fragmentBytecode.data) {
        return fragmentBytecode;
    }

    Impl::StageRequestLease linkageRequestLease;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(_impl->compileMutex);
        if (!_impl->acceptingStageRequests) {
            return {};
        }
        ++_impl->activeStageRequests;
        linkageRequestLease.arm(_impl.get());
        generation = _impl->generation;
    }

    ccstd::vector<StageLinkageParameter> vertexOutputs;
    ccstd::vector<StageLinkageParameter> fragmentInputs;
    if (!reflectStageLinkage(vertexBytecode.data, vertexBytecode.size, true, vertexOutputs) ||
        !reflectStageLinkage(fragmentBytecode.data, fragmentBytecode.size, false, fragmentInputs) ||
        stageLinkageIsCompatible(vertexOutputs, fragmentInputs)) {
        return fragmentBytecode;
    }

    const ccstd::string linkageKey = makeStageLinkageCacheSalt(vertexOutputs);
    ccstd::string source;
    ccstd::string entry;
    ccstd::string shaderName;
    {
        std::lock_guard<std::mutex> lock(_impl->compileMutex);
        if (!_impl->acceptingStageRequests) {
            return fragmentBytecode;
        }
        if (_impl->linkedFragmentKey == linkageKey && !_impl->linkedFragmentDXBC.empty()) {
            return {_impl->linkedFragmentDXBC.data(), _impl->linkedFragmentDXBC.size()};
        }
        if (_impl->fragmentSource.source.empty()) {
            CC_LOG_ERROR("D3D12Shader: incompatible VS/PS linkage cannot be repaired without fragment GLSL source.");
            return fragmentBytecode;
        }
        source = _impl->fragmentSource.source;
        entry = _impl->fragmentSource.entry;
        shaderName = _impl->fragmentSource.shaderName;
    }

    std::vector<uint8_t> linkedBytecode;
    if (!compileGLSLToDXBC(ShaderStageFlagBit::FRAGMENT, source, entry, shaderName,
                           linkedBytecode, false, nullptr, &vertexOutputs)) {
        CC_LOG_ERROR("D3D12Shader '%s': failed to compile linkage-compatible fragment bytecode.",
                     shaderName.c_str());
        return fragmentBytecode;
    }

    ccstd::vector<StageLinkageParameter> linkedInputs;
    if (!reflectStageLinkage(linkedBytecode.data(), linkedBytecode.size(), false, linkedInputs) ||
        !stageLinkageIsCompatible(vertexOutputs, linkedInputs)) {
        CC_LOG_ERROR("D3D12Shader '%s': repaired fragment bytecode still has incompatible stage linkage.",
                     shaderName.c_str());
        return fragmentBytecode;
    }

    std::lock_guard<std::mutex> lock(_impl->compileMutex);
    if (!_impl->acceptingStageRequests || _impl->generation != generation) {
        return {};
    }
    if (_impl->linkedFragmentKey != linkageKey || _impl->linkedFragmentDXBC.empty()) {
        _impl->linkedFragmentDXBC = std::move(linkedBytecode);
        _impl->linkedFragmentKey = linkageKey;
    }
    return {_impl->linkedFragmentDXBC.data(), _impl->linkedFragmentDXBC.size()};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getGeometryBytecode() const {
    return ensureStageBytecode(ShaderStageFlagBit::GEOMETRY);
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getComputeBytecode() const {
    return ensureStageBytecode(ShaderStageFlagBit::COMPUTE);
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getHullBytecode() const {
    return ensureStageBytecode(ShaderStageFlagBit::CONTROL);
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getDomainBytecode() const {
    return ensureStageBytecode(ShaderStageFlagBit::EVALUATION);
}

const ccstd::string &CCD3D12Shader::getVertexEntry() const {
    static const ccstd::string empty;
    return _impl ? _impl->vertexEntry : empty;
}

const ccstd::string &CCD3D12Shader::getFragmentEntry() const {
    static const ccstd::string empty;
    return _impl ? _impl->fragmentEntry : empty;
}

const std::vector<CCD3D12Shader::VertexInputSignature> &CCD3D12Shader::getVertexInputSignature() const {
    static const std::vector<VertexInputSignature> empty;
    const auto bytecode = ensureStageBytecode(ShaderStageFlagBit::VERTEX);
    if (!_impl || !bytecode.data) {
        return empty;
    }
    std::lock_guard<std::mutex> lock(_impl->compileMutex);
    return _impl->acceptingStageRequests ? _impl->vertexInputSignature : empty;
}

bool CCD3D12Shader::hasBytecode(ShaderStageFlagBit stage) const {
    if (!_impl) return false;
    auto blob = getStageBytecode(stage);
    return blob.data != nullptr && blob.size > 0;
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getStageBytecode(ShaderStageFlagBit stage) const {
    if (!_impl) return {};
    if (stage == ShaderStageFlagBit::VERTEX) return getVertexBytecode();
    if (stage == ShaderStageFlagBit::FRAGMENT) return getFragmentBytecode();
    if (stage == ShaderStageFlagBit::GEOMETRY) return getGeometryBytecode();
    if (stage == ShaderStageFlagBit::COMPUTE) return getComputeBytecode();
    if (stage == ShaderStageFlagBit::CONTROL) return getHullBytecode();
    if (stage == ShaderStageFlagBit::EVALUATION) return getDomainBytecode();
    return {};
}

} // namespace gfx
} // namespace cc
