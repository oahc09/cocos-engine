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

class CCD3D12Buffer;
// The built-in standard local set currently declares five ordinary uniform
// buffers. Keep enough room for that layout while staying well below the
// D3D12 64-DWORD root-signature limit (each root CBV costs two DWORDs).
static constexpr uint32_t D3D12_MAX_LOCAL_ROOT_CBVS = 8;

struct D3D12LocalRootCbvBatchData {
    CCD3D12Buffer *rootBuffers[D3D12_MAX_LOCAL_ROOT_CBVS]{};
    uint64_t gpuAddresses[D3D12_MAX_LOCAL_ROOT_CBVS]{};
    uint32_t sizes[D3D12_MAX_LOCAL_ROOT_CBVS]{};
    uint32_t rootCbvCount{0};
    uint64_t staticSignature{0};
    uint64_t samplerSignature{0};
};

struct D3D12LocalRootCbvFastPacket {
    uint64_t gpuAddress{0};
    uint64_t staticSignature{0};
    uint64_t samplerSignature{0};
};

struct D3D12LocalRootCbvPreparedPacket {
    const uint64_t *gpuAddressStorage{nullptr};
    uint64_t staticSignature{0};
    uint64_t samplerSignature{0};
};

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
    // Local ordinary-uniform Root CBVs may occur anywhere in the CPU staging table. The
    // returned static-table count excludes those descriptors and is packed
    // in original descriptor order by CommandBuffer.
    bool getCbvSrvUavPartition(uint32_t &rootCbvCount,
                               uint32_t &staticTableDescriptorCount) const;
    bool getLocalRootCbvDescriptorOffset(uint32_t index, uint32_t &descriptorOffset) const;
    bool canReuseStaticCbvSrvUavResources() const;
    bool hasMatchingStaticCbvSrvUavResources(const CCD3D12DescriptorSet &other) const;
    uint32_t getSamplerDescriptorCount() const;
    uint64_t getVersion() const;
    uint64_t getIdentity() const;
    uint64_t getStaticDescriptorVersion() const;
    uint32_t getUniformDescriptorSlotCount() const;
    uint32_t getDynamicDescriptorSlotCount() const;
    bool getDynamicDescriptorOffset(uint32_t index, uint32_t &descriptorOffset) const;
    bool getDynamicDescriptorSource(uint32_t index, uint64_t &gpuAddress, uint64_t &size) const;
    bool hasOnlyNullDynamicDescriptorSources() const;
    bool getLocalRootCbvData(D3D12LocalRootCbvBatchData &data) const;
    bool getLocalRootCbvBatchData(D3D12LocalRootCbvBatchData &data);
    bool getLocalRootCbvBatchFastData(D3D12LocalRootCbvBatchData &data) const;
    bool getLocalRootCbvFastPacket(D3D12LocalRootCbvFastPacket &packet) const;
    CC_FORCE_INLINE const D3D12LocalRootCbvPreparedPacket *
    getLocalRootCbvPreparedPacket() const {
        return !_isDirty && _localRootCbvPreparedPacket.gpuAddressStorage
                   ? &_localRootCbvPreparedPacket
                   : nullptr;
    }
    bool getUniformDescriptorSignature(uint32_t index, uint32_t &descriptorOffset,
                                       uint64_t &gpuAddress, uint32_t &size) const;
    void getStaticDescriptorAnalysis(uint32_t &dynamicCbvDescriptors,
                                     uint32_t &staticTextureDescriptors,
                                     uint32_t &staticCbvSrvUavDescriptors,
                                     uint64_t &staticSignature) const;
    uint32_t getDescriptorSemanticCount() const;
    bool getDescriptorSemanticSignature(uint32_t index, uint32_t &kind,
                                        uint64_t &value0, uint64_t &value1) const;
    void collectBoundD3D12Resources(ccstd::vector<void *> &resources) const;
    const ccstd::vector<uint32_t> &getSamplerTableKey() const;
    uint64_t getSamplerSignature() const;
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
    D3D12LocalRootCbvPreparedPacket _localRootCbvPreparedPacket;
};

} // namespace gfx
} // namespace cc
