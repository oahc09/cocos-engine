from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "cocos" / "renderer" / "gfx-d3d12"
RENDERER = ROOT / "cocos" / "renderer"


def read(name: str) -> str:
    return (D3D12 / name).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f"Could not find body for {signature}")


def assert_not_in_body(file_name: str, signature: str, forbidden: str) -> None:
    body = function_body(read(file_name), signature)
    assert forbidden not in body, f"{file_name}:{signature} still contains {forbidden}"


def assert_in_body(file_name: str, signature: str, required: str) -> None:
    body = function_body(read(file_name), signature)
    assert required in body, f"{file_name}:{signature} does not contain {required}"


def test_submit_and_present_do_not_wait_for_gpu() -> None:
    assert_not_in_body("D3D12Queue.cpp", "void CCD3D12Queue::submit", "WaitForSingleObject")
    assert_not_in_body("D3D12Device.cpp", "void CCD3D12Device::present", "WaitForSingleObject")


def test_frame_resources_are_waited_before_reuse_not_after_submit() -> None:
    assert_in_body("D3D12CommandBuffer.cpp", "void CCD3D12CommandBuffer::begin", "waitForFenceValue")
    assert_in_body("D3D12Device.cpp", "void CCD3D12Device::acquire", "retireFrameResources")


def test_frame_slot_wait_precedes_descriptor_range_reuse() -> None:
    source = read("D3D12Device.cpp")
    acquire = function_body(source, "void CCD3D12Device::acquire")

    assert "nextFrameResource" in acquire
    assert "frameResources.fence->SetEventOnCompletion" in acquire
    assert "WaitForSingleObject" in acquire
    assert "_impl->gpuDescriptorHeapPool->beginFrameAllocationRange" in acquire
    assert acquire.index("frameResources.fence->SetEventOnCompletion") < acquire.index(
        "_impl->gpuDescriptorHeapPool->beginFrameAllocationRange"
    )
    assert "waitForSubmittedFence" not in acquire


def test_three_frame_pipeline_keeps_swapchain_allocators_and_transient_resources_in_sync() -> None:
    device = read("D3D12Device.cpp")
    command_buffer = read("D3D12CommandBuffer.cpp")
    swapchain = read("D3D12Swapchain.cpp")
    header = read("D3D12Device.h")
    acquire = function_body(device, "void CCD3D12Device::acquire")
    begin = function_body(command_buffer, "void CCD3D12CommandBuffer::begin")

    assert "D3D12_MAX_FRAMES_IN_FLIGHT{3}" in header
    assert "D3D12_MAX_FRAMES_IN_FLIGHT" in device
    assert "D3D12_MAX_FRAMES_IN_FLIGHT" in swapchain
    assert "std::array<FrameResources, D3D12_MAX_FRAMES_IN_FLIGHT>" in device
    assert "activeFrameResource" in device
    assert "framesInCurrentBatch" not in acquire
    assert "D3D12_GPU_DESCRIPTORS_PER_FRAME" in acquire
    submit = function_body(device, "void CCD3D12Device::notifySubmittedFence")
    assert "frameResources.fenceValue = value" in submit
    assert "CommandRecordingContext recordingContexts[D3D12_MAX_FRAMES_IN_FLIGHT]" in command_buffer
    assert "executedBundles[D3D12_MAX_FRAMES_IN_FLIGHT]" in command_buffer
    assert "% D3D12_MAX_FRAMES_IN_FLIGHT" in command_buffer
    assert "activeRecordingContext" in begin
    assert begin.index("activeRecordingContext") < begin.index("waitForFenceValue")


def test_frame_slot_reuse_owns_sampler_and_upload_resets() -> None:
    body = function_body(read("D3D12Device.cpp"), "void CCD3D12Device::waitForGpu")
    assert "gpuDescriptorHeapPool" not in body
    assert "samplerDescriptorHeapPool" not in body
    assert "uploadPages" not in body

    retire = function_body(read("D3D12Device.cpp"), "void CCD3D12Device::retireFrameResources")
    assert "gpuDescriptorHeapPool->reset()" not in retire
    assert "samplerDescriptorHeapPool->reset()" not in retire

    acquire = function_body(read("D3D12Device.cpp"), "void CCD3D12Device::acquire")
    assert "frameResources.samplerDescriptorHeapPool->reset()" in acquire
    assert "frameResources.uploadPages" in acquire
    assert "beginFrameAllocationRange" in acquire


def test_buffers_do_not_always_use_upload_heap() -> None:
    body = function_body(read("D3D12Buffer.cpp"), "bool CCD3D12Buffer::createResource")
    assert "D3D12_HEAP_TYPE_DEFAULT" in body
    assert "_memUsage" in body
    assert "BufferUsageBit::UNIFORM" in body


def test_uniform_updates_copy_to_stable_backing_not_transient_ring_resource() -> None:
    body = function_body(read("D3D12CommandBuffer.cpp"), "void CCD3D12CommandBuffer::updateBuffer")
    assert "CreateCommittedResource" not in body
    assert "allocateUploadBuffer" in body
    assert "CopyBufferRegion" in body
    assert "replaceD3D12Resource" not in body
    assert "forceUpdate" not in body
    assert "replaceD3D12Resource" not in read("D3D12Buffer.h")


def test_hot_uniform_updates_use_fence_safe_transient_upload_descriptors() -> None:
    buffer_header = read("D3D12Buffer.h")
    buffer = read("D3D12Buffer.cpp")
    descriptor_set = read("D3D12DescriptorSet.cpp")
    device_header = read("D3D12Device.h")
    update = function_body(buffer, "void CCD3D12Buffer::update")
    assert "bool CCD3D12Buffer::ensureTransientUniformUpload" in buffer
    ensure_upload = function_body(buffer, "bool CCD3D12Buffer::ensureTransientUniformUpload")
    descriptor_update = function_body(descriptor_set, "void CCD3D12DescriptorSet::update")
    descriptor_force_update = function_body(descriptor_set, "void CCD3D12DescriptorSet::forceUpdate")
    device_flush = function_body(read("D3D12Device.cpp"), "void CCD3D12Device::flushPendingBufferUpdates")

    assert "ensureTransientUniformUpload" in buffer_header
    assert "getUniformDescriptorVersion" in buffer_header
    assert "getD3D12UniformGPUVirtualAddress" in buffer_header
    assert "isTransientUniformEligible" in update
    assert "pendingData" in update
    assert "enqueueBufferUpdate" in update  # exact DEFAULT-resource fallback remains
    assert "allocateUploadBuffer" in ensure_upload
    assert "getBufferStateEpoch" in ensure_upload
    assert "D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT" in ensure_upload
    assert "uploadedContentVersion" in ensure_upload
    assert "getPendingTransientUniformUploadSize" in buffer_header
    assert "flushTransientUniformUpload" in buffer_header
    assert "transientUniformBytes" in device_flush
    assert "allocateUploadBuffer" in device_flush
    assert "flushTransientUniformUpload" in device_flush
    assert "getTransientUniformUploadGeneration" in device_header
    assert "notifyTransientUniformUpload" in device_header
    assert "observedTransientUniformUploadGeneration" in descriptor_update
    assert "cbvSrvUavOffset" in descriptor_update
    assert "CreateConstantBufferView" in descriptor_update
    assert "ensureTransientUniformUpload" not in descriptor_update
    assert "ensureTransientUniformUpload" in descriptor_force_update
    assert "getUniformDescriptorVersion" in descriptor_update
    assert "getD3D12UniformGPUVirtualAddress" in descriptor_force_update
    assert "getD3D12ConstantBufferSize" in descriptor_force_update
    assert "hasBufferViews" in buffer


