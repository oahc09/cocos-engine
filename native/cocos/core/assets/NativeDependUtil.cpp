/****************************************************************************
 Copyright (c) 2021-2023 Xiamen Yaji Software Co., Ltd.

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

#include "core/assets/NativeDependUtil.h"
#include "base/Macros.h"

namespace cc {

void NativeDependUtil::parse(const ccstd::string &uuid,
                             const ccstd::vector<ccstd::string> &depends) {
    // Remove old records first to keep indexes consistent.
    remove(uuid);

    _depends.emplace(uuid, depends);

    // Build reverse index.
    for (const auto &dep : depends) {
        _referencedBy[dep].emplace_back(uuid);
    }
}

ccstd::vector<ccstd::string> NativeDependUtil::getDepends(const ccstd::string &uuid) const {
    auto it = _depends.find(uuid);
    if (it != _depends.end()) {
        return it->second;
    }
    return {};
}

ccstd::vector<ccstd::string> NativeDependUtil::getAllDepends(const ccstd::string &uuid) const {
    ccstd::vector<ccstd::string> result;
    ccstd::unordered_map<ccstd::string, bool> visited;
    ccstd::vector<ccstd::string> stack;

    auto it = _depends.find(uuid);
    if (it != _depends.end()) {
        for (const auto &dep : it->second) {
            stack.emplace_back(dep);
        }
    }

    while (!stack.empty()) {
        ccstd::string current = stack.back();
        stack.pop_back();

        if (visited.find(current) != visited.end()) {
            continue;
        }
        visited.emplace(current, true);
        result.emplace_back(current);

        auto depIt = _depends.find(current);
        if (depIt != _depends.end()) {
            for (const auto &next : depIt->second) {
                if (visited.find(next) == visited.end()) {
                    stack.emplace_back(next);
                }
            }
        }
    }

    return result;
}

void NativeDependUtil::remove(const ccstd::string &uuid) {
    auto it = _depends.find(uuid);
    if (it == _depends.end()) {
        return;
    }

    // Erase uuid from reverse index of each former dependency.
    for (const auto &dep : it->second) {
        auto revIt = _referencedBy.find(dep);
        if (revIt != _referencedBy.end()) {
            auto &refs = revIt->second;
            refs.erase(
                std::remove(refs.begin(), refs.end(), uuid),
                refs.end());
            if (refs.empty()) {
                _referencedBy.erase(revIt);
            }
        }
    }

    _depends.erase(it);

    // Also erase uuid's own reverse-index entry if it exists.
    _referencedBy.erase(uuid);
}

bool NativeDependUtil::isReferencedBy(const ccstd::string &uuid,
                                      const ccstd::string &byUuid) const {
    auto it = _referencedBy.find(uuid);
    if (it != _referencedBy.end()) {
        for (const auto &ref : it->second) {
            if (ref == byUuid) {
                return true;
            }
        }
    }
    return false;
}

ccstd::vector<ccstd::string> NativeDependUtil::getReferencers(const ccstd::string &uuid) const {
    auto it = _referencedBy.find(uuid);
    if (it != _referencedBy.end()) {
        return it->second;
    }
    return {};
}

} // namespace cc
