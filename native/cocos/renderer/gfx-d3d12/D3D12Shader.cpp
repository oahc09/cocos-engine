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
#include "base/Log.h"

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

#include <cstdio>
#include <cstdarg>
#include <regex>

namespace cc {
namespace gfx {

namespace {
void shaderDiagLog(const char *fmt, ...) {
    static FILE *s_file = nullptr;
    if (!s_file) {
        s_file = fopen("C:\\temp\\d3d12-render-diag.log", "a");
        if (!s_file) return;
        setvbuf(s_file, nullptr, _IONBF, 0); // unbuffered
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(s_file, fmt, args);
    va_end(args);
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

// Ensure glslang process-level init happens exactly once
static bool s_glslangInitialized = false;
void ensureGlslangInit() {
    if (!s_glslangInitialized) {
        glslang::InitializeProcess();
        s_glslangInitialized = true;
        shaderDiagLog("[SHADER] glslang::InitializeProcess() done.\n");
    }
}

} // anonymous namespace

struct CCD3D12Shader::Impl {
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
                                       std::vector<uint8_t> &outDXBC) {
    ensureGlslangInit();

    // ============================================================
    // Step 0: Prepend #version 450 (same as Vulkan/Metal/WGPU backends)
    // The glsl4 source from EffectAsset does NOT contain #version;
    // each desktop backend must prepend it at runtime.
    // ============================================================
    ccstd::string processedSource = "#version 450\n" + glslSource;

    // ============================================================
    // Step 1: GLSL -> SPIR-V (using glslang directly, with error checks)
    // ============================================================

    shaderDiagLog("[SHADER] Step0: prepended #version 450 (source was %zu bytes, now %zu bytes)\n",
                  glslSource.size(), processedSource.size());

    // Dump first two lines for diagnostics
    {
        auto p1 = processedSource.find('\n');
        auto p2 = (p1 != ccstd::string::npos) ? processedSource.find('\n', p1 + 1) : ccstd::string::npos;
        ccstd::string firstTwo = (p2 != ccstd::string::npos) ? processedSource.substr(0, p2) : processedSource.substr(0, 160);
        shaderDiagLog("[SHADER] Step0: first two lines: '%s'\n", firstTwo.c_str());
    }

    EShLanguage eshStage = toEShLanguage(stage);
    const char *sourcePtr = processedSource.c_str();

    shaderDiagLog("[SHADER] Step1: GLSL->SPIR-V begin (source=%zu bytes, stage=0x%x)\n",
                  processedSource.size(), static_cast<unsigned>(stage));

    glslang::TShader shader(eshStage);
    shader.setStrings(&sourcePtr, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, eshStage, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);

    auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

    bool parseOK = shader.parse(&glslang::DefaultTBuiltInResource, 450, false, messages);
    if (!parseOK) {
        shaderDiagLog("[SHADER] Step1: GLSL PARSE FAILED:\n%s\n%s\n",
                      shader.getInfoLog(), shader.getInfoDebugLog());
        CC_LOG_ERROR("D3D12Shader: GLSL parse failed:\n%s\n%s",
                     shader.getInfoLog(), shader.getInfoDebugLog());
        return false;
    }
    shaderDiagLog("[SHADER] Step1: GLSL parse OK.\n");

    glslang::TProgram program;
    program.addShader(&shader);

    bool linkOK = program.link(messages);
    if (!linkOK) {
        shaderDiagLog("[SHADER] Step1: GLSL LINK FAILED:\n%s\n%s\n",
                      program.getInfoLog(), program.getInfoDebugLog());
        CC_LOG_ERROR("D3D12Shader: GLSL link failed:\n%s\n%s",
                     program.getInfoLog(), program.getInfoDebugLog());
        return false;
    }
    shaderDiagLog("[SHADER] Step1: GLSL link OK.\n");

    auto *intermediate = program.getIntermediate(eshStage);
    if (!intermediate) {
        shaderDiagLog("[SHADER] Step1: getIntermediate returned NULL!\n");
        CC_LOG_ERROR("D3D12Shader: glslang getIntermediate() returned null.");
        return false;
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
        shaderDiagLog("[SHADER] Step1: SPIR-V output is empty!\n");
        CC_LOG_ERROR("D3D12Shader: GlslangToSpv produced no output.");
        return false;
    }

    shaderDiagLog("[SHADER] Step1: SPIR-V OK (%zu words, %zu bytes).\n",
                  spirvOutput.size(), spirvOutput.size() * sizeof(uint32_t));

    // ============================================================
    // Step 2: SPIR-V -> HLSL (using SPIRV-Cross)
    // ============================================================
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

            shaderDiagLog("[SHADER] Step2: resource '%s' id=%u set=%u binding=%u -> space=%u reg=%u\n",
                          res.name.c_str(), res.id, set, binding, set, binding);

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

        shaderDiagLog("[SHADER] Step2: configured %u ubo, %u ssbo, %u sampled, %u sep_img, %u sep_samp, %u img, %u subpass bindings\n",
                      resources.uniform_buffers.size(), resources.storage_buffers.size(),
                      resources.sampled_images.size(), resources.separate_images.size(),
                      resources.separate_samplers.size(), resources.storage_images.size(),
                      resources.subpass_inputs.size());
    }

    std::string hlslSource;
    try {
        hlslSource = hlslCompiler.compile();
    } catch (const spirv_cross::CompilerError &e) {
        CC_LOG_ERROR("D3D12Shader: SPIR-V -> HLSL failed: %s", e.what());
        shaderDiagLog("[SHADER] Step2: SPIR-V->HLSL FAILED: %s\n", e.what());
        return false;
    }

    shaderDiagLog("[SHADER] Step2: HLSL OK (%zu chars).\n", hlslSource.size());

    // Dump HLSL for diagnostics (first 2000 chars)
    {
        ccstd::string hlslDump = hlslSource.substr(0, std::min(hlslSource.size(), (size_t)2000));
        shaderDiagLog("[SHADER] Step2: HLSL dump (first 2000 chars):\n%s\n[END HLSL DUMP]\n", hlslDump.c_str());
    }

    // ============================================================
    // Step 3: HLSL -> DXBC (using D3DCompile)
    // ============================================================
    const char *profile = getHLSLProfile(stage);
    const auto entryCandidates = collectHLSLEntryCandidates(hlslSource, entryName);

    UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if !defined(NDEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    shaderDiagLog("[SHADER] Step3: candidate entries for stage 0x%x:", static_cast<unsigned>(stage));
    for (const auto &candidate : entryCandidates) {
        shaderDiagLog(" %s", candidate.c_str());
    }
    shaderDiagLog("\n");

    HRESULT hr = E_FAIL;
    ccstd::string compileErrors;
    for (const auto &candidate : entryCandidates) {
        shaderDiagLog("[SHADER] Step3: compiling entry='%s' profile='%s'\n", candidate.c_str(), profile);

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
            shaderDiagLog("[SHADER] Step3: DXBC OK (%zu bytes) for stage 0x%x with entry='%s'. Pipeline complete!\n",
                          outDXBC.size(), static_cast<unsigned>(stage), candidate.c_str());
            return true;
        }

        if (errorBlob) {
            compileErrors = static_cast<const char *>(errorBlob->GetBufferPointer());
            shaderDiagLog("[SHADER] Step3: entry='%s' FAILED: %s\n", candidate.c_str(), compileErrors.c_str());
            errorBlob->Release();
        } else {
            compileErrors.clear();
            shaderDiagLog("[SHADER] Step3: entry='%s' FAILED without compiler diagnostics. HRESULT=0x%08x\n",
                          candidate.c_str(), static_cast<unsigned>(hr));
        }
        if (codeBlob) {
            codeBlob->Release();
        }
    }

    if (!compileErrors.empty()) {
        CC_LOG_ERROR("D3D12Shader: HLSL -> DXBC failed: %s", compileErrors.c_str());
        shaderDiagLog("[SHADER] Step3: HLSL->DXBC FAILED after trying all entries: %s\n", compileErrors.c_str());
    } else {
        CC_LOG_ERROR("D3D12Shader: HLSL -> DXBC failed. HRESULT=0x%08x", static_cast<unsigned>(hr));
        shaderDiagLog("[SHADER] Step3: HLSL->DXBC FAILED after trying all entries. HRESULT=0x%08x\n",
                      static_cast<unsigned>(hr));
    }
    return false;
}

void CCD3D12Shader::doInit(const ShaderInfo &info) {
    for (const auto &stage : _stages) {
        std::vector<uint8_t> *dxbcBuffer = _impl->getDXBCBuffer(stage.stage);
        ccstd::string *entryName = nullptr;

        if (stage.stage == ShaderStageFlagBit::VERTEX) {
            entryName = &_impl->vertexEntry;
        } else if (stage.stage == ShaderStageFlagBit::FRAGMENT) {
            entryName = &_impl->fragmentEntry;
        }

        if (!dxbcBuffer) continue;

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
            shaderDiagLog("[SHADER] '%s' stage 0x%x: precompiled DXBC (%zu bytes)\n",
                          info.name.c_str(), static_cast<unsigned>(stage.stage), stage.source.size());
        } else {
            ccstd::string entry = entryName ? *entryName : getSPIRVCrossEntryName(stage.stage);

            shaderDiagLog("[SHADER] '%s' stage 0x%x: compiling GLSL (%zu bytes)...\n",
                          info.name.c_str(), static_cast<unsigned>(stage.stage), stage.source.size());

            bool ok = compileGLSLToDXBC(stage.stage, stage.source, entry, *dxbcBuffer);

            if (!ok) {
                CC_LOG_WARNING("D3D12Shader '%s': GLSL->DXBC failed for stage 0x%x, will use fallback.",
                               info.name.c_str(), static_cast<unsigned>(stage.stage));
                shaderDiagLog("[SHADER] '%s' stage 0x%x: compilation FAILED, will fallback\n",
                              info.name.c_str(), static_cast<unsigned>(stage.stage));
                dxbcBuffer->clear();
            }
        }
    }

