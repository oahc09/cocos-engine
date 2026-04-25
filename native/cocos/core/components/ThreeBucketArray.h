/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

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

#include <algorithm>
#include <cstdint>
#include "base/std/container/vector.h"
#include "base/Macros.h"

namespace cc {

/**
 * @en Three-bucket array for execution order scheduling. (I-2)
 * Items are sorted into negative, zero, and positive execution order buckets.
 * Iteration order: negative → zero → positive.
 * @zh 三桶排序数组，按 executionOrder 调度。(I-2)
 * 元素按负、零、正执行顺序分桶，遍历顺序：负→零→正。
 */
template <typename T>
class ThreeBucketArray {
public:
    /**
     * Add an item with the given execution order.
     * Negative orders go into the negative bucket,
     * zero into the zero bucket, positive into the positive bucket.
     */
    void add(T *item, int32_t executionOrder) {
        if (executionOrder < 0) {
            _negBucket.push_back({item, executionOrder});
        } else if (executionOrder == 0) {
            _zeroBucket.push_back(item);
        } else {
            _posBucket.push_back({item, executionOrder});
        }
    }

    /**
     * Remove an item from whichever bucket it resides in.
     * Returns true if found and removed.
     */
    bool remove(T *item) {
        // Search negative bucket
        for (auto it = _negBucket.begin(); it != _negBucket.end(); ++it) {
            if (it->ptr == item) {
                _negBucket.erase(it);
                return true;
            }
        }
        // Search zero bucket
        for (auto it = _zeroBucket.begin(); it != _zeroBucket.end(); ++it) {
            if (*it == item) {
                _zeroBucket.erase(it);
                return true;
            }
        }
        // Search positive bucket
        for (auto it = _posBucket.begin(); it != _posBucket.end(); ++it) {
            if (it->ptr == item) {
                _posBucket.erase(it);
                return true;
            }
        }
        return false;
    }

    /**
     * Iterate over all items in order: negative → zero → positive.
     * Negative and positive buckets are sorted by executionOrder.
     */
    template <typename Callable>
    void forEach(Callable &&fn) {
        // Sort negative bucket (ascending, e.g. -100 before -50)
        std::sort(_negBucket.begin(), _negBucket.end(),
                  [](const Entry &a, const Entry &b) { return a.order < b.order; });
        for (auto &entry : _negBucket) {
            fn(entry.ptr);
        }
        for (auto *item : _zeroBucket) {
            fn(item);
        }
        // Sort positive bucket (ascending, e.g. 50 before 100)
        std::sort(_posBucket.begin(), _posBucket.end(),
                  [](const Entry &a, const Entry &b) { return a.order < b.order; });
        for (auto &entry : _posBucket) {
            fn(entry.ptr);
        }
    }

    /**
     * Clear all buckets.
     */
    void clear() {
        _negBucket.clear();
        _zeroBucket.clear();
        _posBucket.clear();
    }

    /**
     * Get total count of items across all buckets.
     */
    size_t size() const {
        return _negBucket.size() + _zeroBucket.size() + _posBucket.size();
    }

    /**
     * Check if there are no items.
     */
    bool empty() const {
        return _negBucket.empty() && _zeroBucket.empty() && _posBucket.empty();
    }

private:
    struct Entry {
        T *ptr{nullptr};
        int32_t order{0};
    };

    ccstd::vector<Entry> _negBucket; // executionOrder < 0
    ccstd::vector<T *> _zeroBucket;  // executionOrder == 0
    ccstd::vector<Entry> _posBucket; // executionOrder > 0
};

} // namespace cc
