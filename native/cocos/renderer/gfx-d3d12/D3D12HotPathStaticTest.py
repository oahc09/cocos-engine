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
        "for (const auto &slot"
    )


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
    print("D3D12 hot-path static tests passed")
