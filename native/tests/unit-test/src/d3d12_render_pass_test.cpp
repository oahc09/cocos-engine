#include "gtest/gtest.h"

#if CC_USE_D3D12

    #include "cocos/renderer/gfx-d3d12/D3D12RenderPass.h"
    #include "cocos/renderer/gfx-d3d12/D3D12Device.h"
    #include "cocos/renderer/gfx-d3d12/D3D12Texture.h"

namespace cc {
namespace gfx {

TEST(D3D12RenderPassTest, UsesOnlyCurrentSubpassColorAttachmentsForPipelineState) {
    RenderPassInfo info;
    info.colorAttachments.resize(2);
    info.colorAttachments[0].format = Format::RGBA8;
    info.colorAttachments[0].sampleCount = SampleCount::X4;
    info.colorAttachments[1].format = Format::RGBA8;
    info.colorAttachments[1].sampleCount = SampleCount::X1;

    auto &subpass = info.subpasses.emplace_back();
    subpass.colors.emplace_back(0);
    subpass.resolves = {1};

    CCD3D12RenderPass renderPass;
    renderPass.initialize(info);

    const auto formats = renderPass.getRTVFormats(0);
    ASSERT_EQ(formats.size(), 1U);
    EXPECT_EQ(formats[0], renderPass.getRTVFormats()[0]);
    EXPECT_EQ(renderPass.getSampleCount(0), 4U);
}

TEST(D3D12RenderPassTest, SelectsSampleCountFromRequestedSubpass) {
    RenderPassInfo info;
    info.colorAttachments.resize(2);
    info.colorAttachments[0].format = Format::RGBA8;
    info.colorAttachments[0].sampleCount = SampleCount::X4;
    info.colorAttachments[1].format = Format::RGBA8;
    info.colorAttachments[1].sampleCount = SampleCount::X1;

    info.subpasses.emplace_back().colors.emplace_back(0);
    info.subpasses.emplace_back().colors.emplace_back(1);

    CCD3D12RenderPass renderPass;
    renderPass.initialize(info);

    EXPECT_EQ(renderPass.getSampleCount(0), 4U);
    EXPECT_EQ(renderPass.getSampleCount(1), 1U);
}

TEST(D3D12RenderPassTest, RecordsAndSubmitsColorResolve) {
    auto *device = CCD3D12Device::getInstance();
    ASSERT_NE(device, nullptr);

    TextureInfo multisampleInfo;
    multisampleInfo.type = TextureType::TEX2D;
    multisampleInfo.usage = TextureUsageBit::COLOR_ATTACHMENT;
    multisampleInfo.format = Format::RGBA8;
    multisampleInfo.width = 16;
    multisampleInfo.height = 16;
    multisampleInfo.samples = SampleCount::X4;

    TextureInfo resolveInfo = multisampleInfo;
    resolveInfo.samples = SampleCount::X1;
    auto *multisampleTexture = device->createTexture(multisampleInfo);
    auto *resolveTexture = device->createTexture(resolveInfo);
    ASSERT_NE(multisampleTexture, nullptr);
    ASSERT_NE(resolveTexture, nullptr);

    RenderPassInfo renderPassInfo;
    renderPassInfo.colorAttachments.resize(2);
    renderPassInfo.colorAttachments[0].format = Format::RGBA8;
    renderPassInfo.colorAttachments[0].sampleCount = SampleCount::X4;
    renderPassInfo.colorAttachments[0].loadOp = LoadOp::CLEAR;
    renderPassInfo.colorAttachments[0].storeOp = StoreOp::DISCARD;
    renderPassInfo.colorAttachments[1].format = Format::RGBA8;
    renderPassInfo.colorAttachments[1].sampleCount = SampleCount::X1;
    renderPassInfo.colorAttachments[1].loadOp = LoadOp::DISCARD;
    renderPassInfo.colorAttachments[1].storeOp = StoreOp::STORE;
    auto &subpass = renderPassInfo.subpasses.emplace_back();
    subpass.colors = {0};
    subpass.resolves = {1};

    auto *renderPass = device->createRenderPass(renderPassInfo);
    auto *framebuffer = device->createFramebuffer({renderPass, {multisampleTexture, resolveTexture}});
    ASSERT_NE(renderPass, nullptr);
    ASSERT_NE(framebuffer, nullptr);

    auto *commandBuffer = device->getCommandBuffer();
    ASSERT_NE(commandBuffer, nullptr);
    ColorList clearColors(2);
    clearColors[0] = {0.F, 0.F, 0.F, 1.F};
    clearColors[1] = {0.F, 0.F, 0.F, 1.F};
    commandBuffer->begin();
    commandBuffer->beginRenderPass(renderPass, framebuffer, {0, 0, 16, 16}, clearColors, 1.F, 0);
    commandBuffer->endRenderPass();
    commandBuffer->end();
    CommandBuffer *commandBuffers[] = {commandBuffer};
    device->getQueue()->submit(commandBuffers, 1);

    EXPECT_EQ(static_cast<CCD3D12Texture *>(multisampleTexture)->getCurrentState(),
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    EXPECT_EQ(static_cast<CCD3D12Texture *>(resolveTexture)->getCurrentState(),
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    framebuffer->destroy();
    delete framebuffer;
    renderPass->destroy();
    delete renderPass;
    multisampleTexture->destroy();
    delete multisampleTexture;
    resolveTexture->destroy();
    delete resolveTexture;
}

} // namespace gfx
} // namespace cc

#endif