def test_direct_buffer_updates_are_deferred_to_main_command_list() -> None:
    update = function_body(read("D3D12Buffer.cpp"), "void CCD3D12Buffer::update")
    for forbidden in [
        "CreateCommandAllocator",
        "CreateCommandList",
        "CreateFence",
        "CreateEvent",
        "ExecuteCommandLists",
        "WaitForSingleObject",
        "CopyBufferRegion",
    ]:
        assert forbidden not in update, f"CCD3D12Buffer::update still contains {forbidden}"
    assert "enqueueBufferUpdate" in update

    flush = function_body(
        read("D3D12Device.cpp"),
        "void CCD3D12Device::flushPendingBufferUpdates",
    )
    assert "flushPendingUpdate(commandBuffer)" in flush

    command_buffer = read("D3D12CommandBuffer.cpp")
    for signature in [
        "void CCD3D12CommandBuffer::draw(const DrawInfo &info)",
        "void CCD3D12CommandBuffer::dispatch",
        "void CCD3D12CommandBuffer::execute",
        "void CCD3D12CommandBuffer::end()",
    ]:
        body = function_body(command_buffer, signature)
        assert "flushPendingBufferUpdates" in body, f"{signature} does not drain deferred buffer updates"


def test_deferred_buffer_queue_does_not_own_validator_actor() -> None:
    device = read("D3D12Device.cpp")
    destroy = function_body(read("D3D12Buffer.cpp"), "void CCD3D12Buffer::doDestroy")

    assert "ccstd::vector<CCD3D12Buffer *> pendingBufferUpdates" in device
    assert "ccstd::vector<IntrusivePtr<CCD3D12Buffer>> pendingBufferUpdates" not in device
    assert "discardPendingBufferUpdate(this)" in destroy


def test_texture_uploads_use_upload_ring_not_region_committed_resources() -> None:
    for file_name, signature in [
        ("D3D12CommandBuffer.cpp", "void CCD3D12CommandBuffer::copyBuffersToTexture"),
        ("D3D12Device.cpp", "void CCD3D12Device::copyBuffersToTextureImmediate"),
    ]:
        body = function_body(read(file_name), signature)
        assert "CreateCommittedResource" not in body
        assert "allocateUploadBuffer" in body


def test_command_list_resource_retention_uses_constant_time_deduplication() -> None:
    source = read("D3D12CommandBuffer.cpp")
    retain = function_body(source, "void retainCommandListResource")

    assert "unordered_set<ID3D12Resource *>" in source
    assert ".emplace(resource)" in retain
    assert "find_if" not in retain
    assert "pendingUploadResourceSet" in source


def test_local_root_table_split_has_shared_layout_and_descriptor_partition_contract() -> None:
    descriptor_set = read("D3D12DescriptorSet.cpp")
    descriptor_header = read("D3D12DescriptorSet.h")
    layout = read("D3D12PipelineLayout.cpp")
    layout_header = read("D3D12PipelineLayout.h")

    assert "getCbvSrvUavPartition" in descriptor_header
    assert "getCbvSrvUavPartition" in descriptor_set
    assert "D3D12_LOCAL_DESCRIPTOR_SET_INDEX" in layout
    assert "dynamicCbvSrvUavRootParameterIndices" in layout
    assert "staticCbvSrvUavRootParameterIndices" in layout
    assert "getDynamicCbvSrvUavRootParameterIndex" in layout_header
    assert "getStaticCbvSrvUavRootParameterIndex" in layout_header


def test_local_root_table_split_caches_only_exact_static_resources() -> None:
    descriptor_header = read("D3D12DescriptorSet.h")
    descriptor_set = read("D3D12DescriptorSet.cpp")
    command_buffer = read("D3D12CommandBuffer.cpp")
    flush = function_body(
        command_buffer,
        "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental",
    )

    assert "hasMatchingStaticCbvSrvUavResources" in descriptor_header
    assert "hasMatchingStaticCbvSrvUavResources" in descriptor_set
    assert "localStaticCbvSrvUavTableCache" in command_buffer
    assert "findCachedLocalStaticCbvSrvUavTable" in flush
    assert "cacheLocalStaticCbvSrvUavTable" in flush
    assert "getDynamicCbvSrvUavRootParameterIndex" in flush
    assert "getStaticCbvSrvUavRootParameterIndex" in flush
    assert "dynamicCbvSrvUavRange" in flush
    assert "staticCbvSrvUavRange" in flush


def test_local_static_table_cache_skips_non_reusable_suffixes() -> None:
    descriptor_header = read("D3D12DescriptorSet.h")
    descriptor_set = read("D3D12DescriptorSet.cpp")
    command_buffer = read("D3D12CommandBuffer.cpp")
    flush = function_body(
        command_buffer,
        "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental",
    )

    assert "canReuseStaticCbvSrvUavResources" in descriptor_header
    assert "canReuseStaticCbvSrvUavResources" in descriptor_set
    assert "binding.set->canReuseStaticCbvSrvUavResources()" in flush


def test_local_b0_root_cbv_moves_only_the_changing_uniform_out_of_the_static_table() -> None:
    descriptor_set = read("D3D12DescriptorSet.cpp")
    layout = read("D3D12PipelineLayout.cpp")
    layout_header = read("D3D12PipelineLayout.h")
    command_buffer = read("D3D12CommandBuffer.cpp")
    flush = function_body(
        command_buffer,
        "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental",
    )

    # The target scene proves that only local b0 changes per draw. The root
    # signature must therefore expose exactly b0 as a root CBV and preserve
    # the remaining CBV/SRV/UAV descriptors as an exact static suffix.
    assert "D3D12_ROOT_PARAMETER_TYPE_CBV" in layout
    assert "getLocalRootCbvParameterIndex" in layout_header
    assert "binding.binding == 0" in layout
    assert "binding.count == 1" in layout
    assert "staticDescriptorChanged" in descriptor_set
    assert "SetGraphicsRootConstantBufferView" in flush
    assert "localRootCbvGpuAddress" in flush


