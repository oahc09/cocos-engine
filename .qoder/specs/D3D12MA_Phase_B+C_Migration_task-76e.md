# D3D12MA Phase B+C: Buffer and Texture Migration

## Goal

Replace `ID3D12Device::CreateCommittedResource` with `D3D12MA::Allocator::CreateResource` in the buffer and texture creation paths. This enables D3D12MA's placed-resource sub-allocation, reducing per-resource heap overhead (64KB minimum per committed resource). Fallback to committed resources when the allocator is unavailable (e.g., WARP failure, creation error).

## Scope

### In scope (Phase B+C)
- `CCD3D12Buffer::createResource()` (line 679) — main buffer creation
- `CCD3D12Buffer::ensureUploadResource()` (lines 762, 829) — per-frame upload resource migration
- `CCD3D12Texture::createResource()` (line 638) — main texture creation

### Out of scope (Phase D or later)
- Device-internal allocations: transient uniform arenas (line 762), upload pages (line 860), shared upload pool (line 929), texture readback (line 1814) — these are already pooled internally and don't benefit from D3D12MA sub-allocation.

## Design

### Resource + Allocation ownership model

D3D12MA::CreateResource with `ppvResource != null` returns:
- `Allocation**` — allocation object (owns internal resource refcount)
- `void**` — resource with separate refcount via QueryInterface

Both must be released independently. The resource can outlive the allocation (e.g., when old backing is still referenced by command buffers).

**Buffer**: Add `Microsoft::WRL::ComPtr<D3D12MA::Allocation> d3d12maAllocation` to `CCD3D12Buffer::Impl`. The existing `ComPtr<ID3D12Resource> resource` continues to work as before.

**Texture**: Add `ComPtr<D3D12MA::Allocation> d3d12maAllocation` to `CCD3D12Texture::Impl`. The `D3D12ResourceBacking` struct is unchanged (it only holds the resource).

### Fallback pattern

```cpp
auto *allocator = device->getMemoryAllocator();
if (allocator) {
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = heapType;
    allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
    hr = allocator->CreateResource(&allocDesc, &resourceDesc, initialState,
                                   optimizedClearValue,
                                   _impl->d3d12maAllocation.ReleaseAndGetAddressOf(),
                                   IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        _impl->d3d12maAllocation.Reset();
        // Fall through to committed path
    }
}
if (!_impl->d3d12maAllocation) {
    // Existing CreateCommittedResource path
}
```

### Destruction order

1. Release `ComPtr<ID3D12Resource>` (decrements resource refcount)
2. Release `ComPtr<D3D12MA::Allocation>` (decrements allocation refcount, which also releases its internal resource refcount)

For buffers: `Impl` already resets via `shared_ptr` destruction. Add explicit `d3d12maAllocation.Reset()` before `resource.Reset()` in `doDestroy()` for clarity.

For textures: `Impl` resets via `unique_ptr` destruction. Add explicit `d3d12maAllocation.Reset()` in `doDestroy()`.

### Resize handling

When `createResource` is called again (resize), release the old allocation before creating a new one. The old backing's resource refcount keeps the resource alive for GPU use.

## Implementation Steps

### Step 1: Buffer migration — createResource()

File: [D3D12Buffer.cpp](file:///d:/cocos_custome/cocos-engine/native/cocos/renderer/gfx-d3d12/D3D12Buffer.cpp)

1. Add `#include "D3D12MemAlloc.h"` to D3D12Buffer.cpp
2. Add `Microsoft::WRL::ComPtr<D3D12MA::Allocation> d3d12maAllocation;` to `Impl` struct
3. In `createResource()`, after building `heapProperties` and `resourceDesc`:
   - Try D3D12MA::CreateResource if allocator is available
   - Fall back to CreateCommittedResource on failure or null allocator
   - Store both allocation and resource
4. In `doDestroy()`, reset allocation before resource

### Step 2: Buffer migration — ensureUploadResource()

File: [D3D12Buffer.cpp](file:///d:/cocos_custome/cocos-engine/native/cocos/renderer/gfx-d3d12/D3D12Buffer.cpp)

1. Add per-frame allocations: `std::array<ComPtr<D3D12MA::Allocation>, D3D12_MAX_FRAMES_IN_FLIGHT> uploadAllocations;` to `Impl`
2. In `ensureUploadResource()`, use D3D12MA for both the migration path (line 762) and the normal path (line 829)
3. On migration from shared upload pool, release the old allocation

### Step 3: Texture migration — createResource()

File: [D3D12Texture.cpp](file:///d:/cocos_custome/cocos-engine/native/cocos/renderer/gfx-d3d12/D3D12Texture.cpp)

1. Add `#include "D3D12MemAlloc.h"` to D3D12Texture.cpp
2. Add `Microsoft::WRL::ComPtr<D3D12MA::Allocation> d3d12maAllocation;` to `Impl` struct
3. In `createResource()`, try D3D12MA::CreateResource with `HeapType = DEFAULT` before falling back to CreateCommittedResource
4. In `doDestroy()`, reset allocation
5. In `doResize()` / `createResource()`, release old allocation before creating new one

### Step 4: Static tests

File: [D3D12HotPathStaticTest.py](file:///d:/cocos_custome/cocos-engine/native/cocos/renderer/gfx-d3d12/D3D12HotPathStaticTest.py)

Add tests:
- `test_buffer_creation_uses_d3d12ma_with_committed_fallback`: verify `CreateResource` is called with allocator, and `CreateCommittedResource` fallback exists
- `test_texture_creation_uses_d3d12ma_with_committed_fallback`: same for textures
- `test_d3d12ma_allocation_released_before_resource`: verify destruction order in doDestroy

## Verification

- Run `python native/cocos/renderer/gfx-d3d12/D3D12HotPathStaticTest.py` — all pass
- User compiles in local VS — no new errors
- User runs a scene with D3D12 backend — verify:
  - Log shows "D3D12MemoryAllocator v3.2.0 initialized."
  - No D3D12MA-related errors
  - Visual output identical to Phase A (no rendering changes — same resources, just allocated differently)
  - Memory usage: D3D12MA's `GetBudget()` can be queried to verify placed-resource usage vs committed

## Assumptions

- D3D12MA's `CreateResource` with `ppvResource != null` returns a separately refcounted resource (confirmed from docs line 1247-1249)
- `Allocation::Release()` is immediate (no deferred destruction) — engine's fence retirement ensures GPU is done before CPU release
- WARP adapter path: allocator is null (creation failed), so all resources use committed path — no D3D12MA involvement
- BufferView shares `_impl` with parent — allocation is owned by parent, views just use the resource (no change needed)
