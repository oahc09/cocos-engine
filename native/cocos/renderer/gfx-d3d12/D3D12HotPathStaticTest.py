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
    assert "COMMAND_CAPACITY = 1024U" in command_cpp
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


def test_transient_uniform_slots_are_reclaimed_per_fence_safe_frame() -> None:
    device_h = read("D3D12Device.h")
    device_cpp = read("D3D12Device.cpp")
    buffer_cpp = read("D3D12Buffer.cpp")

    assert "getOrCreateTransientUniformSlot" in device_h
    assert "TRANSIENT_UNIFORM_SLOT_COUNT" in device_cpp
    assert "transientUniformSlotArena" in device_cpp
    slot_allocator = function_body(
        device_cpp,
        "D3D12UploadAllocation CCD3D12Device::getOrCreateTransientUniformSlot",
    )
    assert "frameResources.nextTransientUniformSlotIndex" in slot_allocator
    assert "D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT" in slot_allocator

    assert "transientUniformSlotIndex" in buffer_cpp
    update = function_body(buffer_cpp, "void CCD3D12Buffer::update")
    assert "transientUniformSlotEpoch != epoch" in update
    assert "_impl->transientUniformSlotIndex = INVALID_SLOT" in update
    assert "getOrCreateTransientUniformSlot" in update
    assert update.index("getOrCreateTransientUniformSlot") < update.index("allocateUploadBuffer")

    acquire = function_body(device_cpp, "void CCD3D12Device::acquire")
    assert "frameResources.nextTransientUniformSlotIndex = 0" in acquire


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
    assert "const uint64_t epoch = frameState.epoch" in update
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
    assert "const uint64_t shadowOffset = _resourceOffset" in regular_update
    assert "uniformShadowData.data() + shadowOffset" in regular_update
    shadow_write = regular_update.index("uniformShadowData.data() + shadowOffset")
    transient_check = regular_update.index("if (isTransientUniformEligible())")
    assert shadow_write < transient_check


def test_shared_upload_migration_preserves_buffer_view_relative_offsets() -> None:
    buffer_cpp = read("D3D12Buffer.cpp")

    view_init = function_body(buffer_cpp, "void CCD3D12Buffer::doInit(const BufferViewInfo &info)")
    assert "_impl = buffer->_impl" in view_init
    assert "_resourceOffset = buffer->_resourceOffset + viewOffset" in view_init

    resource_offset = function_body(buffer_cpp, "uint64_t CCD3D12Buffer::getD3D12ResourceOffset")
    assert "_impl->resourceBaseOffset + _resourceOffset" in resource_offset

    migration = function_body(buffer_cpp, "bool CCD3D12Buffer::ensureUploadResource")
    assert "srcMapped) + _impl->resourceBaseOffset" in migration
    assert "_impl->resourceBaseOffset = 0" in migration
    # _resourceOffset is per-instance: a BufferView keeps its relative offset
    # across migration to a dedicated resource. Resetting it would collapse the
    # view's GPU VA / write position to the resource start (data corruption).
    assert "_resourceOffset = 0" not in migration


def test_frame_local_descriptor_pools_reuse_overflow_heaps_after_fence() -> None:
    device_cpp = read("D3D12Device.cpp")
    pool_cpp = read("D3D12DescriptorHeapPool.cpp")

    assert "gpuDescriptorHeapPools" in device_cpp
    acquire = function_body(device_cpp, "void CCD3D12Device::acquire")
    assert "gpuDescriptorHeapPools[nextFrameResource]" in acquire
    assert "gpuDescriptorHeapPool->reset()" in acquire
    assert "beginFrameAllocationRange" not in acquire

    reset = function_body(pool_cpp, "void D3D12DescriptorHeapPool::reset")
    assert "for (auto &heap : _impl->heaps)" in reset
    assert "heap.usedCount = 0" in reset
    assert "heaps.resize" not in reset