def test_local_root_cbv_writes_all_local_cbvs_only_to_the_shader_visible_heap() -> None:
    descriptor_header = read("D3D12DescriptorSet.h")
    descriptor_set = read("D3D12DescriptorSet.cpp")
    command_buffer = read("D3D12CommandBuffer.cpp")
    flush = function_body(
        command_buffer,
        "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental",
    )

    assert "updateForLocalRootCbv" in descriptor_header
    assert "void CCD3D12DescriptorSet::updateForLocalRootCbv" in descriptor_set
    assert "slot.cbvSrvUavOffset == _impl->staticDescriptorMetadata.rootCbvDescriptorOffset" in descriptor_set
    assert "skipStaticCbvStaging" in descriptor_set
    assert "set->updateForLocalRootCbv(true);" in flush
    assert "copyLocalStaticCbvRange" in flush
    assert "CreateConstantBufferView(&cbvDesc, destination)" in flush
    assert "getUniformDescriptorSignature" in flush


def test_local_root_cbv_split_accepts_b0_at_an_arbitrary_descriptor_offset() -> None:
    descriptor_set = read("D3D12DescriptorSet.cpp")
    descriptor_header = read("D3D12DescriptorSet.h")
    layout = read("D3D12PipelineLayout.cpp")
    command_buffer = read("D3D12CommandBuffer.cpp")
    flush = function_body(
        command_buffer,
        "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental",
    )

    assert "rootCbvDescriptorOffset" in descriptor_header
    assert "staticCbvSrvUavTableCount" in descriptor_set
    assert "copyLocalStaticCbvTable" in flush
    # The static table is compacted by walking every source slot and excluding
    # the root-CBV offset; it must not rely on b0 being the first descriptor.
    assert "sourceOffset == rootCbvOffset" in flush
    assert "destinationOffset == descriptorOffset + binding.staticCbvSrvUavCount" in flush
    assert "if (!localRootCbvPrefixSeen ||" not in layout


def test_local_root_split_separates_dynamic_buffer_slots_from_cacheable_static_table() -> None:
    descriptor_header = read("D3D12DescriptorSet.h")
    descriptor_set = read("D3D12DescriptorSet.cpp")
    layout = read("D3D12PipelineLayout.cpp")
    command_buffer = read("D3D12CommandBuffer.cpp")
    flush = function_body(
        command_buffer,
        "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental",
    )

    assert "getDynamicDescriptorOffset" in descriptor_header
    assert "getDynamicDescriptorSlotCount" in descriptor_set
    assert "dynamicBufferCbvSrvUavRanges" in layout
    assert "dynamicDescriptorCount" in flush
    assert "copyLocalDynamicCbvSrvUavTable" in flush
    assert "dynamicDescriptorCount + binding.staticCbvSrvUavCount" in flush


def test_local_null_dynamic_table_is_cached_once_per_descriptor_heap_epoch() -> None:
    descriptor_header = read("D3D12DescriptorSet.h")
    descriptor_set = read("D3D12DescriptorSet.cpp")
    command_buffer = read("D3D12CommandBuffer.cpp")
    flush = function_body(
        command_buffer,
        "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental",
    )

    assert "hasOnlyNullDynamicDescriptorSources" in descriptor_header
    assert "_buffers[slot.descriptorIndex].ptr" in descriptor_set
    assert "CachedLocalNullDynamicCbvSrvUavTable" in command_buffer
    assert "hasOnlyNullDynamicDescriptorSources()" in flush
    assert "localNullDynamicCbvSrvUavTable" in flush


def test_local_root_cbv_fast_validation_skips_staging_heap_work() -> None:
    descriptor_set = read("D3D12DescriptorSet.cpp")
    update = function_body(
        descriptor_set,
        "void CCD3D12DescriptorSet::updateForLocalRootCbv",
    )

    assert update.index("if (skipStaticCbvStaging)") < update.index("_impl->ensureStagingAllocations(device)")
    assert "slot.cbvSrvUavOffset == _impl->staticDescriptorMetadata.rootCbvDescriptorOffset" in update
    assert "if (_isDirty) {\n            forceUpdate();\n        }\n        return;" in update


def test_pending_default_buffer_updates_batch_resource_barriers() -> None:
    device = read("D3D12Device.cpp")
    command_buffer = read("D3D12CommandBuffer.cpp")
    flush_pending = function_body(device, "void CCD3D12Device::flushPendingBufferUpdates")

    assert "void CCD3D12CommandBuffer::startBufferUpdateBatch" in command_buffer
    assert "void CCD3D12CommandBuffer::finishBufferUpdateBatch" in command_buffer
    assert flush_pending.index("startBufferUpdateBatch") < flush_pending.index("flushPendingUpdate")
    assert flush_pending.index("flushPendingUpdate") < flush_pending.index("finishBufferUpdateBatch")

    update_buffer = function_body(command_buffer, "void CCD3D12CommandBuffer::updateBuffer")
    end_batch = function_body(command_buffer, "void CCD3D12CommandBuffer::finishBufferUpdateBatch")
    assert "bufferUpdateBatchActive" in update_buffer
    assert "pendingDefaultBufferCopies" in update_buffer
    assert "preCopyBarriers" in end_batch
    assert "postCopyBarriers" in end_batch
    assert "ResourceBarrier(static_cast<UINT>(preCopyBarriers.size())" in end_batch
    assert "ResourceBarrier(static_cast<UINT>(postCopyBarriers.size())" in end_batch


def test_batched_buffer_updates_keep_alias_copies_sequential() -> None:
    command_buffer = read("D3D12CommandBuffer.cpp")
    update_buffer = function_body(command_buffer, "void CCD3D12CommandBuffer::updateBuffer")
    finish_batch = function_body(command_buffer, "void CCD3D12CommandBuffer::finishBufferUpdateBatch")

    assert "copyCount{1}" in command_buffer
    assert "pendingDefaultBufferTransitions[transitionIndex].copyCount" in update_buffer
    assert "transition.copyCount != 1" in finish_batch
    assert "transition.copyCount == 1" in finish_batch
    assert "CopyBufferRegion" in finish_batch


def test_buffer_update_batch_reuses_transition_indices_and_barrier_scratch() -> None:
    command_buffer = read("D3D12CommandBuffer.cpp")
    finish_batch = function_body(command_buffer, "void CCD3D12CommandBuffer::finishBufferUpdateBatch")

    assert "copy.transitionIndex" in finish_batch
    assert "pendingDefaultBufferTransitionIndices.find(copy.destination)" not in finish_batch
    assert "bufferUpdatePreCopyBarriers" in command_buffer
    assert "bufferUpdatePostCopyBarriers" in command_buffer
    assert "ccstd::vector<D3D12_RESOURCE_BARRIER> preCopyBarriers;" not in finish_batch
    assert "ccstd::vector<D3D12_RESOURCE_BARRIER> postCopyBarriers;" not in finish_batch


