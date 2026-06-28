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
#include "D3D12Device.h"
#include "base/Data.h"
#include "base/Log.h"
#include "platform/FileUtils.h"

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

#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <future>
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

constexpr uint32_t CC_D3D12_DXBC_CACHE_VERSION = 2;
constexpr uint64_t FNV1A64_OFFSET = 14695981039346656037ULL;
constexpr uint64_t FNV1A64_PRIME = 1099511628211ULL;

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

bool isD3D12VertexCacheNeutralDefine(const ccstd::string &name) {
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

bool shouldPrecompileD3D12ShaderStage(const ccstd::string &shaderName, ShaderStageFlagBit stage) {
    if (stage != ShaderStageFlagBit::VERTEX && stage != ShaderStageFlagBit::FRAGMENT) {
        return false;
    }
    return shaderName.find("|standard-vs|standard-fs|") != ccstd::string::npos;
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

struct D3D12ShaderSourceOptimization {
    ccstd::string source;
    uint32_t removedDefines{0};
    bool changed{false};
};

D3D12ShaderSourceOptimization optimizeD3D12ShaderSource(ShaderStageFlagBit stage,
                                                        const ccstd::string &shaderName,
                                                        const ccstd::string &source) {
    D3D12ShaderSourceOptimization result;
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
                                   !sourceReferencesIdentifierOutsideLine(source, defineName, lineStart, lineEnd);
        if (canDropDefine) {
            ++result.removedDefines;
            result.changed = true;
        } else {
            result.source.append(source.data() + lineStart, nextLineStart - lineStart);
        }

        lineStart = nextLineStart;
    }

    if (!result.changed) {
        result.source = source;
    } else {
        CC_LOG_INFO("[D3D12-PERF] ShaderSourceOptimize name='%s' stage=%s removedDefines=%u originalBytes=%u optimizedBytes=%u",
                    shaderName.c_str(), getShaderStageName(stage),
                    result.removedDefines,
                    static_cast<unsigned>(source.size()),
                    static_cast<unsigned>(result.source.size()));
    }
    return result;
}

ccstd::string buildD3D12StageCacheSource(ShaderStageFlagBit stage,
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
                                isD3D12VertexCacheNeutralDefine(defineName);
        if (dropDefine) {
            ++removedDefines;
        } else {
            result.append(source.data() + lineStart, nextLineStart - lineStart);
        }

        lineStart = nextLineStart;
    }

    if (removedDefines > 0) {
        CC_LOG_INFO("[D3D12-PERF] ShaderCacheKeyNormalize name='%s' stage=%s removedDefines=%u sourceBytes=%u keyBytes=%u",
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
                               const ccstd::string &processedSource) {
    uint64_t hashA = FNV1A64_OFFSET;
    uint64_t hashB = FNV1A64_OFFSET ^ 0x9e3779b97f4a7c15ULL;
    const uint32_t version = CC_D3D12_DXBC_CACHE_VERSION;
    const uint32_t stageValue = static_cast<uint32_t>(stage);

    appendHashBytes(hashA, &version, sizeof(version));
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
    appendHashBytes(hashB, &version, sizeof(version));

    return toHex(hashA) + toHex(hashB);
}

ccstd::string getDXBCCacheDirectory(cc::FileUtils *fileUtils) {
    if (!fileUtils) {
        return "";
    }
    ccstd::string root = fileUtils->getWritablePath();
    if (root.empty()) {
        return "";
    }
    root = joinCachePath(root, "d3d12-dxbc-cache");
    return joinCachePath(root, "v" + std::to_string(CC_D3D12_DXBC_CACHE_VERSION));
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
    return true;
}

bool storeD3D12ShaderCacheDXBC(const ccstd::string &cacheKey, const std::vector<uint8_t> &dxbc) {
    auto *device = CCD3D12Device::getInstance();
    return device && device->storeShaderCacheValue(cacheKey.data(), static_cast<uint32_t>(cacheKey.size()), dxbc);
}

bool loadFileCachedDXBC(const ccstd::string &cacheKey, std::vector<uint8_t> &outDXBC) {
    auto *fileUtils = cc::FileUtils::getInstance();
    const ccstd::string cacheDir = getDXBCCacheDirectory(fileUtils);
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

    outDXBC.assign(bytes, bytes + size);
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
    cc::Data data;
    data.copy(dxbc.data(), static_cast<uint32_t>(dxbc.size()));
    if (!fileUtils->writeDataToFile(data, cachePath)) {
        CC_LOG_WARNING("D3D12Shader: failed to write DXBC cache entry '%s'.", cachePath.c_str());
        return false;
    }
    return true;
}

const char *storeCachedDXBC(const ccstd::string &cacheKey, const std::vector<uint8_t> &dxbc) {
    const bool storedSession = storeD3D12ShaderCacheDXBC(cacheKey, dxbc);
    const bool storedFile = storeFileCachedDXBC(cacheKey, dxbc);
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

bool waitForInFlightDXBCCompile(const InFlightDXBCCompileTicket &ticket, std::vector<uint8_t> &outDXBC) {
    if (!ticket.compile) {
        return false;
    }

    std::unique_lock<std::mutex> lock(ticket.compile->mutex);
    ticket.compile->completedCondition.wait(lock, [&ticket]() {
        return ticket.compile->completed;
    });
    if (ticket.compile->ok) {
        outDXBC = ticket.compile->dxbc;
    } else {
        outDXBC.clear();
    }
    return ticket.compile->ok;
}

bool finishInFlightDXBCCompile(const ccstd::string &cacheKey,
                               const InFlightDXBCCompileTicket &ticket,
                               bool ok,
                               const std::vector<uint8_t> &dxbc) {
    if (ticket.compile) {
        {
            std::lock_guard<std::mutex> lock(ticket.compile->mutex);
            ticket.compile->ok = ok;
            ticket.compile->dxbc = ok ? dxbc : std::vector<uint8_t>{};
            ticket.compile->completed = true;
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

struct CCD3D12Shader::Impl {
    struct StageRecord {
        ccstd::string source;
        ccstd::string entry;
        ccstd::string shaderName;
        std::shared_future<bool> precompileFuture;
        bool failed{false};
        bool precompileStarted{false};
    };

    // Per-stage bytecode storage (self-owned)
    std::vector<uint8_t> vertexDXBC;
    std::vector<uint8_t> fragmentDXBC;
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
                                       std::vector<uint8_t> &outDXBC) const {
    const auto compileStart = D3D12PerfClock::now();
    uint64_t glslToSpirvMs = 0;
    uint64_t spirvToHlslMs = 0;
    uint64_t hlslToDxbcMs = 0;
    const char *profile = getHLSLProfile(stage);
    UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if !defined(NDEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    // ============================================================
    // Step 0: Prepend #version 450 (same as Vulkan/Metal/WGPU backends)
    // The glsl4 source from EffectAsset does NOT contain #version;
    // each desktop backend must prepend it at runtime.
    // ============================================================
    const auto optimizedSource = optimizeD3D12ShaderSource(stage, shaderName, glslSource);
    const ccstd::string &compileSource = optimizedSource.source;
    const ccstd::string cacheSource = buildD3D12StageCacheSource(stage, shaderName, compileSource);
    const ccstd::string processedSource = buildD3D12ProcessedSource(compileSource);
    const ccstd::string processedCacheSource = buildD3D12ProcessedSource(cacheSource);

    const ccstd::string cacheKey = makeDXBCCacheKey(stage, profile, entryName, compileFlags, processedCacheSource);
    const char *cacheHitBackend = nullptr;
    if (loadD3D12ShaderCacheDXBC(cacheKey, outDXBC)) {
        cacheHitBackend = "d3d12-session";
    } else if (loadFileCachedDXBC(cacheKey, outDXBC)) {
        cacheHitBackend = "file";
    }
    if (cacheHitBackend) {
        CC_LOG_INFO("[D3D12-PERF] ShaderStageCompileCacheHit name='%s' stage=%s profile=%s backend=%s totalMs=%llu glslBytes=%u dxbcBytes=%u cacheKey=%s",
                    shaderName.c_str(), getShaderStageName(stage), profile, cacheHitBackend,
                    static_cast<unsigned long long>(elapsedMs(compileStart)),
                    static_cast<unsigned>(compileSource.size()),
                    static_cast<unsigned>(outDXBC.size()),
                    cacheKey.c_str());
        return true;
    }

    const auto inFlightCompile = beginInFlightDXBCCompile(cacheKey);
    if (!inFlightCompile.owner) {
        const bool ok = waitForInFlightDXBCCompile(inFlightCompile, outDXBC);
        CC_LOG_INFO("[D3D12-PERF] ShaderStageCompileInFlightHit name='%s' stage=%s profile=%s ok=%u totalMs=%llu glslBytes=%u dxbcBytes=%u cacheKey=%s",
                    shaderName.c_str(), getShaderStageName(stage), profile, ok ? 1U : 0U,
                    static_cast<unsigned long long>(elapsedMs(compileStart)),
                    static_cast<unsigned>(compileSource.size()),
                    static_cast<unsigned>(outDXBC.size()),
                    cacheKey.c_str());
        return ok;
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
        return finishInFlightDXBCCompile(cacheKey, inFlightCompile, false, outDXBC);
    }

    glslang::TProgram program;
    program.addShader(&shader);

    bool linkOK = program.link(messages);
    if (!linkOK) {
        CC_LOG_ERROR("D3D12Shader: GLSL link failed:\n%s\n%s",
                     program.getInfoLog(), program.getInfoDebugLog());
        return finishInFlightDXBCCompile(cacheKey, inFlightCompile, false, outDXBC);
    }

    auto *intermediate = program.getIntermediate(eshStage);
    if (!intermediate) {
        CC_LOG_ERROR("D3D12Shader: glslang getIntermediate() returned null.");
        return finishInFlightDXBCCompile(cacheKey, inFlightCompile, false, outDXBC);
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
        return finishInFlightDXBCCompile(cacheKey, inFlightCompile, false, outDXBC);
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
        return finishInFlightDXBCCompile(cacheKey, inFlightCompile, false, outDXBC);
    }
    spirvToHlslMs = elapsedMs(spirvToHlslStart);

    // ============================================================
    // Step 3: HLSL -> DXBC (using D3DCompile)
    // ============================================================
    const auto hlslToDxbcStart = D3D12PerfClock::now();
    auto entryCandidates = getPrimaryHLSLEntryCandidates(entryName);

    HRESULT hr = E_FAIL;
    ccstd::string compileErrors;
    auto compileCandidates = [&](const ccstd::vector<ccstd::string> &candidates) -> bool {
        for (const auto &candidate : candidates) {
            ID3DBlob *codeBlob = nullptr;
            ID3DBlob *errorBlob = nullptr;
            hr = D3DCompile(
                hlslSource.c_str(),
                hlslSource.size(),
                nullptr, nullptr, nullptr,
                candidate.c_str(), profile,
                compileFlags, 0,
                &codeBlob, &errorBlob);

            if (SUCCEEDED(hr) && codeBlob) {
                outDXBC.resize(codeBlob->GetBufferSize());
                memcpy(outDXBC.data(), codeBlob->GetBufferPointer(), codeBlob->GetBufferSize());
                codeBlob->Release();
                if (errorBlob) {
                    errorBlob->Release();
                }
                return true;
            }

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
        CC_LOG_INFO("[D3D12-PERF] ShaderStageCompileCacheStore name='%s' stage=%s profile=%s backend=%s dxbcBytes=%u cacheKey=%s",
                    shaderName.c_str(), getShaderStageName(stage), profile, cacheStoreBackend,
                    static_cast<unsigned>(outDXBC.size()), cacheKey.c_str());
        CC_LOG_INFO("[D3D12-PERF] ShaderStageCompile name='%s' stage=%s profile=%s glslToSpirvMs=%llu spirvToHlslMs=%llu hlslToDxbcMs=%llu totalMs=%llu glslBytes=%u hlslBytes=%u dxbcBytes=%u",
                    shaderName.c_str(), getShaderStageName(stage), profile,
                    static_cast<unsigned long long>(glslToSpirvMs),
                    static_cast<unsigned long long>(spirvToHlslMs),
                    static_cast<unsigned long long>(hlslToDxbcMs),
                    static_cast<unsigned long long>(elapsedMs(compileStart)),
                    static_cast<unsigned>(compileSource.size()),
                    static_cast<unsigned>(hlslSource.size()),
                    static_cast<unsigned>(outDXBC.size()));
        return finishInFlightDXBCCompile(cacheKey, inFlightCompile, true, outDXBC);
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
        CC_LOG_INFO("[D3D12-PERF] ShaderStageCompileCacheStore name='%s' stage=%s profile=%s backend=%s dxbcBytes=%u cacheKey=%s",
                    shaderName.c_str(), getShaderStageName(stage), profile, cacheStoreBackend,
                    static_cast<unsigned>(outDXBC.size()), cacheKey.c_str());
        CC_LOG_INFO("[D3D12-PERF] ShaderStageCompile name='%s' stage=%s profile=%s glslToSpirvMs=%llu spirvToHlslMs=%llu hlslToDxbcMs=%llu totalMs=%llu glslBytes=%u hlslBytes=%u dxbcBytes=%u fallbackEntries=%u",
                    shaderName.c_str(), getShaderStageName(stage), profile,
                    static_cast<unsigned long long>(glslToSpirvMs),
                    static_cast<unsigned long long>(spirvToHlslMs),
                    static_cast<unsigned long long>(hlslToDxbcMs),
                    static_cast<unsigned long long>(elapsedMs(compileStart)),
                    static_cast<unsigned>(compileSource.size()),
                    static_cast<unsigned>(hlslSource.size()),
                    static_cast<unsigned>(outDXBC.size()),
                    static_cast<unsigned>(extraFallbackEntryCandidates.size()));
        return finishInFlightDXBCCompile(cacheKey, inFlightCompile, true, outDXBC);
    }

    hlslToDxbcMs = elapsedMs(hlslToDxbcStart);
    CC_LOG_INFO("[D3D12-PERF] ShaderStageCompileFailed name='%s' stage=%s profile=%s glslToSpirvMs=%llu spirvToHlslMs=%llu hlslToDxbcMs=%llu totalMs=%llu glslBytes=%u hlslBytes=%u",
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
    return finishInFlightDXBCCompile(cacheKey, inFlightCompile, false, outDXBC);
}

void CCD3D12Shader::scheduleStagePrecompile(ShaderStageFlagBit stage) {
    if (!_impl) {
        return;
    }

    auto *stageRecord = _impl->getStageRecord(stage);
    if (!stageRecord || stageRecord->source.empty() || stageRecord->failed || stageRecord->precompileStarted) {
        return;
    }
    if (!shouldPrecompileD3D12ShaderStage(stageRecord->shaderName, stage)) {
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

    stageRecord->precompileFuture = std::async(std::launch::async,
                                               [this, stage, source, entry, shaderName]() {
                                                   const auto precompileStart = D3D12PerfClock::now();
                                                   std::vector<uint8_t> compiledDXBC;
                                                   const bool ok = compileGLSLToDXBC(stage, source, entry, shaderName, compiledDXBC);

                                                   uint64_t reflectMs = 0;
                                                   {
                                                       std::lock_guard<std::mutex> lock(_impl->compileMutex);
                                                       auto *record = _impl->getStageRecord(stage);
                                                       auto *buffer = _impl->getDXBCBuffer(stage);
                                                       if (ok && buffer) {
                                                           *buffer = std::move(compiledDXBC);
                                                           if (stage == ShaderStageFlagBit::VERTEX && !buffer->empty()) {
                                                               const auto reflectStart = D3D12PerfClock::now();
                                                               _impl->vertexInputSignature.clear();
                                                               reflectVertexInputSignature(*buffer, _impl->vertexInputSignature);
                                                               reflectMs = elapsedMs(reflectStart);
                                                           }
                                                       } else if (record) {
                                                           record->failed = true;
                                                       }
                                                   }

                                                   CC_LOG_INFO("[D3D12-PERF] ShaderStagePrecompile name='%s' stage=%s ok=%u sourceBytes=%u reflectMs=%llu totalMs=%llu",
                                                               shaderName.c_str(), getShaderStageName(stage), ok ? 1U : 0U,
                                                               static_cast<unsigned>(source.size()),
                                                               static_cast<unsigned long long>(reflectMs),
                                                               static_cast<unsigned long long>(elapsedMs(precompileStart)));
                                                   return ok;
                                               }).share();
}

void CCD3D12Shader::waitForBackgroundCompiles() {
    if (!_impl) {
        return;
    }

    ccstd::vector<std::shared_future<bool>> futures;
    for (ShaderStageFlagBit stage : {
             ShaderStageFlagBit::VERTEX,
             ShaderStageFlagBit::FRAGMENT,
             ShaderStageFlagBit::GEOMETRY,
             ShaderStageFlagBit::COMPUTE,
             ShaderStageFlagBit::CONTROL,
             ShaderStageFlagBit::EVALUATION,
         }) {
        auto *record = _impl->getStageRecord(stage);
        if (record && record->precompileFuture.valid()) {
            futures.emplace_back(record->precompileFuture);
        }
    }

    for (auto &future : futures) {
        try {
            future.wait();
        } catch (...) {
        }
    }
}

void CCD3D12Shader::doInit(const ShaderInfo &info) {
    const auto initStart = D3D12PerfClock::now();
    _impl->vertexInputSignature.clear();

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
            dxbcBuffer->resize(stage.source.size());
            memcpy(dxbcBuffer->data(), stage.source.data(), stage.source.size());

            uint64_t reflectMs = 0;
            if (stage.stage == ShaderStageFlagBit::VERTEX && !dxbcBuffer->empty()) {
                const auto reflectStart = D3D12PerfClock::now();
                reflectVertexInputSignature(*dxbcBuffer, _impl->vertexInputSignature);
                reflectMs = elapsedMs(reflectStart);
            }

            CC_LOG_INFO("[D3D12-PERF] ShaderStageInit name='%s' stage=%s source=DXBC sourceBytes=%u dxbcBytes=%u reflectMs=%llu totalMs=%llu",
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
            CC_LOG_INFO("[D3D12-PERF] ShaderStageInit name='%s' stage=%s source=GLSL-LAZY sourceBytes=%u dxbcBytes=0 reflectMs=0 totalMs=%llu",
                        info.name.c_str(), getShaderStageName(stage.stage),
                        static_cast<unsigned>(stage.source.size()),
                        static_cast<unsigned long long>(elapsedMs(stageInitStart)));
        }
    }

    CC_LOG_INFO("D3D12Shader '%s' initialized with %u stages.",
                info.name.c_str(), static_cast<unsigned>(_stages.size()));
    CC_LOG_INFO("[D3D12-PERF] ShaderInit name='%s' stages=%u totalMs=%llu",
                info.name.c_str(), static_cast<unsigned>(_stages.size()),
                static_cast<unsigned long long>(elapsedMs(initStart)));
}

void CCD3D12Shader::doDestroy() {
    if (_impl) {
        waitForBackgroundCompiles();
        _impl->vertexDXBC.clear();
        _impl->fragmentDXBC.clear();
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

bool CCD3D12Shader::ensureStageBytecode(ShaderStageFlagBit stage) const {
    if (!_impl) {
        return false;
    }

    auto *dxbcBuffer = _impl->getDXBCBuffer(stage);
    auto *stageRecord = _impl->getStageRecord(stage);
    if (!dxbcBuffer) {
        return false;
    }
    if (!dxbcBuffer->empty()) {
        return true;
    }
    if (!stageRecord || stageRecord->source.empty() || stageRecord->failed) {
        return false;
    }

    std::shared_future<bool> precompileFuture = stageRecord->precompileFuture;
    if (precompileFuture.valid()) {
        const auto waitStart = D3D12PerfClock::now();
        bool ok = false;
        try {
            ok = precompileFuture.get();
        } catch (...) {
            ok = false;
        }
        CC_LOG_INFO("[D3D12-PERF] ShaderStagePrecompileWait name='%s' stage=%s ok=%u waitMs=%llu",
                    stageRecord->shaderName.c_str(), getShaderStageName(stage), ok ? 1U : 0U,
                    static_cast<unsigned long long>(elapsedMs(waitStart)));
        return ok && !dxbcBuffer->empty();
    }

    {
        std::lock_guard<std::mutex> lock(_impl->compileMutex);
        if (!dxbcBuffer->empty()) {
            return true;
        }
        if (stageRecord->failed) {
            return false;
        }
    }

    const auto lazyStart = D3D12PerfClock::now();
    std::vector<uint8_t> compiledDXBC;
    const bool ok = compileGLSLToDXBC(stage,
                                      stageRecord->source,
                                      stageRecord->entry,
                                      stageRecord->shaderName,
                                      compiledDXBC);
    if (!ok) {
        std::lock_guard<std::mutex> lock(_impl->compileMutex);
        stageRecord->failed = true;
        dxbcBuffer->clear();
        CC_LOG_ERROR("D3D12Shader '%s': lazy GLSL->DXBC failed for stage 0x%x.",
                     stageRecord->shaderName.c_str(), static_cast<unsigned>(stage));
        return false;
    }

    uint64_t reflectMs = 0;
    {
        std::lock_guard<std::mutex> lock(_impl->compileMutex);
        *dxbcBuffer = std::move(compiledDXBC);
        if (stage == ShaderStageFlagBit::VERTEX && !dxbcBuffer->empty()) {
            const auto reflectStart = D3D12PerfClock::now();
            _impl->vertexInputSignature.clear();
            reflectVertexInputSignature(*dxbcBuffer, _impl->vertexInputSignature);
            reflectMs = elapsedMs(reflectStart);
        }
    }

    CC_LOG_INFO("[D3D12-PERF] ShaderStageLazyCompile name='%s' stage=%s sourceBytes=%u dxbcBytes=%u reflectMs=%llu totalMs=%llu",
                stageRecord->shaderName.c_str(), getShaderStageName(stage),
                static_cast<unsigned>(stageRecord->source.size()),
                static_cast<unsigned>(dxbcBuffer->size()),
                static_cast<unsigned long long>(reflectMs),
                static_cast<unsigned long long>(elapsedMs(lazyStart)));
    return true;
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getVertexBytecode() const {
    ensureStageBytecode(ShaderStageFlagBit::VERTEX);
    if (_impl && !_impl->vertexDXBC.empty())
        return {_impl->vertexDXBC.data(), _impl->vertexDXBC.size()};
    return {};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getFragmentBytecode() const {
    ensureStageBytecode(ShaderStageFlagBit::FRAGMENT);
    if (_impl && !_impl->fragmentDXBC.empty())
        return {_impl->fragmentDXBC.data(), _impl->fragmentDXBC.size()};
    return {};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getGeometryBytecode() const {
    ensureStageBytecode(ShaderStageFlagBit::GEOMETRY);
    if (_impl && !_impl->geometryDXBC.empty())
        return {_impl->geometryDXBC.data(), _impl->geometryDXBC.size()};
    return {};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getComputeBytecode() const {
    ensureStageBytecode(ShaderStageFlagBit::COMPUTE);
    if (_impl && !_impl->computeDXBC.empty())
        return {_impl->computeDXBC.data(), _impl->computeDXBC.size()};
    return {};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getHullBytecode() const {
    ensureStageBytecode(ShaderStageFlagBit::CONTROL);
    if (_impl && !_impl->hullDXBC.empty())
        return {_impl->hullDXBC.data(), _impl->hullDXBC.size()};
    return {};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getDomainBytecode() const {
    ensureStageBytecode(ShaderStageFlagBit::EVALUATION);
    if (_impl && !_impl->domainDXBC.empty())
        return {_impl->domainDXBC.data(), _impl->domainDXBC.size()};
    return {};
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
    ensureStageBytecode(ShaderStageFlagBit::VERTEX);
    return _impl ? _impl->vertexInputSignature : empty;
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