def test_render_pass_color_attachment_count_matches_engine_stack_capacity() -> None:
    framebuffer_cpp = read("D3D12Framebuffer.cpp")
    command_cpp = read("D3D12CommandBuffer.cpp")

    framebuffer_init = function_body(
        framebuffer_cpp,
        "void CCD3D12Framebuffer::doInit(const FramebufferInfo &info)",
    )
    assert "colorCount > MAX_ATTACHMENTS" in framebuffer_init

    begin_pass = function_body(
        command_cpp,
        "void CCD3D12CommandBuffer::beginRenderPass",
    )
    assert "colorCount > MAX_ATTACHMENTS" in begin_pass


def test_persistent_descriptor_staging_pools_do_not_use_tiny_heap_granularity() -> None:
    device_cpp = read("D3D12Device.cpp")
    init = function_body(device_cpp, "bool CCD3D12Device::doInit")

    # These pools retain allocations for every live DescriptorSet, unlike the
    # frame-local shader-visible pools. Tiny 16/64 descriptor heaps cause one
    # ID3D12DescriptorHeap object per few sets and eventually exhaust memory.
    assert "HeapType::CBV_SRV_UAV, 2048, false" in init
    assert "HeapType::SAMPLER, 2048, false" in init
    assert "HeapType::SAMPLER, 2048, true" in init


def test_descriptor_perf_counters_are_runtime_opt_in_and_cover_hot_path() -> None:
    device_cpp = read("D3D12Device.cpp")
    command_cpp = read("D3D12CommandBuffer.cpp")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")

    assert 'isEnvironmentFlagEnabled("CC_D3D12_PERF_COUNTERS")' in device_cpp
    assert '"[D3D12-PERF] descriptor flush=' in device_cpp
    assert "recordDescriptorCopy(count)" in command_cpp
    assert "recordSetDescriptorHeaps()" in command_cpp
    assert "recordRootDescriptorTableBind()" in command_cpp
    assert "recordDynamicDescriptorRewrite(false)" in descriptor_cpp
    assert "recordDynamicDescriptorRewrite(true)" in descriptor_cpp
    assert "transientSlot=%llu" in device_cpp
    assert "recordTransientUniformSlotFallback" in read("D3D12Buffer.cpp")


def test_format_mapping_has_one_d3d12_source_of_truth() -> None:
    texture_h = read("D3D12Texture.h")
    texture_cpp = read("D3D12Texture.cpp")
    render_pass_cpp = read("D3D12RenderPass.cpp")
    framebuffer_cpp = read("D3D12Framebuffer.cpp")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")
    device_cpp = read("D3D12Device.cpp")

    for signature in (
        "getD3D12TextureResourceFormat",
        "getD3D12ShaderResourceFormat",
        "getD3D12DepthStencilViewFormat",
    ):
        assert signature in texture_h
        assert signature in texture_cpp

    typed_map = function_body(texture_cpp, "DXGI_FORMAT mapD3D12Format")
    assert "case Format::R8SN:" in typed_map
    assert "case Format::RG8SN:" in typed_map
    assert "case Format::RGBA8SN:" in typed_map
    texture_create = function_body(texture_cpp, "bool CCD3D12Texture::createResource")
    assert "getD3D12TextureResourceFormat(_info.format)" in texture_create

    assert "toDXGIFormatUint32" not in render_pass_cpp
    assert "toD3D12Format(attachment.format)" in render_pass_cpp
    assert "toD3D12DSVFormat" not in framebuffer_cpp
    assert "getD3D12DepthStencilViewFormat" in framebuffer_cpp
    assert "toSRVFormat" not in descriptor_cpp
    assert "getD3D12ShaderResourceFormat" in descriptor_cpp
    assert "getD3D12ShaderResourceFormat(format)" in device_cpp