def test_unique_non_view_buffer_batch_skips_transition_hash_map() -> None:
    device = read("D3D12Device.cpp")
    command_buffer = read("D3D12CommandBuffer.cpp")
    header = read("D3D12CommandBuffer.h")
    flush_pending = function_body(device, "void CCD3D12Device::flushPendingBufferUpdates")
    start_batch = function_body(command_buffer, "void CCD3D12CommandBuffer::startBufferUpdateBatch")
    update_buffer = function_body(command_buffer, "void CCD3D12CommandBuffer::updateBuffer")

    assert "std::all_of" in flush_pending
    assert "commandBuffer->getType() == CommandBufferType::PRIMARY" in flush_pending
    assert "!buffer->isBufferView()" in flush_pending
    assert "startBufferUpdateBatch(destinationsAreUnique)" in flush_pending
    assert "startBufferUpdateBatch(bool destinationsAreUnique)" in header
    assert "bufferUpdateBatchDestinationsAreUnique = destinationsAreUnique" in start_batch
    direct_index = update_buffer.index("pendingDefaultBufferTransitions.size()")
    hash_lookup = update_buffer.index("pendingDefaultBufferTransitionIndices.try_emplace")
    assert direct_index < hash_lookup
    assert "bufferUpdateBatchDestinationsAreUnique" in update_buffer
    assert "copyCount != 1" in function_body(command_buffer, "void CCD3D12CommandBuffer::finishBufferUpdateBatch")


def test_unique_buffer_batch_retains_owning_resources_without_generic_set() -> None:
    command_buffer = read("D3D12CommandBuffer.cpp")
    update_buffer = function_body(command_buffer, "void CCD3D12CommandBuffer::updateBuffer")

    fast_condition = "_impl->bufferUpdateBatchActive && _impl->bufferUpdateBatchDestinationsAreUnique"
    fast_start = update_buffer.index(fast_condition)
    generic_retain = update_buffer.index("retainCommandListResource", fast_start)
    owning_retain = update_buffer.index("_impl->pendingUploadResources.push_back", fast_start)
    assert fast_start < owning_retain < generic_retain
    assert "Microsoft::WRL::ComPtr<ID3D12Resource> retained" in update_buffer[fast_start:generic_retain]
    assert "retained = resource" in update_buffer[fast_start:generic_retain]
    assert "else" in update_buffer[owning_retain:generic_retain]


def test_query_fetch_reuses_d3d12_objects() -> None:
    body = function_body(read("D3D12QueryPool.cpp"), "void CCD3D12QueryPool::fetchResults")
    for forbidden in ["CreateCommandAllocator", "CreateCommandList", "CreateFence", "CreateEvent"]:
        assert forbidden not in body, f"D3D12QueryPool::fetchResults still contains {forbidden}"
    assert "commandAllocator->Reset" in body
    assert "commandList->Reset" in body


def test_dynamic_pso_repeated_state_is_cached() -> None:
    body = function_body(read("D3D12CommandBuffer.cpp"), "void CCD3D12CommandBuffer::applyDynamicPipelineState")
    assert "dynamicPipelineStateValid" in body
    assert "lastDynamicPipelineStateOwner" in body


def test_background_cache_probe_skips_legacy_v3_migration() -> None:
    body = function_body(
        read("D3D12Shader.cpp"),
        "bool CCD3D12Shader::compileGLSLToDXBC",
    )
    v4_lookup = body.index("fileCacheHit = loadFileCachedDXBC(cacheKey, outDXBC);")
    legacy_policy = body.index("else if (cacheLookupPolicy.probeLegacyV3)")
    legacy_v3_lookup = body.index("legacyFileCacheHit = loadFileCachedDXBC")

    assert v4_lookup < legacy_policy < legacy_v3_lookup


def test_debug_d3d12_shader_compilation_keeps_debug_diagnostics_and_optimized_codegen() -> None:
    body = function_body(
        read("D3D12Shader.cpp"),
        "bool CCD3D12Shader::compileGLSLToDXBC",
    )

    assert "D3DCOMPILE_OPTIMIZATION_LEVEL3" in body
    assert "D3DCOMPILE_DEBUG" in body
    assert "D3DCOMPILE_SKIP_OPTIMIZATION" not in body


def test_legacy_v3_migration_defers_only_file_persistence() -> None:
    body = function_body(
        read("D3D12Shader.cpp"),
        "bool CCD3D12Shader::compileGLSLToDXBC",
    )
    migration_start = body.index("legacyFileCacheHit = loadFileCachedDXBC")
    migration_end = body.index("cacheHitBackend = \"file-v3-migrated\"")
    migration = body[migration_start:migration_end]

    assert "storeD3D12ShaderCacheDXBC(cacheKey, outDXBC)" in migration
    assert "scheduleFileCachedDXBCPersistence(cacheKey, outDXBC)" in migration
    assert "storeCachedDXBC(cacheKey, outDXBC)" not in migration


def test_device_lifecycle_gates_shader_cache_persistence() -> None:
    init_body = function_body(read("D3D12Device.cpp"), "bool CCD3D12Device::doInit")
    destroy_body = function_body(read("D3D12Device.cpp"), "void CCD3D12Device::doDestroy")

    assert "reopenD3D12ShaderCachePersistence();" in init_body
    drain = destroy_body.index("drainD3D12ShaderCachePersistence();")
    session_reset = destroy_body.index("shaderCacheSession.Reset();")
    assert drain < session_reset


def test_shader_cache_store_is_idempotent_under_session_mutex() -> None:
    body = function_body(
        read("D3D12Device.cpp"),
        "bool CCD3D12Device::storeShaderCacheValue",
    )
    mutex = body.index("std::lock_guard<std::mutex> lock(_impl->shaderCacheMutex);")
    find = body.index("_impl->shaderCacheSession->FindValue")
    store = body.index("_impl->shaderCacheSession->StoreValue")

    assert mutex < find < store
    assert "SUCCEEDED(findHr) && existingValueSize > 0" in body


def test_color_attachments_supply_optimized_clear_value() -> None:
    body = function_body(
        read("D3D12Texture.cpp"),
        "bool CCD3D12Texture::createResource",
    )
    clear_setup_start = body.index("D3D12_CLEAR_VALUE clearValue")
    create = body.index("CreateCommittedResource", clear_setup_start)
    color_setup = body[clear_setup_start:create]

    assert "TextureUsageBit::COLOR_ATTACHMENT" in color_setup
    assert "clearValue.Format = viewFormat" in color_setup
    for component in range(4):
        assert f"clearValue.Color[{component}] = 0.0F" in color_setup
    assert "optimizedClearValue = &clearValue" in color_setup


