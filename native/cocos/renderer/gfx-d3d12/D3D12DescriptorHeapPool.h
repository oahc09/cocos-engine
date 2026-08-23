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

#include <cstdint>
#include <memory>

namespace cc {
namespace gfx {

/**
 * D3D12DescriptorHeapPool manages allocation and recycling of D3D12 descriptor heaps.
 * Supports CBV_SRV_UAV and SAMPLER heap types, with both CPU-visible and GPU-visible variants.
 *
 * Threading contract: NOT thread-safe. All methods (including reset() and the
 * dirty-heap bookkeeping) must be called from the frame/command-recording
 * thread only, under the same single-threaded recording convention as the
 * rest of this backend. GPU-visible pools are additionally frame-local and
 * fence-protected by their owning frame resource.
 *
 * This is a utility class designed to be integrated into the Device by Agent C.
 * Usage:
 *   1. Call allocate() to get a contiguous block of descriptors from a heap.
 *   2. Call reset() once per frame to recycle all allocations.
 *   3. Or call deallocate() to return individual allocations.
 */
class D3D12DescriptorHeapPool final {
public:
    enum class HeapType : uint32_t {
        CBV_SRV_UAV,
        SAMPLER,
    };

    struct Allocation {
        void *cpuHandle{nullptr};    // D3D12_CPU_DESCRIPTOR_HANDLE (as ptr)
        uint64_t gpuHandle{0};       // D3D12_GPU_DESCRIPTOR_HANDLE (as uint64_t)
        uint32_t numDescriptors{0};
        uint32_t heapIndex{0};       // which heap in the pool
        bool isValid{false};
    };

    D3D12DescriptorHeapPool();
    ~D3D12DescriptorHeapPool();

    /**
     * Initialize the pool for a given heap type and capacity.
     * @param heapType CBV_SRV_UAV or SAMPLER
     * @param maxDescriptorsPerHeap Max descriptors per individual heap
     * @param shaderVisible Whether the heaps should be GPU-visible (shader-visible)
     */
    void initialize(HeapType heapType, uint32_t maxDescriptorsPerHeap = 1024, bool shaderVisible = false);

    /** Shutdown and release all heaps. */
    void shutdown();

    /**
     * Allocate a contiguous block of descriptors.
     * @param count Number of descriptors needed
     * @return Allocation with CPU/GPU handles, or invalid allocation if out of space
     */
    Allocation allocate(uint32_t count);

    /**
     * Deallocate a previous allocation. May not immediately reclaim space
     * depending on the pool strategy.
     */
    void deallocate(const Allocation &alloc);

    /**
     * Reset all heaps for a new frame. All previous allocations become invalid.
     */
    void reset();

    /**
     * Restrict subsequent allocations to one reusable range in the first heap.
     * The caller must fence-protect that range before selecting it again.
     */
    void beginFrameAllocationRange(uint32_t offset, uint32_t count);

    /** Get the D3D12 descriptor increment size for this heap type. */
    uint32_t getDescriptorSize() const;

    /** Get the raw ID3D12DescriptorHeap pointer for the given heap index (for SetDescriptorHeaps). */
    void *getHeap(uint32_t heapIndex) const;

    /** Get number of active heaps. */
    uint32_t getHeapCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace gfx
} // namespace cc
