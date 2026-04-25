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

#include "core/serialization/TypeRegistry.h"
#include "core/data/Object.h"

namespace cc {

TypeRegistry &TypeRegistry::getInstance() {
    static TypeRegistry instance;
    return instance;
}

void TypeRegistry::registerType(const TypeInfo &info) {
    _types[info.classId] = info;
    if (!info.className.empty()) {
        _nameToId[info.className] = info.classId;
    }
}

uint32_t TypeRegistry::registerScriptType(const ccstd::string &className,
                                          bool hasUpdate, bool hasLateUpdate,
                                          int32_t executionOrder) {
    uint32_t classId = _nextScriptClassId++;
    TypeInfo info;
    info.classId = classId;
    info.className = className;
    info.isComponent = true;
    info.isBuiltin = false;
    info.hasUpdate = hasUpdate;
    info.hasLateUpdate = hasLateUpdate;
    info.executionOrder = executionOrder;
    registerType(info);
    return classId;
}

const TypeRegistry::TypeInfo *TypeRegistry::getTypeInfo(uint32_t classId) const {
    auto it = _types.find(classId);
    return it != _types.end() ? &it->second : nullptr;
}

uint32_t TypeRegistry::getClassIdByName(const ccstd::string &className) const {
    auto it = _nameToId.find(className);
    return it != _nameToId.end() ? it->second : 0;
}

bool TypeRegistry::hasType(uint32_t classId) const {
    return _types.find(classId) != _types.end();
}

CCObject *TypeRegistry::create(uint32_t classId) const {
    auto *info = getTypeInfo(classId);
    if (info && info->constructor) {
        return info->constructor();
    }
    return nullptr;
}

void TypeRegistry::deserialize(uint32_t classId, CCObject *obj,
                               const uint8_t *data, uint32_t size) const {
    auto *info = getTypeInfo(classId);
    if (info && info->deserializer) {
        // 版本迁移 (G-4)
        if (info->migrationFunc && info->binaryVersion > 0) {
            uint8_t *mutData = const_cast<uint8_t *>(data);
            uint32_t mutSize = size;
            info->migrationFunc(mutData, mutSize, info->binaryVersion);
        }
        info->deserializer(obj, data, size);
    }
}

void TypeRegistry::registerMigration(uint32_t classId, uint16_t fromVersion,
                                     uint16_t toVersion, MigrationFunc func) {
    auto *info = getTypeInfo(classId);
    if (info) {
        auto *mutableInfo = const_cast<TypeInfo *>(info);
        mutableInfo->migrationFunc = func;
        mutableInfo->binaryVersion = fromVersion;
    }
}

} // namespace cc
