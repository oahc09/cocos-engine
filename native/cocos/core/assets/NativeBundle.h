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

class Asset;

/**
 * @en NativeBundle provides C++ implementation of asset bundle configuration parsing and query.
 * @zh NativeBundle 提供 C++ 实现的资源包配置解析与查询。
 */
class NativeBundle : public RefCounted {
public:
    NativeBundle(const ccstd::string &name, const ccstd::string &root);
    ~NativeBundle() override;

    /**
     * @en Parse bundle config.json and build internal lookup tables.
     * @zh 解析 bundle 的 config.json 并构建内部查找表。
     * @param configJson The raw JSON string of config.json
     * @return true if parsing succeeded
     */
    bool init(const ccstd::string &configJson);

    /**
     * @en Get an already-loaded asset by its bundle path.
     * @zh 通过 bundle 路径获取已加载的资源。
     * @note Currently a stub — always returns nullptr until AssetManager integration.
     */
    Asset *get(const ccstd::string &path) const;

    /**
     * @en Scene metadata within a bundle.
     * @zh Bundle 中的场景元数据。
     */
    struct SceneInfo {
        ccstd::string url;
        ccstd::string uuid;
    };

    /**
     * @en Query scene info by scene name.
     * @zh 根据场景名查询场景信息。
     */
    SceneInfo getSceneInfo(const ccstd::string &sceneName) const;

    /**
     * @en Convert a bundle-relative path to its asset UUID.
     * @zh 将 bundle 相对路径转换为资源 UUID。
     */
    ccstd::string getUuidByPath(const ccstd::string &path) const;

    /**
     * @en Check whether an asset exists in this bundle's config.
     * @zh 检查资源是否存在于本 bundle 配置中。
     */
    bool hasAsset(const ccstd::string &path) const;

    inline const ccstd::string &getName() const { return _name; }
    inline const ccstd::string &getBasePath() const { return _basePath; }

    /**
     * @en Get all scene names defined in this bundle.
     * @zh 获取本 bundle 中定义的所有场景名。
     */
    ccstd::vector<ccstd::string> getSceneNames() const;

private:
    ccstd::string _name;
    ccstd::string _basePath;

    struct AssetConfig {
        ccstd::string path;
        ccstd::string uuid;
        uint32_t classId{0};
        bool isScene{false};
        ccstd::string nativeExt;
    };

    // uuid → config
    ccstd::unordered_map<ccstd::string, AssetConfig> _configs;
    // path → uuid
    ccstd::unordered_map<ccstd::string, ccstd::string> _pathToUuid;
    // sceneName → info
    ccstd::unordered_map<ccstd::string, SceneInfo> _scenes;

    CC_DISALLOW_COPY_MOVE_ASSIGN(NativeBundle);
};

} // namespace cc
