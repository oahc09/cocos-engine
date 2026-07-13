from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
D3D12 = ROOT / "cocos" / "renderer" / "gfx-d3d12"


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


def test_wait_for_gpu_does_not_reset_descriptor_heaps() -> None:
    body = function_body(read("D3D12Device.cpp"), "void CCD3D12Device::waitForGpu")
    assert "gpuDescriptorHeapPool" not in body
    assert "samplerDescriptorHeapPool" not in body
    assert "uploadPages" in body


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


def test_texture_uploads_use_upload_ring_not_region_committed_resources() -> None:
    for file_name, signature in [
        ("D3D12CommandBuffer.cpp", "void CCD3D12CommandBuffer::copyBuffersToTexture"),
        ("D3D12Device.cpp", "void CCD3D12Device::copyBuffersToTexture"),
    ]:
        body = function_body(read(file_name), signature)
        assert "CreateCommittedResource" not in body
        assert "allocateUploadBuffer" in body


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


if __name__ == "__main__":
    tests = [
        test_submit_and_present_do_not_wait_for_gpu,
        test_frame_resources_are_waited_before_reuse_not_after_submit,
        test_wait_for_gpu_does_not_reset_descriptor_heaps,
        test_buffers_do_not_always_use_upload_heap,
        test_uniform_updates_copy_to_stable_backing_not_transient_ring_resource,
        test_texture_uploads_use_upload_ring_not_region_committed_resources,
        test_query_fetch_reuses_d3d12_objects,
        test_dynamic_pso_repeated_state_is_cached,
        test_background_cache_probe_skips_legacy_v3_migration,
        test_legacy_v3_migration_defers_only_file_persistence,
        test_device_lifecycle_gates_shader_cache_persistence,
        test_shader_cache_store_is_idempotent_under_session_mutex,
        test_color_attachments_supply_optimized_clear_value,
        test_descriptor_set_recovers_staging_allocations_before_writes,
        test_descriptor_sets_suballocate_cpu_staging_descriptors,
    ]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
