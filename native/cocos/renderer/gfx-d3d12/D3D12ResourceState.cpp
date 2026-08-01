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

#include "D3D12ResourceState.h"

#include <algorithm>

namespace cc {
namespace gfx {

D3D12ResourceState::D3D12ResourceState(uint32_t subresourceCount, D3D12_RESOURCE_STATES initialState) {
    reset(subresourceCount, initialState);
}

void D3D12ResourceState::reset(uint32_t subresourceCount, D3D12_RESOURCE_STATES initialState) {
    _subresourceCount = std::max(subresourceCount, 1U);
    _uniformState = initialState;
    _overrides.clear();
}

D3D12_RESOURCE_STATES D3D12ResourceState::get(uint32_t subresource) const {
    if (subresource >= _subresourceCount) {
        return _uniformState;
    }
    const auto iter = _overrides.find(subresource);
    return iter == _overrides.end() ? _uniformState : iter->second;
}

bool D3D12ResourceState::tryGetUniform(D3D12_RESOURCE_STATES &state) const {
    if (!_overrides.empty()) {
        return false;
    }
    state = _uniformState;
    return true;
}

void D3D12ResourceState::set(uint32_t subresource, D3D12_RESOURCE_STATES state) {
    if (subresource >= _subresourceCount) {
        return;
    }
    if (state == _uniformState) {
        _overrides.erase(subresource);
        return;
    }

    _overrides[subresource] = state;
    if (_overrides.size() != _subresourceCount) {
        return;
    }

    const auto collapsedState = _overrides.begin()->second;
    for (const auto &entry : _overrides) {
        if (entry.second != collapsedState) {
            return;
        }
    }
    _uniformState = collapsedState;
    _overrides.clear();
}

void D3D12ResourceState::setAll(D3D12_RESOURCE_STATES state) {
    _uniformState = state;
    _overrides.clear();
}

uint32_t D3D12ResourceBacking::subresourceCount() const {
    return std::max(mipLevels, 1U) * std::max(arraySize, 1U) * std::max(planeCount, 1U);
}

uint32_t D3D12ResourceBacking::subresourceIndex(uint32_t mip, uint32_t arraySlice, uint32_t plane) const {
    return mip + arraySlice * std::max(mipLevels, 1U) +
           plane * std::max(mipLevels, 1U) * std::max(arraySize, 1U);
}

bool D3D12ResourceStateJournal::transition(
    const D3D12ResourceBackingPtr &backing,
    ID3D12Resource *resource,
    uint32_t subresource,
    D3D12_RESOURCE_STATES nextState,
    D3D12_RESOURCE_BARRIER &barrier) {
    if (!backing || !backing->valid || !resource || backing->resource.Get() != resource ||
        subresource >= backing->states.subresourceCount()) {
        return false;
    }

    auto [iter, inserted] = _entries.try_emplace(backing.get());
    auto &entry = iter->second;
    if (inserted) {
        entry.backing = backing;
        entry.resource = resource;
        entry.initialStates = backing->states;
        entry.finalStates = backing->states;
        entry.deviceEpoch = backing->deviceEpoch;
    } else if (entry.resource.Get() != resource ||
               entry.deviceEpoch != backing->deviceEpoch ||
               entry.finalStates.subresourceCount() != backing->states.subresourceCount()) {
        return false;
    }

    const auto previousState = entry.finalStates.get(subresource);
    if (previousState == nextState) {
        return false;
    }

    barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = previousState;
    barrier.Transition.StateAfter = nextState;
    entry.finalStates.set(subresource, nextState);
    return true;
}

bool D3D12ResourceStateJournal::getCurrentState(
    const D3D12ResourceBackingPtr &backing,
    ID3D12Resource *resource,
    uint32_t subresource,
    D3D12_RESOURCE_STATES &state) const {
    if (!backing || !backing->valid || !resource || backing->resource.Get() != resource ||
        subresource >= backing->states.subresourceCount()) {
        return false;
    }
    const auto iter = _entries.find(backing.get());
    if (iter == _entries.end()) {
        state = backing->states.get(subresource);
        return true;
    }
    const auto &entry = iter->second;
    if (entry.resource.Get() != resource ||
        entry.deviceEpoch != backing->deviceEpoch ||
        subresource >= entry.finalStates.subresourceCount()) {
        return false;
    }
    state = entry.finalStates.get(subresource);
    return true;
}

void D3D12ResourceStateJournal::clear() {
    _entries.clear();
}

void D3D12ResourceStateJournal::captureCommittedStates(
    std::vector<D3D12ResourceStateSnapshot> &snapshots) const {
    for (const auto &[backingKey, entry] : _entries) {
        const auto duplicate = std::find_if(
            snapshots.begin(), snapshots.end(),
            [backingKey](const D3D12ResourceStateSnapshot &snapshot) {
                return snapshot.backing.get() == backingKey;
            });
        if (duplicate == snapshots.end()) {
            snapshots.push_back({entry.backing, entry.backing->states});
        }
    }
}

bool D3D12ResourceStateJournal::appendSubmissionFixupBarriers(
    std::vector<D3D12_RESOURCE_BARRIER> &barriers) const {
    for (const auto &[backingKey, entry] : _entries) {
        (void)backingKey;
        // Owner resize invalidates future views, but an already-recorded list
        // retains this backing and may still submit its old native resource.
        if (!entry.backing || !entry.resource ||
            entry.backing->resource.Get() != entry.resource.Get() ||
            entry.backing->deviceEpoch != entry.deviceEpoch ||
            entry.backing->states.subresourceCount() != entry.initialStates.subresourceCount()) {
            return false;
        }

        D3D12_RESOURCE_STATES committedUniform{};
        D3D12_RESOURCE_STATES initialUniform{};
        if (entry.backing->states.tryGetUniform(committedUniform) &&
            entry.initialStates.tryGetUniform(initialUniform)) {
            if (committedUniform != initialUniform) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = entry.resource.Get();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = committedUniform;
                barrier.Transition.StateAfter = initialUniform;
                barriers.push_back(barrier);
            }
            continue;
        }

        for (uint32_t subresource = 0;
             subresource < entry.initialStates.subresourceCount();
             ++subresource) {
            const auto committedState = entry.backing->states.get(subresource);
            const auto initialState = entry.initialStates.get(subresource);
            if (committedState == initialState) {
                continue;
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = entry.resource.Get();
            barrier.Transition.Subresource = subresource;
            barrier.Transition.StateBefore = committedState;
            barrier.Transition.StateAfter = initialState;
            barriers.push_back(barrier);
        }
    }
    return true;
}

bool D3D12ResourceStateJournal::commit() const {
    for (const auto &[backingKey, entry] : _entries) {
        (void)backingKey;
        if (!entry.backing || !entry.resource ||
            entry.backing->resource.Get() != entry.resource.Get() ||
            entry.backing->deviceEpoch != entry.deviceEpoch ||
            entry.backing->states.subresourceCount() != entry.finalStates.subresourceCount()) {
            return false;
        }
    }
    for (const auto &[backingKey, entry] : _entries) {
        (void)backingKey;
        entry.backing->states = entry.finalStates;
    }
    return true;
}

bool needsD3D12UavOrderingBarrier(
    bool hasUnchangedUavSubresource,
    bool previousUavWrite,
    bool nextUavWrite,
    D3D12_RESOURCE_STATES nextState) {
    return hasUnchangedUavSubresource &&
           (previousUavWrite || nextUavWrite) &&
           nextState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

bool makeD3D12TransitionBarrier(
    D3D12ResourceBacking &backing,
    ID3D12Resource *resource,
    uint32_t subresource,
    D3D12_RESOURCE_STATES nextState,
    D3D12_RESOURCE_BARRIER &barrier) {
    if (!resource || subresource >= backing.states.subresourceCount()) {
        return false;
    }
    const auto previousState = backing.states.get(subresource);
    if (previousState == nextState) {
        return false;
    }

    barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = previousState;
    barrier.Transition.StateAfter = nextState;
    backing.states.set(subresource, nextState);
    return true;
}

} // namespace gfx
} // namespace cc