def test_descriptor_set_recovers_staging_allocations_before_writes() -> None:
    body = function_body(
        read("D3D12DescriptorSet.cpp"),
        "void CCD3D12DescriptorSet::forceUpdate",
    )
    readiness = body.index("_impl->ensureStagingAllocations(device)")
    binding_walk = body.index("for (const auto &binding : bindings)")
    failure_path = body[readiness:binding_walk]

    assert readiness < binding_walk
    assert "_isDirty = true" in failure_path
    assert "cbvSrvUavAllocation.cpuHandle" in failure_path
    assert "samplerAllocation.cpuHandle" in failure_path
    assert "cbvSrvUavCpuStart" in failure_path
    assert "samplerCpuStart" in failure_path


def test_descriptor_sets_suballocate_cpu_staging_descriptors() -> None:
    descriptor_set = read("D3D12DescriptorSet.cpp")
    device = read("D3D12Device.cpp")

    assert "CreateDescriptorHeap" not in descriptor_set
    assert "getCPUDescriptorHeapPool" in descriptor_set
    assert "getCPUSamplerDescriptorHeapPool" in descriptor_set
    assert "cpuDescriptorHeapPool" in device
    assert "cpuSamplerDescriptorHeapPool" in device
    assert "HeapType::CBV_SRV_UAV, 16384, false" in device
    assert "HeapType::SAMPLER, 2048, false" in device
    command_buffer = read("D3D12CommandBuffer.cpp")
    assert "getCbvSrvUavCPUDescriptorHandle" in command_buffer
    assert "getSamplerCPUDescriptorHandle" in command_buffer


def test_local_static_descriptor_metadata_is_built_on_update_not_per_draw() -> None:
    descriptor_set = read("D3D12DescriptorSet.cpp")
    force_update = function_body(descriptor_set, "void CCD3D12DescriptorSet::forceUpdate")
    partition = function_body(descriptor_set, "bool CCD3D12DescriptorSet::getCbvSrvUavPartition")
    analysis = function_body(descriptor_set, "void CCD3D12DescriptorSet::getStaticDescriptorAnalysis")
    exact_match = function_body(descriptor_set, "bool CCD3D12DescriptorSet::hasMatchingStaticCbvSrvUavResources")

    assert "refreshStaticDescriptorMetadata" in descriptor_set
    assert "refreshStaticDescriptorMetadata();" in force_update
    assert "staticDescriptorMetadata" in descriptor_set
    assert "staticResourceIdentity" in descriptor_set
    assert "_layout->getBindings()" not in partition
    assert "_layout->getBindings()" not in analysis
    assert "_layout->getBindings()" not in exact_match
    assert "staticResourceIdentity == other._impl->staticDescriptorMetadata.staticResourceIdentity" in exact_match


def test_command_buffer_caches_redundant_graphics_state() -> None:
    command_buffer = read("D3D12CommandBuffer.cpp")
    input_assembler = read("D3D12InputAssembler.cpp")
    buffer_header = read("D3D12Buffer.h")
    buffer = read("D3D12Buffer.cpp")
    bind_pso = function_body(
        command_buffer,
        "void CCD3D12CommandBuffer::bindPipelineState",
    )
    bind_ia = function_body(
        command_buffer,
        "void CCD3D12CommandBuffer::bindInputAssembler",
    )
    viewport = function_body(
        command_buffer,
        "void CCD3D12CommandBuffer::setViewport",
    )
    scissor = function_body(
        command_buffer,
        "void CCD3D12CommandBuffer::setScissor",
    )

    assert "boundNativePipelineState != d3d12PipelineState" in bind_pso
    assert "boundRootSignature != rootSig" in bind_pso
    assert "boundPrimitiveTopology != topology" in bind_pso
    assert "boundBlendFactor" in bind_pso
    assert "boundStencilRef" in bind_pso
    assert "if (!logicalPipelineChanged)" in bind_pso
    assert bind_pso.index("if (!logicalPipelineChanged)") < bind_pso.index("getID3D12PipelineState")
    assert "applyDynamicPipelineState();" in bind_pso
    assert "memcmp" in bind_ia
    assert "refreshBufferViews()" in bind_ia
    assert "if (!vertexBuffersChanged && !indexBufferChanged)" in bind_ia
    assert "localRootCbvBatchActive && (vertexBuffersChanged || indexBufferChanged)" in bind_ia
    assert "getD3D12ResourceVersion" in buffer_header
    assert "getD3D12ResourceVersion" in input_assembler
    assert "++_impl->resourceVersion" in buffer
    assert "viewportValid" in viewport
    assert "scissorValid" in scissor


def test_pso_file_cache_key_includes_serialized_root_signature_identity() -> None:
    pipeline_layout_header = read("D3D12PipelineLayout.h")
    pipeline_layout = read("D3D12PipelineLayout.cpp")
    pipeline_state = read("D3D12PipelineState.cpp")

    assert "getRootSignatureHash" in pipeline_layout_header
    assert "signatureBlob->GetBufferPointer()" in pipeline_layout
    assert "getRootSignatureHash()" in pipeline_state
    assert "rootSignatureHash" in function_body(
        pipeline_state,
        "ccstd::string makeGraphicsPSOCacheKey",
    )


def test_shader_blit_invalidates_cached_graphics_state() -> None:
    blit = function_body(
        read("D3D12CommandBuffer.cpp"),
        "void CCD3D12CommandBuffer::blitTexture",
    )

    assert "graphicsStateClobbered" in blit
    assert blit.index("graphicsStateClobbered = true") < blit.index("shaderBlitRegion")
    assert "if (graphicsStateClobbered)" in blit
    assert "invalidateGraphicsState()" in blit


def test_descriptor_flush_is_versioned_and_incremental() -> None:
    descriptor_set = read("D3D12DescriptorSet.cpp")
    descriptor_header = read("D3D12DescriptorSet.h")
    command_buffer = read("D3D12CommandBuffer.cpp")
    bind_set = function_body(
        command_buffer,
        "void CCD3D12CommandBuffer::bindDescriptorSet",
    )
    flush = function_body(
        command_buffer,
        "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental",
    )

    assert "getVersion" in descriptor_header
    assert "++_impl->version" in descriptor_set
    assert "sameDynamicOffsets" in bind_set
    assert "getVersion()" in bind_set
    assert "d3d12Set->update()" not in bind_set
    assert "set->update()" in flush
    assert "gpuDescriptorCache" in flush
    assert "boundCbvSrvUavHeap != cbvHeap" in flush
    assert "boundSamplerHeap != samplerHeap" in flush
    assert "boundRootTables" in flush
    device = read("D3D12Device.cpp")
    assert "D3D12_GPU_DESCRIPTORS_PER_FRAME" in device
    assert "D3D12_GPU_DESCRIPTORS_PER_FRAME * D3D12_MAX_FRAMES_IN_FLIGHT" in device


