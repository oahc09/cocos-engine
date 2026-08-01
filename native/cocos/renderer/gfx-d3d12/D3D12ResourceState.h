/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
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

#ifndef NOMINMAX
    #define NOMINMAX
#endif

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

namespace cc {
namespace gfx {

class D3D12ResourceState final {
public:
    D3D12ResourceState() = default;
    D3D12ResourceState(uint32_t subresourceCount, D3D12_RESOURCE_STATES initialState);

    void reset(uint32_t subresourceCount, D3D12_RESOURCE_STATES initialState);
    D3D12_RESOURCE_STATES get(uint32_t subresource) const;
    bool tryGetUniform(D3D12_RESOURCE_STATES &state) const;
    void set(uint32_t subresource, D3D12_RESOURCE_STATES state);
    void setAll(D3D12_RESOURCE_STATES state);
    uint32_t subresourceCount() const { return _subresourceCount; }

private:
    uint32_t _subresourceCount{1};
    D3D12_RESOURCE_STATES _uniformState{D3D12_RESOURCE_STATE_COMMON};
    std::unordered_map<uint32_t, D3D12_RESOURCE_STATES> _overrides;
};

struct D3D12ResourceBacking final {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12ResourceState states;
    uint64_t deviceEpoch{0};
    uint64_t generation{1};
    uint32_t mipLevels{1};
    uint32_t arraySize{1};
    uint32_t planeCount{1};
    bool valid{true};

    uint32_t subresourceCount() const;
    uint32_t subresourceIndex(uint32_t mip, uint32_t arraySlice, uint32_t plane) const;
};

using D3D12ResourceBackingPtr = std::shared_ptr<D3D12ResourceBacking>;

struct D3D12ResourceStateSnapshot final {
    D3D12ResourceBackingPtr backing;
    D3D12ResourceState states;
};

class D3D12ResourceStateJournal final {
public:
    bool transition(
        const D3D12ResourceBackingPtr &backing,
        ID3D12Resource *resource,
        uint32_t subresource,
        D3D12_RESOURCE_STATES nextState,
        D3D12_RESOURCE_BARRIER &barrier);
    bool getCurrentState(
        const D3D12ResourceBackingPtr &backing,
        ID3D12Resource *resource,
        uint32_t subresource,
        D3D12_RESOURCE_STATES &state) const;

    void clear();
    void captureCommittedStates(std::vector<D3D12ResourceStateSnapshot> &snapshots) const;
    bool appendSubmissionFixupBarriers(std::vector<D3D12_RESOURCE_BARRIER> &barriers) const;
    bool commit() const;

private:
    struct Entry final {
        D3D12ResourceBackingPtr backing;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12ResourceState initialStates;
        D3D12ResourceState finalStates;
        uint64_t deviceEpoch{0};
    };

    std::unordered_map<D3D12ResourceBacking *, Entry> _entries;
};

bool needsD3D12UavOrderingBarrier(
    bool hasUnchangedUavSubresource,
    bool previousUavWrite,
    bool nextUavWrite,
    D3D12_RESOURCE_STATES nextState);

bool makeD3D12TransitionBarrier(
    D3D12ResourceBacking &backing,
    ID3D12Resource *resource,
    uint32_t subresource,
    D3D12_RESOURCE_STATES nextState,
    D3D12_RESOURCE_BARRIER &barrier);

} // namespace gfx
} // namespace cc
