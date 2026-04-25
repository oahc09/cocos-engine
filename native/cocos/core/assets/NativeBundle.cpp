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

#include "core/assets/NativeBundle.h"
#include "core/assets/Asset.h"
#include "base/Macros.h"
#include "base/Log.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"

namespace cc {

NativeBundle::NativeBundle(const ccstd::string &name, const ccstd::string &root)
: _name(name),
  _basePath(root) {
}

NativeBundle::~NativeBundle() = default;

bool NativeBundle::init(const ccstd::string &configJson) {
    rapidjson::Document doc;
    doc.Parse(configJson.c_str());
    if (doc.HasParseError()) {
        CC_LOG_ERROR("NativeBundle::init — failed to parse config.json for bundle '%s'", _name.c_str());
        return false;
    }

    // Parse "paths": { "uuid": { "path": "...", "ctype": 4 } }
    if (doc.HasMember("paths") && doc["paths"].IsObject()) {
        const rapidjson::Value &paths = doc["paths"];
        for (auto it = paths.MemberBegin(); it != paths.MemberEnd(); ++it) {
            const char *uuid = it->name.GetString();
            const rapidjson::Value &entry = it->value;

            AssetConfig config;
            config.uuid = uuid;

            if (entry.IsObject()) {
                if (entry.HasMember("path") && entry["path"].IsString()) {
                    config.path = entry["path"].GetString();
                }
                if (entry.HasMember("ctype") && entry["ctype"].IsInt()) {
                    config.classId = static_cast<uint32_t>(entry["ctype"].GetInt());
                }
                if (entry.HasMember("isScene") && entry["isScene"].IsBool()) {
                    config.isScene = entry["isScene"].GetBool();
                }
                if (entry.HasMember("nativeExt") && entry["nativeExt"].IsString()) {
                    config.nativeExt = entry["nativeExt"].GetString();
                }
            }

            _configs.emplace(uuid, config);
            if (!config.path.empty()) {
                _pathToUuid.emplace(config.path, uuid);
            }
        }
    }

    // Parse "scenes": { "sceneName": { "url": "...", "uuid": "..." } }
    if (doc.HasMember("scenes") && doc["scenes"].IsObject()) {
        const rapidjson::Value &scenes = doc["scenes"];
        for (auto it = scenes.MemberBegin(); it != scenes.MemberEnd(); ++it) {
            const char *sceneName = it->name.GetString();
            const rapidjson::Value &entry = it->value;

            SceneInfo info;
            if (entry.IsObject()) {
                if (entry.HasMember("url") && entry["url"].IsString()) {
                    info.url = entry["url"].GetString();
                }
                if (entry.HasMember("uuid") && entry["uuid"].IsString()) {
                    info.uuid = entry["uuid"].GetString();
                }
            }

            _scenes.emplace(sceneName, info);
        }
    }

    return true;
}

Asset *NativeBundle::get(const ccstd::string & /*path*/) const {
    // Stub: AssetManager integration will resolve path → loaded Asset*.
    return nullptr;
}

NativeBundle::SceneInfo NativeBundle::getSceneInfo(const ccstd::string &sceneName) const {
    auto it = _scenes.find(sceneName);
    if (it != _scenes.end()) {
        return it->second;
    }
    return SceneInfo{};
}

ccstd::string NativeBundle::getUuidByPath(const ccstd::string &path) const {
    auto it = _pathToUuid.find(path);
    if (it != _pathToUuid.end()) {
        return it->second;
    }
    return "";
}

bool NativeBundle::hasAsset(const ccstd::string &path) const {
    return _pathToUuid.find(path) != _pathToUuid.end();
}

ccstd::vector<ccstd::string> NativeBundle::getSceneNames() const {
    ccstd::vector<ccstd::string> names;
    names.reserve(_scenes.size());
    for (const auto &pair : _scenes) {
        names.emplace_back(pair.first);
    }
    return names;
}

} // namespace cc