def test_dynamic_offset_restore_rewrites_only_dynamic_descriptors() -> None:
    restore = function_body(
        read("D3D12DescriptorSet.cpp"),
        "void CCD3D12DescriptorSet::restoreDynamicOffsetDescriptors",
    )
    assert "forceUpdate" not in restore
    assert "applyDynamicOffsets" in restore
    assert "zeroOffsets" in restore


def test_dynamic_offset_updates_use_cached_descriptor_slots() -> None:
    source = read("D3D12DescriptorSet.cpp")
    apply = function_body(
        source,
        "void CCD3D12DescriptorSet::applyDynamicOffsets",
    )

    assert "dynamicDescriptorSlots" in source
    assert "for (const auto &slot : _impl->dynamicDescriptorSlots)" in apply
    assert "_layout->getBindings()" not in apply
    assert "_layout->getDescriptorIndices()" not in apply


def test_empty_descriptor_sets_do_not_consume_root_parameters() -> None:
    body = function_body(
        read("D3D12PipelineLayout.cpp"),
        "void CCD3D12PipelineLayout::doInit",
    )
    assert "Empty set layout: still create" not in body
    assert "dummyRange" not in body


def test_d3d12_debug_layer_is_explicitly_opt_in() -> None:
    source = read("D3D12Device.cpp")
    body = function_body(
        source,
        "bool CCD3D12Device::initializeD3D12Context",
    )

    assert "CC_D3D12_DEBUG_LAYER" in source
    assert "isD3D12DebugLayerRequested()" in body
    assert "if (isD3D12DebugLayerRequested())" in body
    assert "debug layer enabled by CC_D3D12_DEBUG_LAYER" in body


def test_buffer_state_epoch_tracks_execute_submission_boundary() -> None:
    device = read("D3D12Device.cpp")
    queue = read("D3D12Queue.cpp")
    buffer = read("D3D12Buffer.cpp")
    command_buffer = read("D3D12CommandBuffer.cpp")
    acquire = function_body(device, "void CCD3D12Device::acquire")
    submit = function_body(queue, "void CCD3D12Queue::submit")
    get_state = function_body(buffer, "D3D12_RESOURCE_STATES CCD3D12Buffer::getCurrentState")
    update_buffer = function_body(command_buffer, "void CCD3D12CommandBuffer::updateBuffer")

    empty_check = submit.index("if (commandLists.empty()) return")
    execute = submit.index("graphicsQueue->ExecuteCommandLists")
    advance = submit.index("device->advanceBufferStateEpoch")
    signal = submit.index("graphicsQueue->Signal")
    assert empty_check < execute < advance < signal
    assert submit.count("advanceBufferStateEpoch") == 1
    assert "advanceBufferStateEpoch" not in acquire
    assert "getBufferStateEpoch" in get_state
    assert "D3D12_RESOURCE_STATE_COMMON" in get_state
    assert "previousState != D3D12_RESOURCE_STATE_COMMON" in update_buffer


def test_sampler_table_cache_reuses_only_exact_owned_keys() -> None:
    command_buffer = read("D3D12CommandBuffer.cpp")
    begin = function_body(command_buffer, "void CCD3D12CommandBuffer::begin")
    flush = function_body(command_buffer, "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental")

    assert "struct CachedSamplerTable" in command_buffer
    assert "ccstd::vector<uint32_t> key" in command_buffer
    assert "std::unordered_map<uint64_t, ccstd::vector<CachedSamplerTable>> samplerTableCache" in command_buffer
    assert "samplerTableCache.clear()" in begin
    assert "entry.key == samplerTableKey" in flush
    assert flush.index("entry.key == samplerTableKey") < flush.index("const auto allocation = pool->allocate(count)")
    # The local-only fast path may probe an already-populated sampler table;
    # retain the update-before-key ordering for the conservative full path.
    full_flush = flush[flush.index("SetBindingInfo bindings"):]
    assert full_flush.index("set->update()") < full_flush.index("getSamplerTableKey()")
    assert "cached.heap != _impl->samplerTableCacheHeap" in flush
    assert "cacheSamplerTableRange" in flush
    assert flush.count("cacheSamplerTableRange") >= 3
    assert "cached = {binding.version" in flush
    assert "hashBucket.emplace_back" in flush
    assert "std::unordered_map<uint64_t, CachedGpuDescriptorRange>" not in command_buffer


def test_descriptor_flush_uses_bounded_set_index_cache() -> None:
    command_buffer = read("D3D12CommandBuffer.cpp")
    flush = function_body(
        command_buffer,
        "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental",
    )

    assert "CCD3D12DescriptorSet *owner" in command_buffer
    assert "CachedGpuDescriptorSet gpuDescriptorCache[D3D12_MAX_BOUND_SETS]" in command_buffer
    assert "gpuDescriptorCache[pending.setIndex]" in flush
    assert "cachedSet.owner != set" in flush
    assert "cachedSet.reset(set)" in flush
    assert "Impl::CachedGpuDescriptorSet *cachedSet" in flush
    assert "gpuDescriptorCache.reserve" not in flush
    assert "gpuDescriptorCache.try_emplace" not in flush
    assert "gpuDescriptorCache.find(binding.set)" not in flush
    assert "gpuDescriptorCache[binding.set]" not in flush


def test_descriptor_heap_pool_caches_immutable_start_handles() -> None:
    source = read("D3D12DescriptorHeapPool.cpp")
    allocate = function_body(source, "D3D12DescriptorHeapPool::Allocation D3D12DescriptorHeapPool::allocate")
    deallocate = function_body(source, "void D3D12DescriptorHeapPool::deallocate")

    assert "D3D12_CPU_DESCRIPTOR_HANDLE cpuStart" in source
    assert "D3D12_GPU_DESCRIPTOR_HANDLE gpuStart" in source
    assert "heap.cpuStart" in allocate
    assert "heap.gpuStart" in allocate
    assert allocate.count("GetCPUDescriptorHandleForHeapStart") == 1
    assert allocate.count("GetGPUDescriptorHandleForHeapStart") == 1
    assert "GetCPUDescriptorHandleForHeapStart" not in deallocate


def test_dynamic_descriptor_table_cache_keys_exact_offsets() -> None:
    command_buffer = read("D3D12CommandBuffer.cpp")
    flush = function_body(
        command_buffer,
        "bool CCD3D12CommandBuffer::flushDescriptorSetsIncremental",
    )

    assert "cbvDynamicOffsets" in command_buffer
    assert "cachedSet->cbvDynamicOffsets == *binding.dynamicOffsets" in flush
    assert "hasDynamicOffsets && !dynamicCbvCacheHit" in flush
    assert "binding.cachedSet->cbvDynamicOffsets = *binding.dynamicOffsets" in flush
    assert "cacheable = sampler || !hasDynamicOffsets" not in flush