def test_texture_view_type_mapping_covers_arrays_and_storage_ranges() -> None:
    texture_cpp = read("D3D12Texture.cpp")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")

    texture_create = function_body(texture_cpp, "bool CCD3D12Texture::createResource")
    assert "TextureType::TEX1D_ARRAY" in texture_create
    assert "D3D12_RESOURCE_DIMENSION_TEXTURE1D" in texture_create

    srv_dimension = function_body(descriptor_cpp, "D3D12_SRV_DIMENSION toSRVDimension")
    uav_dimension = function_body(descriptor_cpp, "D3D12_UAV_DIMENSION toUAVDimension")
    for source in (srv_dimension, uav_dimension):
        assert "case TextureType::TEX1D_ARRAY:" in source
        assert "case TextureType::TEX2D_ARRAY:" in source
        assert "DIMENSION_UNKNOWN" in source

    srv_desc = function_body(descriptor_cpp, "D3D12_SHADER_RESOURCE_VIEW_DESC makeTextureSRVDesc")
    assert "D3D12_SRV_DIMENSION_TEXTURE1DARRAY" in srv_desc
    assert "desc.Texture1DArray.FirstArraySlice = baseLayer" in srv_desc
    assert descriptor_cpp.count("srvDesc.ViewDimension != D3D12_SRV_DIMENSION_UNKNOWN") == 3

    uav_desc = function_body(descriptor_cpp, "bool makeTextureUAVDesc")
    assert "viewInfo.baseLevel" in uav_desc
    assert "viewInfo.baseLayer" in uav_desc
    assert "D3D12_UAV_DIMENSION_TEXTURE2DARRAY" in uav_desc
    assert "D3D12_UAV_DIMENSION_TEXTURE3D" in uav_desc


def test_root_visibility_does_not_treat_each_stage_as_all_stages() -> None:
    pipeline_layout_cpp = read("D3D12PipelineLayout.cpp")
    visibility = function_body(
        pipeline_layout_cpp,
        "D3D12_SHADER_VISIBILITY toD3D12ShaderVisibility",
    )
    assert "hasAllFlags(stageFlags, ShaderStageFlagBit::ALL)" in visibility
    assert "hasAnyFlags(stageFlags, ShaderStageFlagBit::ALL)" not in visibility


def test_shader_scheduler_releases_completed_task_captures() -> None:
    scheduler = read("D3D12ShaderCompileScheduler.h")
    execute = function_body(scheduler, "void executeTask")
    assert "task->status = TaskStatus::COMPLETED" in execute
    assert "task->task = {}" in execute


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


def test_skipped_render_pass_restores_swapchain_back_buffer_to_present() -> None:
    command_cpp = read("D3D12CommandBuffer.cpp")
    end_pass = function_body(command_cpp, "void CCD3D12CommandBuffer::endRenderPass")

    # beginRenderPass records the PRESENT -> RENDER_TARGET barrier before skip
    # conditions (empty render area / MAX_ATTACHMENTS). The skip branch must
    # transition the back buffer back to PRESENT, otherwise Present() fails.
    skip_block = end_pass[end_pass.index("_impl->skipRenderPass"):]
    assert "OMSetRenderTargets(0, nullptr, FALSE, nullptr)" in skip_block
    assert "activeSwapchainBackBuffer" in skip_block
    assert "D3D12_RESOURCE_STATE_PRESENT" in skip_block
    assert "activeSwapchain = nullptr" in skip_block


def test_fragment_hlsl_source_is_retained_for_linkage_repair() -> None:
    shader_cpp = read("D3D12Shader.cpp")

    # getFragmentBytecodeForVertexLinkage recompiles _impl->fragmentSource.source
    # when VS/PS linkage is incompatible. ensureStageBytecode must not release
    # the fragment source when installing DXBC, or repair permanently fails and
    # CreateGraphicsPipelineState drops those materials.
    linkage = function_body(
        shader_cpp,
        "CCD3D12Shader::BytecodeBlob CCD3D12Shader::getFragmentBytecodeForVertexLinkage",
    )
    assert "fragmentSource.source.empty()" in linkage

    ensure = function_body(
        shader_cpp,
        "CCD3D12Shader::BytecodeBlob CCD3D12Shader::ensureStageBytecode",
    )
    assert ensure.count("stage != ShaderStageFlagBit::FRAGMENT") >= 2


