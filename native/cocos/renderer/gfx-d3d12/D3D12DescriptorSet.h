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

#pragma once

#include "gfx-base/GFXDescriptorSet.h"
#include <memory>

namespace cc {
namespace gfx {

class CC_DLL CCD3D12DescriptorSet final : public DescriptorSet {
public:
    CCD3D12DescriptorSet();
    ~CCD3D12DescriptorSet() override;

    void update() override;
    void forceUpdate() override;
    void *getCbvSrvUavDescriptorHeap() const;
    void *getSamplerDescriptorHeap() const;
    uint64_t getCbvSrvUavCPUDescriptorHandle() const;
    uint64_t getSamplerCPUDescriptorHandle() const;
    uint32_t getCbvSrvUavDescriptorCount() const;
    // The local b0 root CBV may occur anywhere in the CPU staging table. The
    // returned static-table count excludes that one descriptor and is packed
    // in original descriptor order by CommandBuffer.
    bool getCbvSrvUavPartition(uint32_t &rootCbvDescriptorOffset,
                               uint32_t &staticTableDescriptorCount) const;
    bool canReuseStaticCbvSrvUavResources() const;
    bool hasMatchingStaticCbvSrvUavResources(const CCD3D12DescriptorSet &other) const;
    uint32_t getSamplerDescriptorCount() const;
    uint64_t getVersion() const;
    uint64_t getStaticDescriptorVersion() const;
    uint32_t getUniformDescriptorSlotCount() const;
    uint32_t getDynamicDescriptorSlotCount() const;
    bool getDynamicDescriptorOffset(uint32_t index, uint32_t &descriptorOffset) const;
    bool getDynamicDescriptorSource(uint32_t index, uint64_t &gpuAddress, uint64_t &size) const;
    bool hasOnlyNullDynamicDescriptorSources() const;
    bool getUniformDescriptorSignature(uint32_t index, uint32_t &descriptorOffset,
                                       uint64_t &gpuAddress, uint32_t &size) const;
    void getStaticDescriptorAnalysis(uint32_t &dynamicCbvDescriptors,
                                     uint32_t &staticTextureDescriptors,
                                     uint32_t &staticCbvSrvUavDescriptors,
                                     uint64_t &staticSignature) const;
    uint32_t getDescriptorSemanticCount() const;
    bool getDescriptorSemanticSignature(uint32_t index, uint32_t &kind,
                                        uint64_t &value0, uint64_t &value1) const;
    const ccstd::vector<uint32_t> &getSamplerTableKey() const;
    void applyDynamicOffsets(uint32_t dynamicOffsetCount, const uint32_t *dynamicOffsets);
    void restoreDynamicOffsetDescriptors();
    void updateForLocalRootCbv(bool skipStaticCbvStaging = false);

protected:
    void doInit(const DescriptorSetInfo &info) override;
    void doDestroy() override;

private:
    void refreshStaticDescriptorMetadata();

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace gfx
} // namespace cc
