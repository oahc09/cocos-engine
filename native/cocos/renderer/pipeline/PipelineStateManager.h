/****************************************************************************
 Copyright (c) 2020-2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
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

#pragma once

#include "cocos/base/Ptr.h"
#include "cocos/base/std/hash/hash.h"
#include "gfx-base/GFXDef.h"

namespace cc {
namespace scene {
class Pass;
}
namespace pipeline {

struct PipelineStateKey {
    ccstd::hash_t passHash{0};
    ccstd::hash_t renderPassHash{0};
    ccstd::hash_t iaHash{0};
    uint32_t shaderID{0};
    uint32_t subpass{0};

    bool operator==(const PipelineStateKey &other) const {
        return passHash == other.passHash &&
               renderPassHash == other.renderPassHash &&
               iaHash == other.iaHash &&
               shaderID == other.shaderID &&
               subpass == other.subpass;
    }

    bool operator!=(const PipelineStateKey &other) const {
        return !(*this == other);
    }
};

struct PipelineStateKeyHasher {
    ccstd::hash_t operator()(const PipelineStateKey &key) const {
        ccstd::hash_t hash{0};
        ccstd::hash_combine(hash, key.passHash);
        ccstd::hash_combine(hash, key.renderPassHash);
        ccstd::hash_combine(hash, key.iaHash);
        ccstd::hash_combine(hash, key.shaderID);
        ccstd::hash_combine(hash, key.subpass);
        return hash;
    }
};

class CC_DLL PipelineStateManager {
public:
    static gfx::PipelineState *getOrCreatePipelineState(const scene::Pass *pass,
                                                        gfx::Shader *shader,
                                                        gfx::InputAssembler *inputAssembler,
                                                        gfx::RenderPass *renderPass,
                                                        uint32_t subpass = 0);
    static void destroyAll();

private:
    static ccstd::unordered_map<PipelineStateKey, IntrusivePtr<gfx::PipelineState>, PipelineStateKeyHasher> psoHashMap;
};

} // namespace pipeline
} // namespace cc