def test_d3d12ma_allocator_lifecycle() -> None:
    device_h = read("D3D12Device.h")
    device_cpp = read("D3D12Device.cpp")

    # Public header forward-declares D3D12MA and exposes the accessor without
    # leaking the vendored header to every includer.
    assert "namespace D3D12MA" in device_h
    assert "getMemoryAllocator" in device_h
    assert '#include "D3D12MemAlloc.h"' not in device_h

    assert '#include "D3D12MemAlloc.h"' in device_cpp
    assert "D3D12MA::Allocator *CCD3D12Device::getMemoryAllocator" in device_cpp

    # Allocator creation must happen after the D3D12 device exists. Device
    # creation lives in initializeD3D12Context, invoked from doInit.
    do_init = function_body(device_cpp, "bool CCD3D12Device::doInit")
    assert "initializeD3D12Context()" in do_init
    context_init = function_body(device_cpp, "bool CCD3D12Device::initializeD3D12Context")
    assert "D3D12MA::CreateAllocator" in context_init
    # ALLOCATOR_FLAGS is an unscoped enum; MSVC rejects a bare int literal.
    assert "D3D12MA::ALLOCATOR_FLAG_NONE" in context_init
    assert context_init.index("D3D12CreateDevice") < context_init.index("D3D12MA::CreateAllocator")

    # WARP fallback must use EnumWarpAdapter (not D3D12CreateDevice(nullptr,
    # ...)) — the latter selects the default hardware adapter, not WARP.
    assert "EnumWarpAdapter" in context_init
    assert "D3D12CreateDevice(nullptr" not in context_init

    # D3D12MA v3.2.0 requires a non-null pAdapter. Allocator creation must
    # be guarded by an adapter check.
    assert "if (_impl->adapter)" in context_init

    # Allocator is created last so earlier failures don't leak it.
    assert context_init.index("CreateFence") < context_init.index("D3D12MA::CreateAllocator")
    assert context_init.index("CreateEvent") < context_init.index("D3D12MA::CreateAllocator")

    # Allocator teardown must precede device release; the adapter must be
    # released after the allocator (the allocator AddRefs the adapter).
    do_destroy = function_body(device_cpp, "void CCD3D12Device::doDestroy")
    assert "memoryAllocator.Reset()" in do_destroy
    assert do_destroy.index("memoryAllocator.Reset()") < do_destroy.index("d3dDevice.Reset()")
    assert do_destroy.index("memoryAllocator.Reset()") < do_destroy.index("adapter.Reset()")


def test_initializeD3D12Context_failure_rollback() -> None:
    device_cpp = read("D3D12Device.cpp")
    context_init = function_body(device_cpp, "bool CCD3D12Device::initializeD3D12Context")

    # Every return-false path must call cleanupContext() to release partially
    # created D3D12/adapter/factory resources. GFXDevice::initialize does not
    # call doDestroy() on failure, so the context must self-clean.
    assert "auto cleanupContext" in context_init
    return_count = context_init.count("return false")
    cleanup_count = context_init.count("cleanupContext()")
    assert return_count > 0
    assert cleanup_count >= return_count


