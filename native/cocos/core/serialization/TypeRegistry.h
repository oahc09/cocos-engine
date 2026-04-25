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

#include <functional>
#include <cstdint>
#include "base/std/container/unordered_map.h"
#include "base/std/container/string.h"
#include "base/std/container/vector.h"

namespace cc {

class CCObject;
class Asset;

class TypeRegistry {
public:
    static TypeRegistry &getInstance();

    using Constructor = std::function<CCObject *()>;
    using BinaryDeserializer = std::function<void(CCObject *, const uint8_t *, uint32_t)>;
    using AssetPropertiesGetter = std::function<ccstd::vector<Asset *>(CCObject *)>;
    using MigrationFunc = std::function<void(uint8_t *&, uint32_t &, uint16_t fromVersion)>;

    struct TypeInfo {
        uint32_t classId{0};
        ccstd::string className;
        Constructor constructor;
        BinaryDeserializer deserializer;
        AssetPropertiesGetter assetGetter;

        // 组件专用
        bool isComponent{false};
        bool isBuiltin{false};
        bool hasUpdate{false};
        bool hasLateUpdate{false};
        int32_t executionOrder{0};
        uint32_t requireComponent{0};
        bool disallowMultiple{false};

        // 版本迁移 (G-4)
        uint16_t binaryVersion{0};
        MigrationFunc migrationFunc;
    };

    // 注册内置类型
    void registerType(const TypeInfo &info);

    // 注册用户脚本类型 (运行时)
    uint32_t registerScriptType(const ccstd::string &className,
                                bool hasUpdate, bool hasLateUpdate,
                                int32_t executionOrder);

    // 查询
    const TypeInfo *getTypeInfo(uint32_t classId) const;
    uint32_t getClassIdByName(const ccstd::string &className) const;
    bool hasType(uint32_t classId) const;

    // 创建实例
    CCObject *create(uint32_t classId) const;

    // 二进制反序列化
    void deserialize(uint32_t classId, CCObject *obj,
                     const uint8_t *data, uint32_t size) const;

    // 版本迁移 (G-4)
    void registerMigration(uint32_t classId, uint16_t fromVersion,
                           uint16_t toVersion, MigrationFunc func);

private:
    TypeRegistry() = default;
    ccstd::unordered_map<uint32_t, TypeInfo> _types;
    ccstd::unordered_map<ccstd::string, uint32_t> _nameToId;
    uint32_t _nextScriptClassId{10000};
};

// 内置组件注册宏
#define CC_REGISTER_BUILTIN(ClassName, StringName, BuiltinId)            \
    static bool _reg_##ClassName = []() {                                \
        cc::TypeRegistry::TypeInfo info;                                  \
        info.classId = BuiltinId;                                         \
        info.className = StringName;                                      \
        info.isComponent = true;                                          \
        info.isBuiltin = true;                                            \
        info.constructor = []() -> cc::CCObject * {                      \
            return ccnew ClassName();                                     \
        };                                                                \
        info.deserializer = [](cc::CCObject *obj, const uint8_t *d, uint32_t s) { \
            static_cast<ClassName *>(obj)->deserializeBinary(d, s);       \
        };                                                                \
        info.assetGetter = [](cc::CCObject *obj) -> ccstd::vector<cc::Asset *> { \
            return static_cast<ClassName *>(obj)->getAssetProperties();   \
        };                                                                \
        cc::TypeRegistry::getInstance().registerType(info);               \
        return true;                                                      \
    }()

} // namespace cc
