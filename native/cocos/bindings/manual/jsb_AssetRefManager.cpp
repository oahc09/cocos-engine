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

#include "jsb_AssetRefManager.h"

#include "bindings/auto/jsb_assets_auto.h"
#include "bindings/jswrapper/SeApi.h"
#include "bindings/manual/jsb_conversions.h"
#include "bindings/manual/jsb_global.h"
#include "core/assets/AssetRefManager.h"

// ─── Forward declarations ──────────────────────────────────────────────────
namespace {
se::Object *__jsb_cc_AssetRefManager_proto = nullptr; // NOLINT(readability-identifier-naming)
se::Class  *__jsb_cc_AssetRefManager_class  = nullptr; // NOLINT(readability-identifier-naming)
} // namespace

// ─── getInstance ───────────────────────────────────────────────────────────
// Returns the singleton AssetRefManager instance as a JS object.
// JS: cc.AssetRefManager.getInstance()
static bool js_AssetRefManager_getInstance(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *instance = &cc::AssetRefManager::getInstance();
    native_ptr_to_seval<cc::AssetRefManager>(instance, __jsb_cc_AssetRefManager_class, &s.rval());
    s.rval().toObject()->root(); // prevent GC of singleton
    return true;
}
SE_BIND_FUNC(js_AssetRefManager_getInstance)

