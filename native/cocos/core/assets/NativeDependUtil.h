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

#pragma once

#include "base/Macros.h"
#include "base/RefCounted.h"
#include "base/std/container/string.h"
#include "base/std/container/vector.h"
#include "base/std/container/unordered_map.h"

namespace cc {

/**
 * @en NativeDependUtil tracks asset dependency relationships in C++.
 * @zh NativeDependUtil 在 C++ 侧追踪资源依赖关系。
 *
 * Maintains both forward (uuid → its dependencies) and reverse
 * (uuid → assets that depend on it) indexes for fast lookup.
 */
class NativeDependUtil : public RefCounted {
public:
    NativeDependUtil() = default;
    ~NativeDependUtil() override = default;

    /**
     * @en Record dependencies for an asset.
     * @zh 记录资源的依赖关系。
     * @param uuid The asset UUID
     * @param depends List of dependency UUIDs
     *
     * Any previous dependency record for this uuid is replaced.
     */
    void parse(const ccstd::string &uuid,
               const ccstd::vector<ccstd::string> &depends);

    /**
     * @en Get direct dependencies of an asset.
     * @zh 获取资源的直接依赖 UUID 列表。
     */
    ccstd::vector<ccstd::string> getDepends(const ccstd::string &uuid) const;

    /**
     * @en Get all direct and indirect dependencies (transitive closure).
     * @zh 获取资源的所有直接和间接依赖（传递闭包）。
     * @note Guards against circular dependencies.
     */
    ccstd::vector<ccstd::string> getAllDepends(const ccstd::string &uuid) const;

    /**
     * @en Remove all dependency records for an asset.
     * @zh 移除资源的所有依赖记录。
     */
    void remove(const ccstd::string &uuid);

    /**
     * @en Check whether 'uuid' is directly depended on by 'byUuid'.
     * @zh 检查资源是否被另一资源直接依赖。
     */
    bool isReferencedBy(const ccstd::string &uuid,
                        const ccstd::string &byUuid) const;

    /**
     * @en Get all assets that directly depend on the given asset.
     * @zh 获取所有直接依赖给定资源的资源 UUID 列表。
     */
    ccstd::vector<ccstd::string> getReferencers(const ccstd::string &uuid) const;

private:
    // uuid → direct dependency list
    ccstd::unordered_map<ccstd::string, ccstd::vector<ccstd::string>> _depends;

    // uuid → list of assets that depend on it (reverse index)
    ccstd::unordered_map<ccstd::string, ccstd::vector<ccstd::string>> _referencedBy;

    CC_DISALLOW_COPY_MOVE_ASSIGN(NativeDependUtil);
};

} // namespace cc