def test_validator_skips_invariant_layout_and_empty_offset_work() -> None:
    header = (RENDERER / "gfx-validator" / "CommandBufferValidator.h").read_text(encoding="utf-8")
    source = (RENDERER / "gfx-validator" / "CommandBufferValidator.cpp").read_text(encoding="utf-8")
    bind_pso = function_body(source, "void CommandBufferValidator::bindPipelineState")
    bind_set = function_body(source, "void CommandBufferValidator::bindDescriptorSet")
    draw = function_body(source, "void CommandBufferValidator::drawInternal")
    assert "_drawLayoutsDirty" in header
    assert "_currentPipelineLayout" in header
    assert "_drawLayoutsDirty" in bind_pso
    assert "_curStates.pipelineState->getPipelineLayout" not in bind_pso
    assert "_drawLayoutsDirty" in bind_set
    assert "dynamicOffsetCount > 0" in bind_set
    assert "bindingMappingInfo" not in bind_set
    assert "if (_drawLayoutsDirty)" in draw
    assert "_drawLayoutsDirty = false" in draw


def test_executed_bundle_lifetime_follows_primary_submission_fence() -> None:
    command_buffer = read("D3D12CommandBuffer.cpp")
    begin = function_body(command_buffer, "void CCD3D12CommandBuffer::begin")
    notify = function_body(command_buffer, "void CCD3D12CommandBuffer::notifySubmitted")
    execute = function_body(command_buffer, "void CCD3D12CommandBuffer::execute")

    assert "ccstd::vector<IntrusivePtr<CCD3D12CommandBuffer>> executedBundles[D3D12_MAX_FRAMES_IN_FLIGHT]" in command_buffer
    assert "auto &executedBundles = _impl->executedBundles[_impl->activeRecordingContext]" in begin
    assert "executedBundles.clear()" in begin
    assert "_impl->executedBundles[_impl->activeRecordingContext]" in notify
    assert "bundleCommandBuffer->notifySubmitted(fence, fenceValue)" in notify
    assert "executedBundles[_impl->activeRecordingContext].emplace_back(d3d12CmdBuff)" in execute
    assert execute.index("ExecuteBundle(bundle)") < execute.index("executedBundles[_impl->activeRecordingContext].emplace_back(d3d12CmdBuff)")


def test_render_queue_skips_only_redundant_pipeline_and_material_bind_calls() -> None:
    queue = read("../pipeline/RenderQueue.cpp")

    assert "lastPipelineState" in queue
    assert "lastMaterialDescriptorSet" in queue
    assert "if (!useDrawBatch || pso != lastPipelineState)" in queue
    assert "lastMaterialDescriptorSet = nullptr" in queue
    assert "if (!useDrawBatch || descriptorSet != lastMaterialDescriptorSet)" in queue


def test_d3d12_owns_draw_batch_input_assembler_compatibility() -> None:
    queue = read("../pipeline/RenderQueue.cpp")
    command_buffer = read("D3D12CommandBuffer.cpp")

    assert "hasSameBatchableInputAssemblyState" not in queue
    assert "lastBatchInputAssembler" not in queue
    assert "cmdBuff->drawWithInputAssemblerAndDescriptorSet(" in queue
    assert "bindInputAssembler(inputAssembler);" in command_buffer
    assert "std::memcmp(_impl->boundVertexBufferViews, vbViews" in command_buffer
    assert "std::memcmp(&_impl->boundIndexBufferView, &ibView" in command_buffer
    assert "if (_impl->localRootCbvBatchActive && (vertexBuffersChanged || indexBufferChanged))" in command_buffer


def test_local_root_cbv_batch_fuses_local_set_selection_with_draw_encoding() -> None:
    command_base = read("../gfx-base/GFXCommandBuffer.h")
    command_validator = read("../gfx-validator/CommandBufferValidator.cpp")
    command_d3d12 = read("D3D12CommandBuffer.cpp")
    queue = read("../pipeline/RenderQueue.cpp")

    assert "drawWithInputAssemblerAndDescriptorSet(InputAssembler *inputAssembler, uint32_t set," in command_base
    fused_signature = "drawWithInputAssemblerAndDescriptorSet(InputAssembler *inputAssembler,"
    assert fused_signature in command_validator
    assert fused_signature in command_d3d12
    assert "cmdBuff->drawWithInputAssemblerAndDescriptorSet(" in queue
    assert "drawLocalRootCbvBatchInternal(info, d3d12Set)" in command_d3d12
    assert "hasMatchingStaticCbvSrvUavResources" in command_d3d12


def test_local_root_cbv_batch_is_scoped_and_preserves_immediate_fallback() -> None:
    command_buffer = read("D3D12CommandBuffer.cpp")
    command_header = read("D3D12CommandBuffer.h")
    device = read("D3D12Device.cpp")
    queue = read("../pipeline/RenderQueue.cpp")

    assert "cmdBuff->supportsDrawBatch()" in queue
    assert "cmdBuff->beginDrawBatch()" in queue
    assert "cmdBuff->endDrawBatch()" in queue
    assert "cmdBuff->drawWithInputAssemblerAndDescriptorSet(" in queue
    assert "const bool useDrawBatch = !enableOcclusionQuery && cmdBuff->supportsDrawBatch()" in queue
    assert "captureLocalRootCbvBatchBinding" in command_header
    assert "flushLocalRootCbvBatch" in command_buffer
    assert "hasMatchingStaticCbvSrvUavResources" in command_buffer
    assert "getSamplerTableKey() != _impl->localRootCbvBatchSamplerKey" in command_buffer
    assert "ExecuteIndirect(signature, commandCount" in command_buffer
    assert "DrawIndexedInstanced" in command_buffer
    assert "DrawInstanced" in command_buffer
    assert "getOrCreateLocalRootCbvIndirectSignature" in device
    assert "D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW" in device
    assert "if (_impl->localRootCbvBatchActive && (vertexBuffersChanged || indexBufferChanged))" in command_buffer
    assert "if (!vertexBuffersChanged && !indexBufferChanged)" in command_buffer


def test_draw_batch_contract_is_backend_neutral_and_forwarded() -> None:
    command_base = read("../gfx-base/GFXCommandBuffer.h")
    command_agent = read("../gfx-agent/CommandBufferAgent.cpp")
    command_validator = read("../gfx-validator/CommandBufferValidator.cpp")
    vulkan_header = read("../gfx-vulkan/VKCommandBuffer.h")
    gles3_header = read("../gfx-gles3/GLES3CommandBuffer.h")

    assert "virtual bool supportsDrawBatch() const { return false; }" in command_base
    assert "virtual void beginDrawBatch() {}" in command_base
    assert "virtual void endDrawBatch() {}" in command_base
    assert "bindInputAssembler(inputAssembler);" in command_base
    assert "bindDescriptorSet(set, descriptorSet, 0, nullptr);" in command_base
    assert "draw(info);" in command_base
    assert "RootCbv" not in command_base
    assert "supportsDrawBatch()" in command_agent
    assert "actor->beginDrawBatch()" in command_agent
    assert "actor->drawWithInputAssemblerAndDescriptorSet(inputAssembler, set, descriptorSet, info)" in command_agent
    assert "return _actor->supportsDrawBatch();" in command_validator
    assert "supportsDrawBatch" not in vulkan_header
    assert "supportsDrawBatch" not in gles3_header


