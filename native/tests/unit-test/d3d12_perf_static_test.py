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
    ]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
