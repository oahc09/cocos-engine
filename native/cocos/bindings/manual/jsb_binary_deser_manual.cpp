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

#include "jsb_binary_deser_manual.h"

#include "bindings/auto/jsb_assets_auto.h"
#include "bindings/auto/jsb_scene_auto.h"
#include "bindings/jswrapper/SeApi.h"
#include "bindings/manual/jsb_conversions.h"
#include "core/serialization/BinaryDeserializer.h"
#include "core/scene-graph/Scene.h"
#include "core/assets/Asset.h"

// ─── Forward declarations ──────────────────────────────────────────────────
namespace {
se::Object *__jsb_cc_BinaryDeserializer_proto = nullptr; // NOLINT(readability-identifier-naming)
se::Class  *__jsb_cc_BinaryDeserializer_class  = nullptr; // NOLINT(readability-identifier-naming)
} // namespace

// ─── deserialize ───────────────────────────────────────────────────────────
// JS: jsb.BinaryDeserializer.deserialize(data: ArrayBuffer | Uint8Array)
//     -> { scene: Scene | null, assets: Asset[], success: boolean, errorMessage: string }
static bool js_BinaryDeserializer_deserialize(se::State &s) { // NOLINT(readability-identifier-naming)
    const auto &args = s.args();
    size_t argc = args.size();

    if (argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }

    SE_PRECONDITION2(args[0].isObject(), false, "Argument must be an ArrayBuffer or TypedArray");
    se::Object *dataObj = args[0].toObject();

    uint8_t *data = nullptr;
    size_t length = 0;
    bool ok = false;

    if (dataObj->isArrayBuffer()) {
        ok = dataObj->getArrayBufferData(&data, &length);
        SE_PRECONDITION2(ok, false, "getArrayBufferData failed!");
    } else if (dataObj->isTypedArray()) {
        ok = dataObj->getTypedArrayData(&data, &length);
        SE_PRECONDITION2(ok, false, "getTypedArrayData failed!");
    } else {
        SE_REPORT_ERROR("Argument must be an ArrayBuffer or TypedArray");
        return false;
    }

    // Call the native deserializer
    cc::BinaryDeserializeResult result = cc::BinaryDeserializer::deserialize(data, static_cast<uint32_t>(length));

    // Build the return object: { scene, assets, success, errorMessage }
    se::HandleObject retObj(se::Object::createPlainObject());
    SE_PRECONDITION2(retObj.isValid(), false, "Failed to create return object");

    // scene: Scene | null
    if (result.scene != nullptr) {
        se::Value sceneVal;
        ok = native_ptr_to_seval<cc::Scene>(result.scene.get(), __jsb_cc_Scene_class, &sceneVal);
        SE_PRECONDITION2(ok, false, "Failed to convert Scene* to JS object");
        retObj->setProperty("scene", sceneVal);
    } else {
        retObj->setProperty("scene", se::Value(se::NullObject()));
    }

    // assets: Asset[]
    se::HandleObject assetsArr(se::Object::createArrayObject(result.assets.size()));
    for (size_t i = 0; i < result.assets.size(); ++i) {
        se::Value assetVal;
        ok = native_ptr_to_seval<cc::Asset>(result.assets[i].get(), __jsb_cc_Asset_class, &assetVal);
        if (!ok) {
            assetVal.setNull();
        }
        assetsArr->setArrayElement(static_cast<uint32_t>(i), assetVal);
    }
    retObj->setProperty("assets", se::Value(assetsArr));

    // success: boolean
    retObj->setProperty("success", se::Value(result.success));

    // errorMessage: string
    retObj->setProperty("errorMessage", se::Value(result.errorMessage));

    s.rval().setObject(retObj, true);
    return true;
}
SE_BIND_FUNC(js_BinaryDeserializer_deserialize)

// ─── Registration ──────────────────────────────────────────────────────────
bool register_all_binary_deser(se::Object *obj) { // NOLINT(readability-identifier-naming)
    // Get the jsb namespace
    se::Value nsVal;
    if (!obj->getProperty("jsb", &nsVal)) {
        se::HandleObject jsobj(se::Object::createPlainObject());
        nsVal.setObject(jsobj);
        obj->setProperty("jsb", nsVal);
    }
    se::Object *ns = nsVal.toObject();

    // Create BinaryDeserializer class in jsb namespace
    // No constructor — pure static utility class
    auto *cls = se::Class::create("BinaryDeserializer", ns, nullptr, nullptr);

    cls->defineStaticFunction("deserialize", _SE(js_BinaryDeserializer_deserialize));

    cls->install();

    JSBClassType::registerClass<cc::BinaryDeserializer>(cls);

    __jsb_cc_BinaryDeserializer_proto = cls->getProto();
    __jsb_cc_BinaryDeserializer_class = cls;

    se::ScriptEngine::getInstance()->clearException();

    return true;
}