// ─── addRef ────────────────────────────────────────────────────────────────
// JS: assetRefManager.addRef(asset)
static bool js_AssetRefManager_addRef(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::AssetRefManager>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 1) {
        cc::Asset *asset = nullptr;
        bool ok = sevalue_to_native(args[0], &asset, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing arguments: expected Asset");
        cobj->addRef(asset);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(js_AssetRefManager_addRef)

// ─── decRef ────────────────────────────────────────────────────────────────
// JS: assetRefManager.decRef(asset, autoRelease = true)
static bool js_AssetRefManager_decRef(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::AssetRefManager>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 1) {
        cc::Asset *asset = nullptr;
        bool ok = sevalue_to_native(args[0], &asset, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing arguments: expected Asset");
        cobj->decRef(asset);
        return true;
    }
    if (argc == 2) {
        cc::Asset *asset = nullptr;
        bool autoRelease = true;
        bool ok = sevalue_to_native(args[0], &asset, s.thisObject());
        ok &= sevalue_to_native(args[1], &autoRelease, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing arguments");
        cobj->decRef(asset, autoRelease);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(js_AssetRefManager_decRef)

// ─── getRefCount ───────────────────────────────────────────────────────────
// JS: assetRefManager.getRefCount(asset) -> uint32_t
static bool js_AssetRefManager_getRefCount(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::AssetRefManager>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 1) {
        cc::Asset *asset = nullptr;
        bool ok = sevalue_to_native(args[0], &asset, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing arguments: expected Asset");
        uint32_t count = cobj->getRefCount(asset);
        s.rval().setUint32(count);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(js_AssetRefManager_getRefCount)

// ─── addRefBatch ───────────────────────────────────────────────────────────
// JS: assetRefManager.addRefBatch([asset1, asset2, ...])
static bool js_AssetRefManager_addRefBatch(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::AssetRefManager>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 1) {
        SE_PRECONDITION2(args[0].isObject(), false, "Argument must be an array");
        se::Object *arrObj = args[0].toObject();
        SE_PRECONDITION2(arrObj->isArray(), false, "Argument must be an array");

        uint32_t length = 0;
        arrObj->getArrayLength(&length);
        ccstd::vector<cc::Asset *> assets;
        assets.reserve(length);

        for (uint32_t i = 0; i < length; ++i) {
            se::Value elem;
            if (arrObj->getArrayElement(i, &elem)) {
                cc::Asset *asset = nullptr;
                bool ok = sevalue_to_native(elem, &asset, s.thisObject());
                SE_PRECONDITION2(ok, false, "Error processing array element %d", i);
                assets.push_back(asset);
            }
        }
        cobj->addRefBatch(assets);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(js_AssetRefManager_addRefBatch)

// ─── decRefBatch ───────────────────────────────────────────────────────────
// JS: assetRefManager.decRefBatch([asset1, asset2, ...], autoRelease = true)
static bool js_AssetRefManager_decRefBatch(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::AssetRefManager>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    bool autoRelease = true;
    if (argc == 1 || argc == 2) {
        SE_PRECONDITION2(args[0].isObject(), false, "First argument must be an array");
        se::Object *arrObj = args[0].toObject();
        SE_PRECONDITION2(arrObj->isArray(), false, "First argument must be an array");

        if (argc == 2) {
            bool ok = sevalue_to_native(args[1], &autoRelease, s.thisObject());
            SE_PRECONDITION2(ok, false, "Error processing autoRelease argument");
        }

        uint32_t length = 0;
        arrObj->getArrayLength(&length);
        ccstd::vector<cc::Asset *> assets;
        assets.reserve(length);

        for (uint32_t i = 0; i < length; ++i) {
            se::Value elem;
            if (arrObj->getArrayElement(i, &elem)) {
                cc::Asset *asset = nullptr;
                bool ok = sevalue_to_native(elem, &asset, s.thisObject());
                SE_PRECONDITION2(ok, false, "Error processing array element %d", i);
                assets.push_back(asset);
            }
        }
        cobj->decRefBatch(assets, autoRelease);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d or %d", (int)argc, 1, 2);
    return false;
}
SE_BIND_FUNC(js_AssetRefManager_decRefBatch)

// ─── setRefCountChangedCallback ────────────────────────────────────────────
// JS: assetRefManager.setRefCountChangedCallback(function(asset, oldCount, newCount) { ... })
// C++ callback → JS callback pattern (see jsb_cocos_manual.cpp / jsb_network_manual.cpp)
static bool js_AssetRefManager_setRefCountChangedCallback(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::AssetRefManager>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 1) {
        if (args[0].isObject() && args[0].toObject()->isFunction()) {
            se::Value jsThis(s.thisObject());
            se::Value jsFunc(args[0]);
            // Attach JS function to this object to prevent GC
            jsThis.toObject()->attachObject(jsFunc.toObject());
            auto *thisObj = s.thisObject();

            auto lambda = [=](cc::Asset *asset, uint32_t oldCount, uint32_t newCount) -> void {
                se::ScriptEngine::getInstance()->clearException();
                se::AutoHandleScope hs;

                se::ValueArray jsArgs;
                jsArgs.resize(3);

                // Convert Asset* to JS object
                bool ok = nativevalue_to_se(asset, jsArgs[0], thisObj);
                if (!ok) {
                    jsArgs[0].setNull();
                }
                jsArgs[1].setUint32(oldCount);
                jsArgs[2].setUint32(newCount);

                se::Value rval;
                se::Object *funcObj = jsFunc.toObject();
                bool succeed = funcObj->call(jsArgs, thisObj, &rval);
                if (!succeed) {
                    se::ScriptEngine::getInstance()->clearException();
                }
            };

            cobj->setRefCountChangedCallback(lambda);
        } else if (args[0].isNull() || args[0].isUndefined()) {
            // Allow clearing the callback by passing null/undefined
            cobj->setRefCountChangedCallback(nullptr);
        } else {
            SE_REPORT_ERROR("Argument must be a function or null");
            return false;
        }
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(js_AssetRefManager_setRefCountChangedCallback)

// ─── Registration ──────────────────────────────────────────────────────────
bool register_all_AssetRefManager(se::Object *obj) { // NOLINT(readability-identifier-naming)
    // Get the jsb namespace
    se::Value nsVal;
    if (!obj->getProperty("jsb", &nsVal)) {
        se::HandleObject jsobj(se::Object::createPlainObject());
        nsVal.setObject(jsobj);
        obj->setProperty("jsb", nsVal);
    }
    se::Object *ns = nsVal.toObject();

    // Create AssetRefManager class in jsb namespace
    // No constructor — singleton accessed via getInstance()
    auto *cls = se::Class::create("AssetRefManager", ns, nullptr, nullptr);

    cls->defineStaticFunction("getInstance", _SE(js_AssetRefManager_getInstance));

    cls->defineFunction("addRef", _SE(js_AssetRefManager_addRef));
    cls->defineFunction("decRef", _SE(js_AssetRefManager_decRef));
    cls->defineFunction("getRefCount", _SE(js_AssetRefManager_getRefCount));
    cls->defineFunction("addRefBatch", _SE(js_AssetRefManager_addRefBatch));
    cls->defineFunction("decRefBatch", _SE(js_AssetRefManager_decRefBatch));
    cls->defineFunction("setRefCountChangedCallback", _SE(js_AssetRefManager_setRefCountChangedCallback));

    cls->install();

    JSBClassType::registerClass<cc::AssetRefManager>(cls);

    __jsb_cc_AssetRefManager_proto = cls->getProto();
    __jsb_cc_AssetRefManager_class = cls;

    se::ScriptEngine::getInstance()->clearException();

    return true;
}