def test_buffer_creation_uses_d3d12ma_with_committed_fallback() -> None:
    buffer_cpp = read("D3D12Buffer.cpp")
    buffer_h = read("D3D12Buffer.h")
    resource_state_h = read("D3D12ResourceState.h")

    # D3D12BufferBacking is defined in D3D12ResourceState.h and holds
    # allocations BEFORE resources for correct C++ destruction order.
    assert "struct D3D12BufferBacking final" in resource_state_h
    backing_block = resource_state_h[
        resource_state_h.index("struct D3D12BufferBacking final"):
        resource_state_h.index("};", resource_state_h.index("struct D3D12BufferBacking final"))
    ]
    assert backing_block.index("d3d12maAllocation") < backing_block.index("ComPtr<ID3D12Resource> resource")
    assert backing_block.index("uploadAllocations") < backing_block.index("uploadResources")

    # Impl holds an immutable backing via shared_ptr; resource/Allocation
    # members are no longer directly in Impl.
    impl_block = buffer_cpp[
        buffer_cpp.index("struct CCD3D12Buffer::Impl"):
        buffer_cpp.index("};", buffer_cpp.index("struct CCD3D12Buffer::Impl"))
    ]
    assert "shared_ptr<D3D12BufferBacking> backing" in impl_block
    assert "ComPtr<D3D12MA::Allocation> d3d12maAllocation" not in impl_block
    assert "ComPtr<ID3D12Resource> resource" not in impl_block

    # getD3D12BufferBacking returns typed shared_ptr, not shared_ptr<void>.
    assert "shared_ptr<D3D12BufferBacking> getD3D12BufferBacking" in buffer_h
    assert "shared_ptr<void> getD3D12BufferBacking" not in buffer_h

    # createResource() tries the allocator first, falls back to committed.
    create = function_body(buffer_cpp, "bool CCD3D12Buffer::createResource")
    assert "device->getMemoryAllocator()" in create
    assert "allocator->CreateResource" in create
    assert "D3D12MA::ALLOCATION_FLAG_NONE" in create
    assert "CreateCommittedResource" in create
    alloc_check = create.index("if (!allocation)")
    committed = create.index("CreateCommittedResource", alloc_check)
    assert alloc_check < committed

    # Resize is atomic: new backing created, populated, then swapped.
    assert "make_shared<D3D12BufferBacking>" in create
    assert "_impl->backing = std::move(newBacking)" in create
    # Old resources are NOT moved out before creation — if creation fails,
    # the buffer keeps its old backing intact.
    assert "oldResource = std::move" not in create
    assert "oldAllocation = std::move" not in create
    committed_swap = create.rindex("_impl->backing = std::move(newBacking)")
    committed_pool_state = create.index("_impl->usingSharedUploadPool = false")
    assert committed_swap < committed_pool_state

    # ensureUploadResource() uses COW: copies backing if shared.
    ensure = function_body(buffer_cpp, "bool CCD3D12Buffer::ensureUploadResource")
    assert "backing.use_count() > 1" in ensure
    assert "make_shared<D3D12BufferBacking>" in ensure
    assert "allocator->CreateResource" in ensure
    assert "CreateCommittedResource" in ensure
    assert "uploadRes0" in ensure
    assert "uploadResN" in ensure

    # Buffer doDestroy does NOT modify shared Impl — only drops the shared_ptr.
    buf_destroy = function_body(buffer_cpp, "void CCD3D12Buffer::doDestroy")
    assert "_impl.reset()" in buf_destroy
    assert "backing->resource.Reset()" not in buf_destroy
    assert "backing->d3d12maAllocation.Reset()" not in buf_destroy


def test_texture_creation_uses_d3d12ma_with_committed_fallback() -> None:
    texture_cpp = read("D3D12Texture.cpp")
    resource_state_h = read("D3D12ResourceState.h")

    # Allocation lives in D3D12ResourceBacking (unified owner), not in Impl.
    assert "ComPtr<D3D12MA::Allocation> d3d12maAllocation" in resource_state_h
    assert "ComPtr<D3D12MA::Allocation> d3d12maAllocation" not in texture_cpp

    # createResource() tries the allocator first, falls back to committed.
    create = function_body(texture_cpp, "bool CCD3D12Texture::createResource")
    assert "device->getMemoryAllocator()" in create
    assert "allocator->CreateResource" in create
    assert "D3D12_HEAP_TYPE_DEFAULT" in create
    # The optimized clear value is forwarded to D3D12MA.
    assert "optimizedClearValue, allocation.ReleaseAndGetAddressOf()" in create
    # Fallback: committed path guarded by null-allocation check.
    assert "if (!allocation)" in create
    assert "CreateCommittedResource" in create

    # Allocation is stored in the new backing (unified owner), not in Impl.
    assert "newBacking->d3d12maAllocation = std::move(allocation)" in create
    # Resize does NOT prematurely release the old allocation — the old backing
    # shared_ptr defers release until all consumers drop their references.
    assert "d3d12maAllocation.Reset()" not in create

    # Failure cleanup releases resource before allocation.
    fail_cleanup = create[create.index("if (FAILED(hr) || !allocation)"):]
    fail_cleanup = fail_cleanup[:fail_cleanup.index("}")]
    assert fail_cleanup.index("resource.Reset()") < fail_cleanup.index("allocation.Reset()")