def test_d3d12_runtime_selection_is_explicit_and_precedes_vulkan() -> None:
    source = (RENDERER / "GFXDeviceManager.h").read_text(encoding="utf-8")
    explicit_selection = source.index('isRequestedGFXAPI("D3D12")')
    default_vulkan = source.index("if (!skipVulkan && tryCreate<CCVKDevice>")

    assert "CC_GFX_API" in source
    assert "return requested && api && std::strcmp(requested, api) == 0;" in source
    assert explicit_selection < default_vulkan
    assert 'CC_LOG_ERROR("Requested GFX API D3D12 initialization failed.")' in source


def test_win32_platform_is_the_only_windows_frame_pacer() -> None:
    engine = (ROOT / "cocos" / "engine" / "Engine.cpp").read_text(encoding="utf-8")
    platform = (ROOT / "cocos" / "platform" / "win32" / "WindowsPlatform.cpp").read_text(encoding="utf-8")

    assert "CC_PLATFORM == CC_PLATFORM_WINDOWS && !CC_EDITOR" not in engine
    assert "frameSleepGuard" not in engine
    assert "desiredInterval = (LONGLONG)(1.0 / getFps() * nFreq.QuadPart);" in platform
    assert "if (actualInterval >= desiredInterval)" in platform


def test_frame_slot_reuse_keeps_the_cbv_srv_uav_heap_object_stable() -> None:
    device = read("D3D12Device.cpp")
    pool_header = read("D3D12DescriptorHeapPool.h")
    pool_source = read("D3D12DescriptorHeapPool.cpp")
    acquire = function_body(device, "void CCD3D12Device::acquire")

    assert "beginFrameAllocationRange" in pool_header
    assert "allocationRangeStart" in pool_source
    assert "allocationRangeEnd" in pool_source
    assert "frameResources" in device
    assert "_impl->gpuDescriptorHeapPool->beginFrameAllocationRange" in acquire
    assert "waitForSubmittedFence" not in acquire


if __name__ == "__main__":
    tests = [
        test_submit_and_present_do_not_wait_for_gpu,
        test_frame_resources_are_waited_before_reuse_not_after_submit,
        test_frame_slot_wait_precedes_descriptor_range_reuse,
        test_three_frame_pipeline_keeps_swapchain_allocators_and_transient_resources_in_sync,
        test_frame_slot_reuse_owns_sampler_and_upload_resets,
        test_buffers_do_not_always_use_upload_heap,
        test_uniform_updates_copy_to_stable_backing_not_transient_ring_resource,
        test_hot_uniform_updates_use_fence_safe_transient_upload_descriptors,
        test_direct_buffer_updates_are_deferred_to_main_command_list,
        test_deferred_buffer_queue_does_not_own_validator_actor,
        test_texture_uploads_use_upload_ring_not_region_committed_resources,
        test_command_list_resource_retention_uses_constant_time_deduplication,
        test_local_root_table_split_has_shared_layout_and_descriptor_partition_contract,
        test_local_root_table_split_caches_only_exact_static_resources,
        test_local_static_table_cache_skips_non_reusable_suffixes,
        test_local_b0_root_cbv_moves_only_the_changing_uniform_out_of_the_static_table,
        test_local_root_cbv_writes_all_local_cbvs_only_to_the_shader_visible_heap,
        test_local_root_cbv_split_accepts_b0_at_an_arbitrary_descriptor_offset,
        test_local_root_split_separates_dynamic_buffer_slots_from_cacheable_static_table,
        test_local_null_dynamic_table_is_cached_once_per_descriptor_heap_epoch,
        test_local_root_cbv_fast_validation_skips_staging_heap_work,
        test_pending_default_buffer_updates_batch_resource_barriers,
        test_batched_buffer_updates_keep_alias_copies_sequential,
        test_buffer_update_batch_reuses_transition_indices_and_barrier_scratch,
        test_unique_non_view_buffer_batch_skips_transition_hash_map,
        test_unique_buffer_batch_retains_owning_resources_without_generic_set,
        test_query_fetch_reuses_d3d12_objects,
        test_dynamic_pso_repeated_state_is_cached,
        test_background_cache_probe_skips_legacy_v3_migration,
        test_debug_d3d12_shader_compilation_keeps_debug_diagnostics_and_optimized_codegen,
        test_legacy_v3_migration_defers_only_file_persistence,
        test_device_lifecycle_gates_shader_cache_persistence,
        test_shader_cache_store_is_idempotent_under_session_mutex,
        test_color_attachments_supply_optimized_clear_value,
        test_descriptor_set_recovers_staging_allocations_before_writes,
        test_descriptor_sets_suballocate_cpu_staging_descriptors,
        test_local_static_descriptor_metadata_is_built_on_update_not_per_draw,
        test_command_buffer_caches_redundant_graphics_state,
        test_shader_blit_invalidates_cached_graphics_state,
        test_descriptor_flush_is_versioned_and_incremental,
        test_dynamic_offset_restore_rewrites_only_dynamic_descriptors,
        test_dynamic_offset_updates_use_cached_descriptor_slots,
        test_empty_descriptor_sets_do_not_consume_root_parameters,
        test_d3d12_debug_layer_is_explicitly_opt_in,
        test_buffer_state_epoch_tracks_execute_submission_boundary,
        test_sampler_table_cache_reuses_only_exact_owned_keys,
        test_descriptor_flush_uses_bounded_set_index_cache,
        test_descriptor_heap_pool_caches_immutable_start_handles,
        test_dynamic_descriptor_table_cache_keys_exact_offsets,
        test_validator_skips_invariant_layout_and_empty_offset_work,
        test_executed_bundle_lifetime_follows_primary_submission_fence,
        test_render_queue_skips_only_redundant_pipeline_and_material_bind_calls,
        test_d3d12_owns_draw_batch_input_assembler_compatibility,
        test_local_root_cbv_batch_fuses_local_set_selection_with_draw_encoding,
        test_local_root_cbv_batch_is_scoped_and_preserves_immediate_fallback,
        test_draw_batch_contract_is_backend_neutral_and_forwarded,
        test_d3d12_runtime_selection_is_explicit_and_precedes_vulkan,
        test_win32_platform_is_the_only_windows_frame_pacer,
        test_frame_slot_reuse_keeps_the_cbv_srv_uav_heap_object_stable,
    ]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