    CC_LOG_INFO("D3D12Shader '%s' initialized with %u stages.",
                info.name.c_str(), static_cast<unsigned>(_stages.size()));
}

void CCD3D12Shader::doDestroy() {
    if (_impl) {
        _impl->vertexDXBC.clear();
        _impl->fragmentDXBC.clear();
        _impl->geometryDXBC.clear();
        _impl->computeDXBC.clear();
        _impl->hullDXBC.clear();
        _impl->domainDXBC.clear();
    }
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getVertexBytecode() const {
    if (_impl && !_impl->vertexDXBC.empty())
        return {_impl->vertexDXBC.data(), _impl->vertexDXBC.size()};
    return {};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getFragmentBytecode() const {
    if (_impl && !_impl->fragmentDXBC.empty())
        return {_impl->fragmentDXBC.data(), _impl->fragmentDXBC.size()};
    return {};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getGeometryBytecode() const {
    if (_impl && !_impl->geometryDXBC.empty())
        return {_impl->geometryDXBC.data(), _impl->geometryDXBC.size()};
    return {};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getComputeBytecode() const {
    if (_impl && !_impl->computeDXBC.empty())
        return {_impl->computeDXBC.data(), _impl->computeDXBC.size()};
    return {};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getHullBytecode() const {
    if (_impl && !_impl->hullDXBC.empty())
        return {_impl->hullDXBC.data(), _impl->hullDXBC.size()};
    return {};
}

CCD3D12Shader::BytecodeBlob CCD3D12Shader::getDomainBytecode() const {
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
