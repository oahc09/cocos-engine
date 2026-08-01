from pathlib import Path


ROOT = Path(__file__).resolve().parent


def read(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def test_restored_revision_fast_paths() -> None:
    buffer_h = read("D3D12Buffer.h")
    buffer_cpp = read("D3D12Buffer.cpp")
    ia_cpp = read("D3D12InputAssembler.cpp")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")

    assert "getD3D12GlobalResourceGeneration" in buffer_h
    assert "BUFFER_RESOURCE_GENERATION" in buffer_cpp
    assert "observedBufferResourceGeneration" in ia_cpp
    refresh = function_body(ia_cpp, "bool CCD3D12InputAssembler::refreshBufferViews")
    assert refresh.index("observedBufferResourceGeneration") < refresh.index("for (size_t i = 0;")

    assert "onlyNullDynamicDescriptorSources" in descriptor_cpp
    null_sources = function_body(
        descriptor_cpp,
        "bool CCD3D12DescriptorSet::hasOnlyNullDynamicDescriptorSources",
    )
    assert "dynamicDescriptorSlots" not in null_sources

    local_update = function_body(
        descriptor_cpp,
        "void CCD3D12DescriptorSet::updateForLocalRootCbv",
    )
    assert local_update.index("observedTransientUniformUploadGeneration") < local_update.index(
        "for (auto &slot"
    )


def test_legacy_shadow_depth_source_is_normalized_before_cache_lookup() -> None:
    shader_cpp = read("D3D12Shader.cpp")
    rewrite = function_body(
        shader_cpp,
        "D3D12LegacyShadowSourceRewrite rewriteD3D12LegacyShadowClipDepth",
    )
    compile_shader = function_body(shader_cpp, "bool CCD3D12Shader::compileGLSLToDXBC")

    assert "shadowPosWithDepthBias.w * 0.5 + 0.5" in rewrite
    assert "shadowNDCPos.xy = shadowNDCPos.xy * 0.5 + 0.5" in rewrite
    assert "shadowPos.xyz / shadowPos.w * 0.5 + 0.5" in rewrite
    assert "CCGetLinearDepth(worldPos, viewspaceDepthBias) * 2.0 - 1.0" in rewrite
    assert "v_clip_depth.x / v_clip_depth.y * 0.5 + 0.5" in rewrite
    assert "v_clip_depth = clipPos.z / clipPos.w * 0.5 + 0.5" in rewrite

    rewrite_call = compile_shader.index("rewriteD3D12LegacyShadowClipDepth(glslSource)")
    optimize_call = compile_shader.index("optimizeD3D12ShaderSource(", rewrite_call)
    cache_key = compile_shader.index("makeDXBCCacheKey(", optimize_call)
    assert rewrite_call < optimize_call < cache_key
    assert "legacyShadowRewrite.source, false" in compile_shader


def test_local_batch_uses_prepared_binding_metadata_once() -> None:
    descriptor_h = read("D3D12DescriptorSet.h")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")
    command_cpp = read("D3D12CommandBuffer.cpp")

    assert "D3D12LocalRootCbvBatchData" in descriptor_h
    assert "getLocalRootCbvBatchData" in descriptor_h
    assert "rootUniformSlotIndices" in descriptor_cpp
    assert "lastComparedStaticResource" in descriptor_cpp

    capture = function_body(
        command_cpp,
        "bool CCD3D12CommandBuffer::captureLocalRootCbvBatchBinding",
    )
    assert "getLocalRootCbvBatchData" in capture
    assert "getUniformDescriptorSlotCount" not in capture
    assert "getUniformDescriptorSignature" not in capture

    fused_draw = function_body(
        command_cpp,
        "void CCD3D12CommandBuffer::drawWithInputAssemblerAndDescriptorSet",
    )
    assert "drawLocalRootCbvBatchInternal(info, d3d12Set)" in fused_draw
    assert "getCbvSrvUavPartition" not in fused_draw
    assert "getStaticDescriptorAnalysis" not in fused_draw
    assert "getSamplerTableKey" not in fused_draw


def test_unchanged_input_assembler_skips_native_view_rebuild() -> None:
    command_cpp = read("D3D12CommandBuffer.cpp")
    bind_ia = function_body(command_cpp, "void CCD3D12CommandBuffer::bindInputAssembler")
    assert "sameLogicalInputAssembler && !bufferViewsChanged" in bind_ia
    assert bind_ia.index("sameLogicalInputAssembler && !bufferViewsChanged") < bind_ia.index(
        "fillVertexBufferViews"
    )


def test_stable_draw_sequence_reuses_verified_local_binding() -> None:
    descriptor_h = read("D3D12DescriptorSet.h")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")
    command_cpp = read("D3D12CommandBuffer.cpp")
    input_h = read("D3D12InputAssembler.h")

    assert "rootBuffers" in descriptor_h
    assert "D3D12_MAX_LOCAL_ROOT_CBVS = 8" in descriptor_h
    assert "COMMAND_CAPACITY = 4096U" in command_cpp
    assert "getLocalRootCbvBatchFastData" in descriptor_h
    fast_data = function_body(
        descriptor_cpp,
        "bool CCD3D12DescriptorSet::getLocalRootCbvBatchFastData",
    )
    assert "_isDirty" in fast_data
    assert "uniformBufferDescriptorSlots.size() != 1" not in fast_data
    assert "metadata.rootUniformSlotIndices[i]" in fast_data
    assert "data.rootBuffers[i]" in fast_data
    assert "getDummyBuffer" in fast_data
    assert "updateForLocalRootCbv" not in fast_data

    prepared_data = function_body(
        descriptor_cpp,
        "bool CCD3D12DescriptorSet::getLocalRootCbvBatchData",
    )
    assert "getDummyBuffer" in prepared_data

    assert "getD3D12ViewSignature" in input_h
    assert "localRootCbvCachedDraws" not in command_cpp
    cached_draw = function_body(
        command_cpp,
        "bool CCD3D12CommandBuffer::tryAppendCompatibleLocalRootCbvDraw",
    )
    assert "getD3D12ViewSignature" in cached_draw
    assert "getLocalRootCbvPreparedPacket" in cached_draw
    assert "preparedPacket->staticSignature" in cached_draw
    assert "preparedPacket->samplerSignature" in cached_draw
    assert "bindInputAssembler" not in cached_draw
    assert "captureLocalRootCbvBatchBinding" not in cached_draw


def test_transient_uniform_update_writes_final_frame_allocation_once() -> None:
    buffer_cpp = read("D3D12Buffer.cpp")
    update = function_body(buffer_cpp, "void CCD3D12Buffer::update")
    transient_start = update.index("if (isTransientUniformEligible())")
    fallback_start = update.index("if (!isD3D12UploadHeap())", transient_start)
    transient_path = update[transient_start:fallback_start]

    assert "allocateUploadBuffer" in transient_path
    assert "transientUniformGPUAddress" in transient_path
    assert "enqueueBufferUpdate" not in transient_path
    assert "pendingData.resize" not in transient_path


def test_indirect_record_packs_only_active_root_cbvs() -> None:
    command_cpp = read("D3D12CommandBuffer.cpp")
    append = function_body(
        command_cpp,
        "bool CCD3D12CommandBuffer::appendLocalRootCbvBatchCommand",
    )

    assert "rootAddressBytes" in append
    assert "packedPayloadBytes" in append
    assert "rootAddressBytes + sizeof(D3D12_DRAW_ARGUMENTS)" in append
    assert "rootAddressBytes + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)" in append
    assert "destination + rootAddressBytes" in append


def test_local_root_partition_promotes_only_per_draw_b0() -> None:
    pipeline_cpp = read("D3D12PipelineLayout.cpp")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")

    assert "localRootCbvAssigned" in pipeline_cpp
    assert "binding.count > 1" in pipeline_cpp
    assert "suffixRange.BaseShaderRegister = binding.binding + 1" in pipeline_cpp
    refresh = function_body(
        descriptor_cpp,
        "void CCD3D12DescriptorSet::refreshStaticDescriptorMetadata",
    )
    assert "rootCbvAssigned" in refresh
    assert "metadata.rootCbvCount = 1" in refresh
    assert "staticElementStart = 1" in refresh


def test_local_root_split_repacks_when_descriptor_tables_cross_heaps() -> None:
    command_cpp = read("D3D12CommandBuffer.cpp")
    flush = function_body(
        command_cpp,
        "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental",
    )
    split_start = flush.index("auto prepareLocalStaticSplit")
    root_branch_start = flush.index("if (binding.useLocalRootCbv)", split_start)
    root_branch_end = flush.index(
        "const auto dynamicAllocation = cbvPool->allocate(binding.dynamicCbvCount)",
        root_branch_start,
    )
    root_branch = flush[root_branch_start:root_branch_end]

    assert "dynamicDescriptorCount + binding.staticCbvSrvUavCount" in root_branch
    assert "const auto combinedAllocation = cbvPool->allocate" in root_branch
    assert "copyLocalDynamicCbvSrvUavTable" in root_branch
    assert "copyLocalStaticCbvTable" in root_branch
    assert "staticAllocation.heapIndex != dynamicAllocation.heapIndex" not in root_branch


def test_local_root_split_accepts_non_null_dynamic_descriptor_sources() -> None:
    descriptor_h = read("D3D12DescriptorSet.h")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")
    command_cpp = read("D3D12CommandBuffer.cpp")

    assert "getLocalRootCbvData" in descriptor_h
    root_data = function_body(
        descriptor_cpp,
        "bool CCD3D12DescriptorSet::getLocalRootCbvData",
    )
    assert "metadata.cbvSrvUavPartitionValid" in root_data
    assert "onlyNullDynamicDescriptorSources" not in root_data
    assert "staticResourceIdentityValid" not in root_data

    flush = function_body(
        command_cpp,
        "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental",
    )
    split_start = flush.index("auto prepareLocalStaticSplit")
    root_branch_start = flush.index("if (binding.useLocalRootCbv)", split_start)
    root_branch_end = flush.index(
        "const auto dynamicAllocation = cbvPool->allocate(binding.dynamicCbvCount)",
        root_branch_start,
    )
    root_branch = flush[root_branch_start:root_branch_end]
    assert "getLocalRootCbvData" in root_branch
    assert "getLocalRootCbvBatchFastData" not in root_branch

    for signature in (
        "bool CCD3D12DescriptorSet::getLocalRootCbvBatchData",
        "bool CCD3D12DescriptorSet::getLocalRootCbvBatchFastData",
    ):
        batch_data = function_body(descriptor_cpp, signature)
        assert "metadata.onlyNullDynamicDescriptorSources" in batch_data


def test_local_batch_writes_commands_directly_to_final_upload() -> None:
    command_h = read("D3D12CommandBuffer.h")
    command_cpp = read("D3D12CommandBuffer.cpp")

    assert "appendLocalRootCbvBatchCommand" in command_h
    append = function_body(
        command_cpp,
        "bool CCD3D12CommandBuffer::appendLocalRootCbvBatchCommand",
    )
    assert "localRootCbvBatchUpload.mappedData" in append
    assert "destination + rootAddressBytes" in append
    assert "localRootCbvBatchDraws.emplace_back" not in append
    assert "localRootCbvBatchIndexedDraws.emplace_back" not in append

    compatible = function_body(
        command_cpp,
        "bool CCD3D12CommandBuffer::tryAppendCompatibleLocalRootCbvDraw",
    )
    assert "appendLocalRootCbvBatchCommand(info, &gpuAddress, 1U)" in compatible

    flush = function_body(command_cpp, "void CCD3D12CommandBuffer::flushLocalRootCbvBatch")
    assert "localRootCbvBatchUpload.resource" in flush
    assert "for (const auto &command : _impl->localRootCbvBatch" not in flush


def test_descriptor_set_exposes_lightweight_local_draw_packet() -> None:
    descriptor_h = read("D3D12DescriptorSet.h")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")
    command_cpp = read("D3D12CommandBuffer.cpp")

    assert "D3D12LocalRootCbvFastPacket" in descriptor_h
    assert "getLocalRootCbvFastPacket" in descriptor_h
    assert "uint64_t gpuAddress" in descriptor_h
    assert "rootBuffers" in descriptor_cpp
    fast_packet = function_body(
        descriptor_cpp,
        "bool CCD3D12DescriptorSet::getLocalRootCbvFastPacket",
    )
    assert "metadata.localBatchFastPacketValid" in fast_packet
    assert "metadata.rootBuffers[0]" in fast_packet
    assert "packet.gpuAddress" in fast_packet
    assert "for (" not in fast_packet
    assert "CCD3D12Device::getInstance" not in fast_packet

    compatible = function_body(
        command_cpp,
        "bool CCD3D12CommandBuffer::tryAppendCompatibleLocalRootCbvDraw",
    )
    assert "D3D12LocalRootCbvPreparedPacket" in descriptor_h
    assert "getLocalRootCbvPreparedPacket" in compatible
    assert "getLocalRootCbvFastPacket" not in compatible
    assert "D3D12LocalRootCbvBatchData fastData" not in compatible


def test_transient_uniform_uses_stable_per_buffer_frame_slots() -> None:
    device_h = read("D3D12Device.h")
    device_cpp = read("D3D12Device.cpp")
    buffer_cpp = read("D3D12Buffer.cpp")

    assert "getOrCreateTransientUniformSlot" in device_h
    assert "TRANSIENT_UNIFORM_SLOT_COUNT" in device_cpp
    assert "transientUniformSlotArena" in device_cpp
    stable_slot = function_body(
        device_cpp,
        "D3D12UploadAllocation CCD3D12Device::getOrCreateTransientUniformSlot",
    )
    assert "activeFrameResource" in stable_slot
    assert "slotIndex" in stable_slot
    assert "D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT" in stable_slot

    assert "transientUniformSlotIndex" in buffer_cpp
    update = function_body(buffer_cpp, "void CCD3D12Buffer::update")
    assert "transientUniformSlotEpoch != epoch" in update
    assert "getOrCreateTransientUniformSlot" in update
    assert update.index("getOrCreateTransientUniformSlot") < update.index("allocateUploadBuffer")


def test_local_draw_uses_persistent_resource_packet() -> None:
    buffer_h = read("D3D12Buffer.h")
    descriptor_h = read("D3D12DescriptorSet.h")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")
    command_cpp = read("D3D12CommandBuffer.cpp")

    assert "getD3D12UniformGPUVirtualAddressStorage" in buffer_h
    assert "D3D12LocalRootCbvPreparedPacket" in descriptor_h
    assert "getLocalRootCbvPreparedPacket" in descriptor_h
    refresh = function_body(
        descriptor_cpp,
        "void CCD3D12DescriptorSet::refreshStaticDescriptorMetadata",
    )
    assert "getD3D12UniformGPUVirtualAddressStorage" in refresh

    compatible = function_body(
        command_cpp,
        "bool CCD3D12CommandBuffer::tryAppendCompatibleLocalRootCbvDraw",
    )
    assert "getLocalRootCbvPreparedPacket" in compatible
    assert "getLocalRootCbvFastPacket" not in compatible
    assert "getD3D12UniformGPUVirtualAddress" not in compatible


def test_local_compatibility_packet_is_force_inlined() -> None:
    input_h = read("D3D12InputAssembler.h")
    input_cpp = read("D3D12InputAssembler.cpp")
    descriptor_h = read("D3D12DescriptorSet.h")
    command_cpp = read("D3D12CommandBuffer.cpp")

    assert "CC_FORCE_INLINE uint64_t getD3D12ViewSignature() const" in input_h
    assert "_d3d12ViewSignature" in input_h
    assert "uint64_t CCD3D12InputAssembler::getD3D12ViewSignature() const" not in input_cpp
    assert "CC_FORCE_INLINE const D3D12LocalRootCbvPreparedPacket *" in descriptor_h
    compatible = function_body(
        command_cpp,
        "bool CCD3D12CommandBuffer::tryAppendCompatibleLocalRootCbvDraw",
    )
    assert "const uint64_t inputAssemblerSignature" in compatible
    assert compatible.count("getD3D12ViewSignature()") == 1


def test_transient_uniform_derives_slot_from_compact_active_frame_state() -> None:
    device_h = read("D3D12Device.h")
    device_cpp = read("D3D12Device.cpp")
    buffer_cpp = read("D3D12Buffer.cpp")

    assert "D3D12TransientUniformFrameState" in device_h
    assert "getActiveTransientUniformFrameState" in device_h
    assert "activeTransientUniformFrameState" in device_cpp
    update = function_body(buffer_cpp, "void CCD3D12Buffer::update")
    assert "getActiveTransientUniformFrameState" in update
    assert "transientUniformSlotIndex != INVALID_SLOT" in update
    assert "frameState.mappedData" in update
    assert "frameState.gpuAddress" in update
    assert update.index("getActiveTransientUniformFrameState") < update.index(
        "getOrCreateTransientUniformSlot"
    )


def test_transient_uniform_static_content_survives_frame_resource_reuse() -> None:
    buffer_cpp = read("D3D12Buffer.cpp")
    device_cpp = read("D3D12Device.cpp")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")

    update = function_body(buffer_cpp, "void CCD3D12Buffer::update")
    assert "uniformShadowData" in update
    assert update.index("uniformShadowData") < update.index(
        "if (isTransientUniformEligible())"
    )

    flush_transient = function_body(
        buffer_cpp,
        "bool CCD3D12Buffer::flushTransientUniformUpload",
    )
    ensure_transient = function_body(
        buffer_cpp,
        "bool CCD3D12Buffer::ensureTransientUniformUpload",
    )
    assert "uniformShadowData" in flush_transient
    assert "uniformShadowData" in ensure_transient

    acquire = function_body(device_cpp, "void CCD3D12Device::acquire")
    frame_switch = acquire.index("_impl->activeFrameResource = nextFrameResource")
    assert acquire.index("notifyTransientUniformUpload()", frame_switch) > frame_switch

    regular_update = function_body(
        descriptor_cpp,
        "void CCD3D12DescriptorSet::update()",
    )
    local_update = function_body(
        descriptor_cpp,
        "void CCD3D12DescriptorSet::updateForLocalRootCbv",
    )
    prepare_uniform = function_body(
        descriptor_cpp,
        "void prepareCurrentFrameUniformBuffer",
    )
    assert "ensureTransientUniformUpload" in prepare_uniform
    for descriptor_update in (regular_update, local_update):
        prepare = descriptor_update.index("prepareCurrentFrameUniformBuffer")
        version = descriptor_update.index("getUniformDescriptorVersion", prepare)
        assert prepare < version


def test_command_buffer_uniform_updates_freeze_dynamic_cbv_addresses() -> None:
    buffer_h = read("D3D12Buffer.h")
    buffer_cpp = read("D3D12Buffer.cpp")
    command_cpp = read("D3D12CommandBuffer.cpp")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")

    assert "updateTransientUniform" in buffer_h
    freeze = function_body(
        buffer_cpp,
        "bool CCD3D12Buffer::updateTransientUniform",
    )
    assert "isDynamicUniformOnly()" in freeze
    assert "uniformBackingSize" in freeze
    assert "uniformShadowData.data() + resourceOffset" in freeze
    assert "allocateUploadBuffer" in freeze
    assert "transientUniformGPUAddress = allocation.gpuAddress" in freeze

    command_update = function_body(
        command_cpp,
        "void CCD3D12CommandBuffer::updateBuffer",
    )
    freeze_call = command_update.index("updateTransientUniform")
    stable_resource = command_update.index("getD3D12ResourceHandle")
    assert freeze_call < stable_resource

    dynamic_source = function_body(
        descriptor_cpp,
        "bool CCD3D12DescriptorSet::getDynamicDescriptorSource",
    )
    assert "slot.type == DescriptorType::DYNAMIC_UNIFORM_BUFFER" in dynamic_source
    assert "getD3D12UniformGPUVirtualAddress" in dynamic_source

    apply_offsets = function_body(
        descriptor_cpp,
        "void CCD3D12DescriptorSet::applyDynamicOffsets",
    )
    assert "getD3D12UniformGPUVirtualAddress() + dynamicOffset" in apply_offsets

    regular_update = function_body(
        buffer_cpp,
        "void CCD3D12Buffer::update",
    )
    assert "uniformBackingSize" in regular_update
    assert "uniformShadowData.data() + resourceOffset" in regular_update
    shadow_write = regular_update.index("uniformShadowData.data() + resourceOffset")
    transient_check = regular_update.index("if (isTransientUniformEligible())")
    assert shadow_write < transient_check


def test_local_descriptor_change_cannot_downgrade_full_dirty_state() -> None:
    command_cpp = read("D3D12CommandBuffer.cpp")
    bind = function_body(
        command_cpp,
        "void CCD3D12CommandBuffer::bindDescriptorSet",
    )
    assert "const bool wasDescriptorSetsDirty" in bind
    assert "wasDescriptorSetsDirty" in bind
    assert "_impl->localDescriptorSetOnlyDirty && localSetChanged" in bind


def test_dynamic_uniform_address_change_invalidates_descriptor_table_cache() -> None:
    descriptor_cpp = read("D3D12DescriptorSet.cpp")

    update = function_body(
        descriptor_cpp,
        "void CCD3D12DescriptorSet::update",
    )
    assert "dynamicDescriptorSlots" in update
    assert "DescriptorType::DYNAMIC_UNIFORM_BUFFER" in update
    assert "getUniformDescriptorVersion" in update
    assert "dynamicDescriptorChanged" in update
    assert "++_impl->version" in update

    force_update = function_body(
        descriptor_cpp,
        "void CCD3D12DescriptorSet::forceUpdate",
    )
    assert "slot.buffer = buffer" in force_update
    assert "slot.version = buffer ? buffer->getUniformDescriptorVersion() : 0" in force_update


def test_release_build_has_no_unconditional_diagnostic_logging() -> None:
    diagnostics = read("D3D12DebugOptimization.h")
    assert "CC_D3D12_ENABLE_DIAGNOSTIC_LOGS" in diagnostics
    assert "#define CC_D3D12_DIAGNOSTICS_ENABLED 0" in diagnostics
    assert "#define CC_D3D12_DIAGNOSTIC_LOG(...) ((void)0)" in diagnostics

    sources = "\n".join(
        path.read_text(encoding="utf-8")
        for path in ROOT.glob("*.cpp")
    )
    assert 'CC_LOG_INFO("[D3D12-PERF]' not in sources
    assert 'CC_LOG_INFO("[D3D12-MIP-DIAG]' not in sources
    assert 'CC_LOG_WARNING("[D3D12-MIP-DIAG]' not in sources
    assert 'CC_LOG_ERROR("[DIAG' not in sources

    swapchain = read("D3D12Swapchain.cpp")
    present = function_body(swapchain, "bool CCD3D12Swapchain::present")
    assert "#if CC_D3D12_DIAGNOSTICS_ENABLED" in present
    assert "std::chrono::steady_clock::now()" in present


def test_partial_texture_upload_uses_region_sized_footprints() -> None:
    texture_h = read("D3D12Texture.h")
    texture_cpp = read("D3D12Texture.cpp")
    device_cpp = read("D3D12Device.cpp")
    command_cpp = read("D3D12CommandBuffer.cpp")

    assert "getD3D12TextureUploadFootprint" in texture_h
    footprint = function_body(
        texture_cpp,
        "bool getD3D12TextureUploadFootprint",
    )
    assert "uploadDesc.Width = region.texExtent.width" in footprint
    assert "uploadDesc.Height = region.texExtent.height" in footprint
    assert "uploadDesc.MipLevels = 1" in footprint
    assert "GetCopyableFootprints(&uploadDesc, 0, 1, 0" in footprint

    device_upload = function_body(
        device_cpp,
        "void CCD3D12Device::copyBuffersToTextureImmediate",
    )
    command_upload = function_body(
        command_cpp,
        "void CCD3D12CommandBuffer::copyBuffersToTexture",
    )
    for upload in (device_upload, command_upload):
        assert "getD3D12TextureUploadFootprint" in upload
        assert "GetCopyableFootprints(&textureDesc, subresource" not in upload


def test_fragment_linkage_repair_is_shader_name_independent() -> None:
    shader_cpp = read("D3D12Shader.cpp")
    shader_h = read("D3D12Shader.h")

    linkage_patch = function_body(
        shader_cpp,
        "bool patchD3D12FragmentInputLinkage",
    )
    dummy_type = function_body(shader_cpp, "ccstd::string makeLinkageDummyType")
    assert "SPIRV_Cross_Input" in linkage_patch
    assert "semanticName" in linkage_patch
    assert "componentMask" in dummy_type
    assert "componentType" in dummy_type
    assert "legacy/toon" not in linkage_patch
    assert "getFragmentBytecodeForVertexLinkage" in shader_h
    assert "patchD3D12FragmentInputLinkage(hlslSource, *fragmentLinkage)" in shader_cpp


def test_render_pass_end_transitions_offscreen_attachments_for_legacy_consumers() -> None:
    command_cpp = read("D3D12CommandBuffer.cpp")
    end_pass = function_body(command_cpp, "void CCD3D12CommandBuffer::endRenderPass")

    assert "getColorTextureCount" in end_pass
    assert "getColorBacking" in end_pass
    assert "getColorAttachment" in end_pass
    assert "appendFramebufferAttachmentTransitions" in end_pass
    assert "D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE" in end_pass
    assert "D3D12_RESOURCE_STATE_DEPTH_READ" in end_pass


if __name__ == "__main__":
    test_restored_revision_fast_paths()
    test_local_batch_uses_prepared_binding_metadata_once()
    test_unchanged_input_assembler_skips_native_view_rebuild()
    test_stable_draw_sequence_reuses_verified_local_binding()
    test_transient_uniform_update_writes_final_frame_allocation_once()
    test_indirect_record_packs_only_active_root_cbvs()
    test_local_root_partition_promotes_only_per_draw_b0()
    test_local_batch_writes_commands_directly_to_final_upload()
    test_descriptor_set_exposes_lightweight_local_draw_packet()
    test_transient_uniform_uses_stable_per_buffer_frame_slots()
    test_local_draw_uses_persistent_resource_packet()
    test_local_compatibility_packet_is_force_inlined()
    test_transient_uniform_derives_slot_from_compact_active_frame_state()
    test_transient_uniform_static_content_survives_frame_resource_reuse()
    test_partial_texture_upload_uses_region_sized_footprints()
    test_fragment_linkage_repair_is_shader_name_independent()
    test_render_pass_end_transitions_offscreen_attachments_for_legacy_consumers()
    print("D3D12 hot-path static tests passed")