def test_d3d12ma_unified_backing_owner() -> None:
    resource_state_h = read("D3D12ResourceState.h")
    resource_state_cpp = read("D3D12ResourceState.cpp")
    device_cpp = read("D3D12Device.cpp")

    # D3D12ResourceBacking is the texture unified owner: allocation declared
    # first, resource declared second.
    backing_struct = resource_state_h[
        resource_state_h.index("struct D3D12ResourceBacking final"):
        resource_state_h.index("};", resource_state_h.index("struct D3D12ResourceBacking final"))
    ]
    alloc_pos = backing_struct.index("ComPtr<D3D12MA::Allocation> d3d12maAllocation")
    res_pos = backing_struct.index("ComPtr<ID3D12Resource> resource")
    assert alloc_pos < res_pos, "Texture backing must declare allocation before resource"

    # D3D12BufferBacking is the buffer immutable owner: allocations before
    # resources for correct C++ reverse-declaration destruction order.
    buf_backing = resource_state_h[
        resource_state_h.index("struct D3D12BufferBacking final"):
        resource_state_h.index("};", resource_state_h.index("struct D3D12BufferBacking final"))
    ]
    buf_alloc = buf_backing.index("ComPtr<D3D12MA::Allocation> d3d12maAllocation")
    buf_res = buf_backing.index("ComPtr<ID3D12Resource> resource")
    assert buf_alloc < buf_res, "Buffer backing must declare allocation before resource"
    # Upload arrays also follow the same order.
    buf_upload_alloc = buf_backing.index("uploadAllocations")
    buf_upload_res = buf_backing.index("uploadResources")
    assert buf_upload_alloc < buf_upload_res, "Buffer backing upload allocations before resources"

    # Both destructors are defined out-of-line.
    assert "D3D12ResourceBacking::~D3D12ResourceBacking" in resource_state_cpp
    assert "D3D12BufferBacking::~D3D12BufferBacking" in resource_state_cpp
    assert '#include "D3D12MemAlloc.h"' in resource_state_cpp

    # Device checks allocation count before destroying allocator.
    do_destroy = function_body(device_cpp, "void CCD3D12Device::doDestroy")
    assert "GetBudget" in do_destroy
    assert "AllocationCount" in do_destroy
    assert "memoryAllocator.Detach()" in do_destroy
    budget_pos = do_destroy.index("GetBudget")
    detach_or_reset = min(
        do_destroy.index("memoryAllocator.Detach()"),
        do_destroy.index("memoryAllocator.Reset()"),
    )
    assert budget_pos < detach_or_reset


def test_d3d12ma_vendored_sources_present() -> None:
    header = read("D3D12MemAlloc.h")
    impl = read("D3D12MemAlloc.cpp")

    # Vendored copy must stay pinned to the audited 3.2.0 release.
    assert "Version 3.2.0" in header
    assert "HRESULT CreateAllocator(" in impl


def test_d3d12ma_command_buffer_retains_buffer_backing() -> None:
    buffer_h = read("D3D12Buffer.h")
    buffer_cpp = read("D3D12Buffer.cpp")
    command_h = read("D3D12CommandBuffer.h")
    command_cpp = read("D3D12CommandBuffer.cpp")
    descriptor_h = read("D3D12DescriptorSet.h")
    descriptor_cpp = read("D3D12DescriptorSet.cpp")

    # Buffer exposes typed backing (not shared_ptr<void>).
    assert "shared_ptr<D3D12BufferBacking> getD3D12BufferBacking" in buffer_h
    backing_fn = function_body(buffer_cpp, "std::shared_ptr<D3D12BufferBacking> CCD3D12Buffer::getD3D12BufferBacking")
    assert "_impl->backing" in backing_fn

    # CommandRecordingContext stores buffer backing owners for fence completion.
    assert "pendingBufferBackings" in command_cpp
    assert "pendingBufferBackingSet" in command_cpp

    # Dedicated helper deduplicates and retains buffer backing shared_ptrs.
    assert "retainCommandListBufferBacking" in command_cpp

    # Overloaded retainRecordingResource accepts a backing parameter.
    assert "std::shared_ptr<void> backing" in command_h

    # bindInputAssembler retains buffer backing for vertex/index/indirect buffers.
    bind_ia = function_body(command_cpp, "void CCD3D12CommandBuffer::bindInputAssembler")
    assert "getD3D12BufferBacking()" in bind_ia

    # updateBuffer retains buffer backing in both batch and non-batch paths.
    update_buf = function_body(command_cpp, "void CCD3D12CommandBuffer::updateBuffer")
    assert "getD3D12BufferBacking()" in update_buf

    # retainDescriptorSetResources collects buffer backings from descriptor set.
    retain_ds = function_body(
        command_cpp,
        "void CCD3D12CommandBuffer::retainDescriptorSetResources",
    )
    assert "bufferBackings" in retain_ds
    assert "pendingBufferBackings" in retain_ds

    # Lifecycle: end() moves backings to context, begin() clears them,
    # retireD3D12CommandRecordingContext clears them.
    end_fn = function_body(command_cpp, "void CCD3D12CommandBuffer::end")
    assert "context.pendingBufferBackings = std::move(_impl->pendingBufferBackings)" in end_fn
    begin_fn = function_body(command_cpp, "void CCD3D12CommandBuffer::begin")
    assert "pendingBufferBackings.clear()" in begin_fn
    retire_fn = function_body(
        command_cpp,
        "void CCD3D12CommandBuffer::retireD3D12CommandRecordingContext",
    )
    assert "pendingBufferBackings.clear()" in retire_fn

    # Descriptor set collectBoundD3D12Resources also collects buffer backing owners.
    assert "bufferBackings" in descriptor_h
    collect = function_body(
        descriptor_cpp,
        "void CCD3D12DescriptorSet::collectBoundD3D12Resources",
    )
    assert "getD3D12BufferBacking()" in collect
    assert "bufferBackings" in collect


