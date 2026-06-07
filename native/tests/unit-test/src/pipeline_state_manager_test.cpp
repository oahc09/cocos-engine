#include "gtest/gtest.h"

#include "renderer/pipeline/PipelineStateManager.h"

namespace cc::pipeline {

TEST(PipelineStateManagerTest, DistinguishesKeysThatCollideUnderLegacyXorHash) {
    const PipelineStateKey billboard{
        0x10U,
        0x20U,
        0x40U,
        0x80U,
        0U,
    };
    const PipelineStateKey stretched{
        0x11U,
        0x20U,
        0x41U,
        0x80U,
        0U,
    };

    EXPECT_EQ(
        billboard.passHash ^ billboard.renderPassHash ^ billboard.iaHash ^ billboard.shaderID,
        stretched.passHash ^ stretched.renderPassHash ^ stretched.iaHash ^ stretched.shaderID);
    EXPECT_NE(billboard, stretched);

    ccstd::unordered_map<PipelineStateKey, uint32_t, PipelineStateKeyHasher> cache;
    cache.emplace(billboard, 1U);
    cache.emplace(stretched, 2U);

    EXPECT_EQ(cache.size(), 2U);
    EXPECT_EQ(cache.at(billboard), 1U);
    EXPECT_EQ(cache.at(stretched), 2U);
}

} // namespace cc::pipeline