if __name__ == "__main__":
    test_restored_revision_fast_paths()
    test_legacy_shadow_depth_source_is_normalized_before_cache_lookup()
    test_local_batch_uses_prepared_binding_metadata_once()
    test_unchanged_input_assembler_skips_native_view_rebuild()
    test_stable_draw_sequence_reuses_verified_local_binding()
    test_transient_uniform_update_writes_final_frame_allocation_once()
    test_indirect_record_packs_only_active_root_cbvs()
    test_local_root_partition_promotes_only_per_draw_b0()
    test_local_root_split_repacks_when_descriptor_tables_cross_heaps()
    test_local_root_split_accepts_non_null_dynamic_descriptor_sources()
    test_local_batch_writes_commands_directly_to_final_upload()
    test_descriptor_set_exposes_lightweight_local_draw_packet()
    test_transient_uniform_slots_are_reclaimed_per_fence_safe_frame()
    test_local_draw_uses_persistent_resource_packet()
    test_local_compatibility_packet_is_force_inlined()
    test_transient_uniform_derives_slot_from_compact_active_frame_state()
    test_transient_uniform_static_content_survives_frame_resource_reuse()
    test_command_buffer_uniform_updates_freeze_dynamic_cbv_addresses()
    test_shared_upload_migration_preserves_buffer_view_relative_offsets()
    test_frame_local_descriptor_pools_reuse_overflow_heaps_after_fence()
    test_render_pass_color_attachment_count_matches_engine_stack_capacity()
    test_persistent_descriptor_staging_pools_do_not_use_tiny_heap_granularity()
    test_descriptor_perf_counters_are_runtime_opt_in_and_cover_hot_path()
    test_format_mapping_has_one_d3d12_source_of_truth()
    test_texture_view_type_mapping_covers_arrays_and_storage_ranges()
    test_root_visibility_does_not_treat_each_stage_as_all_stages()
    test_shader_scheduler_releases_completed_task_captures()
    test_partial_texture_upload_uses_region_sized_footprints()
    test_fragment_linkage_repair_is_shader_name_independent()
    test_render_pass_end_transitions_offscreen_attachments_for_legacy_consumers()
    test_skipped_render_pass_restores_swapchain_back_buffer_to_present()
    test_fragment_hlsl_source_is_retained_for_linkage_repair()
    test_local_descriptor_change_cannot_downgrade_full_dirty_state()
    test_dynamic_uniform_address_change_invalidates_descriptor_table_cache()
    test_release_build_has_no_unconditional_diagnostic_logging()
    test_d3d12ma_allocator_lifecycle()
    test_initializeD3D12Context_failure_rollback()
    test_buffer_creation_uses_d3d12ma_with_committed_fallback()
    test_texture_creation_uses_d3d12ma_with_committed_fallback()
    test_d3d12ma_unified_backing_owner()
    test_d3d12ma_vendored_sources_present()
    test_d3d12ma_command_buffer_retains_buffer_backing()
    print("D3D12 hot-path static tests passed")
