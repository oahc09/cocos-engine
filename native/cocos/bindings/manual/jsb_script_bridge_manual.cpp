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

#include "jsb_script_bridge_manual.h"

#include "bindings/jswrapper/SeApi.h"
#include "bindings/manual/jsb_conversions.h"
#include "core/scripting/ScriptBridge.h"
#include "core/components/ScriptComponent.h"

// ─── Forward declarations ──────────────────────────────────────────────────
namespace {
se::Object *__jsb_cc_ScriptBridge_proto = nullptr; // NOLINT(readability-identifier-naming)
se::Class  *__jsb_cc_ScriptBridge_class  = nullptr; // NOLINT(readability-identifier-naming)
} // namespace

// ─── getInstance ───────────────────────────────────────────────────────────
// Returns the singleton ScriptBridge instance as a JS object.
// JS: jsb.ScriptBridge.getInstance()
static bool js_ScriptBridge_getInstance(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *instance = &cc::ScriptBridge::getInstance();
    native_ptr_to_seval<cc::ScriptBridge>(instance, __jsb_cc_ScriptBridge_class, &s.rval());
    s.rval().toObject()->root(); // prevent GC of singleton
    return true;
}
SE_BIND_FUNC(js_ScriptBridge_getInstance)

// ─── init ──────────────────────────────────────────────────────────────────
// JS: scriptBridge.init()
static bool js_ScriptBridge_init(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 0) {
        cobj->init(s.thisObject());
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_init)

// ─── shutdown ──────────────────────────────────────────────────────────────
// JS: scriptBridge.shutdown()
static bool js_ScriptBridge_shutdown(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cobj->shutdown();
    return true;
}
SE_BIND_FUNC(js_ScriptBridge_shutdown)

// ─── registerScriptClass ───────────────────────────────────────────────────
// JS: scriptBridge.registerScriptClass(className, hasStart, hasUpdate, hasLateUpdate,
//                                      hasOnLoad, hasOnDestroy, hasOnEnable, hasOnDisable,
//                                      executionOrder, requireComponent, disallowMultiple) -> number
static bool js_ScriptBridge_registerScriptClass(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 11) {
        ccstd::string className;
        bool hasStart = false;
        bool hasUpdate = false;
        bool hasLateUpdate = false;
        bool hasOnLoad = false;
        bool hasOnDestroy = false;
        bool hasOnEnable = false;
        bool hasOnDisable = false;
        int32_t executionOrder = 0;
        uint32_t requireComponent = 0;
        bool disallowMultiple = false;

        bool ok = true;
        ok &= sevalue_to_native(args[0], &className, s.thisObject());
        ok &= sevalue_to_native(args[1], &hasStart, s.thisObject());
        ok &= sevalue_to_native(args[2], &hasUpdate, s.thisObject());
        ok &= sevalue_to_native(args[3], &hasLateUpdate, s.thisObject());
        ok &= sevalue_to_native(args[4], &hasOnLoad, s.thisObject());
        ok &= sevalue_to_native(args[5], &hasOnDestroy, s.thisObject());
        ok &= sevalue_to_native(args[6], &hasOnEnable, s.thisObject());
        ok &= sevalue_to_native(args[7], &hasOnDisable, s.thisObject());
        ok &= sevalue_to_native(args[8], &executionOrder, s.thisObject());
        ok &= sevalue_to_native(args[9], &requireComponent, s.thisObject());
        ok &= sevalue_to_native(args[10], &disallowMultiple, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing arguments");

        uint32_t classId = cobj->registerScriptClass(className, hasStart, hasUpdate, hasLateUpdate,
                                                      hasOnLoad, hasOnDestroy, hasOnEnable, hasOnDisable,
                                                      executionOrder, requireComponent, disallowMultiple);
        s.rval().setUint32(classId);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 11);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_registerScriptClass)

// ─── registerScriptInstance ────────────────────────────────────────────────
// JS:
//   scriptBridge.registerScriptInstance(jsComp, scriptComp, className) -> number
//   scriptBridge.registerScriptInstance(jsComp, compId, className, scriptComp?) -> number
static bool js_ScriptBridge_registerScriptInstance(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 3 || argc == 4) {
        // arg0: jsComp — se::Object* (JS component object)
        SE_PRECONDITION2(args[0].isObject(), false, "First argument (jsComp) must be an object");
        se::Object *jsComp = args[0].toObject();

        if (args[1].isObject()) {
            SE_PRECONDITION2(argc == 3, false, "Legacy registerScriptInstance expects exactly 3 arguments");

            // arg1: scriptComp — ScriptComponent* (C++ component object)
            // Since ScriptComponent may not have JSB auto binding, extract via getPrivateData.
            se::Object *scriptCompObj = args[1].toObject();
            auto *scriptComp = static_cast<cc::ScriptComponent *>(scriptCompObj->getPrivateData());
            SE_PRECONDITION2(scriptComp != nullptr, false, "Failed to extract ScriptComponent from JS object");

            ccstd::string className;
            bool ok = sevalue_to_native(args[2], &className, s.thisObject());
            SE_PRECONDITION2(ok, false, "Error processing className argument");

            uint32_t compId = cobj->registerScriptInstance(jsComp, scriptComp, className);
            s.rval().setUint32(compId);
            return true;
        }

        uint32_t compId = 0;
        bool ok = sevalue_to_native(args[1], &compId, s.thisObject());
        SE_PRECONDITION2(ok, false, "Second argument (compId) must be a uint32");

        ccstd::string className;
        ok = sevalue_to_native(args[2], &className, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing className argument");

        cc::ScriptComponent *scriptComp = nullptr;
        if (argc == 4) {
            SE_PRECONDITION2(args[3].isObject(), false, "Fourth argument (scriptComp) must be an object");
            se::Object *scriptCompObj = args[3].toObject();
            scriptComp = static_cast<cc::ScriptComponent *>(scriptCompObj->getPrivateData());
            SE_PRECONDITION2(scriptComp != nullptr, false, "Failed to extract ScriptComponent from JS object");
        }

        uint32_t registeredCompId = cobj->registerScriptInstance(compId, jsComp, className, scriptComp);
        s.rval().setUint32(registeredCompId);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d or %d", (int)argc, 3, 4);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_registerScriptInstance)

// ─── unregisterScriptInstance ──────────────────────────────────────────────
// JS: scriptBridge.unregisterScriptInstance(compId)
static bool js_ScriptBridge_unregisterScriptInstance(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 1) {
        uint32_t compId = 0;
        bool ok = sevalue_to_native(args[0], &compId, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing compId argument");
        cobj->unregisterScriptInstance(compId);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_unregisterScriptInstance)

// ─── Helper: JS Array → ccstd::vector<uint32_t> ────────────────────────────
static bool jsArrayToUint32Vector(se::Object *arrObj, ccstd::vector<uint32_t> &out) {
    if (!arrObj->isArray()) {
        return false;
    }
    uint32_t length = 0;
    arrObj->getArrayLength(&length);
    out.reserve(length);
    for (uint32_t i = 0; i < length; ++i) {
        se::Value v;
        if (arrObj->getArrayElement(i, &v)) {
            out.push_back(v.toUint32());
        }
    }
    return true;
}

// ─── invokeStartBatch ──────────────────────────────────────────────────────
// JS: scriptBridge.invokeStartBatch(compIds)
static bool js_ScriptBridge_invokeStartBatch(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 1) {
        SE_PRECONDITION2(args[0].isObject(), false, "Argument must be an array");
        se::Object *arrObj = args[0].toObject();
        ccstd::vector<uint32_t> compIds;
        bool ok = jsArrayToUint32Vector(arrObj, compIds);
        SE_PRECONDITION2(ok, false, "Argument must be an array of numbers");
        cobj->invokeStartBatch(compIds);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_invokeStartBatch)

// ─── invokeUpdateBatch ─────────────────────────────────────────────────────
// JS: scriptBridge.invokeUpdateBatch(compIds, dt)
static bool js_ScriptBridge_invokeUpdateBatch(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 2) {
        SE_PRECONDITION2(args[0].isObject(), false, "First argument must be an array");
        se::Object *arrObj = args[0].toObject();
        ccstd::vector<uint32_t> compIds;
        bool ok = jsArrayToUint32Vector(arrObj, compIds);
        SE_PRECONDITION2(ok, false, "First argument must be an array of numbers");

        float dt = 0.0f;
        ok = sevalue_to_native(args[1], &dt, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing dt argument");

        cobj->invokeUpdateBatch(compIds, dt);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 2);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_invokeUpdateBatch)

// ─── invokeLateUpdateBatch ─────────────────────────────────────────────────
// JS: scriptBridge.invokeLateUpdateBatch(compIds, dt)
static bool js_ScriptBridge_invokeLateUpdateBatch(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 2) {
        SE_PRECONDITION2(args[0].isObject(), false, "First argument must be an array");
        se::Object *arrObj = args[0].toObject();
        ccstd::vector<uint32_t> compIds;
        bool ok = jsArrayToUint32Vector(arrObj, compIds);
        SE_PRECONDITION2(ok, false, "First argument must be an array of numbers");

        float dt = 0.0f;
        ok = sevalue_to_native(args[1], &dt, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing dt argument");

        cobj->invokeLateUpdateBatch(compIds, dt);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 2);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_invokeLateUpdateBatch)

// ─── invokeOnDestroy ───────────────────────────────────────────────────────
// JS: scriptBridge.invokeOnDestroy(compId)
static bool js_ScriptBridge_invokeOnDestroy(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 1) {
        uint32_t compId = 0;
        bool ok = sevalue_to_native(args[0], &compId, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing compId argument");
        cobj->invokeOnDestroy(compId);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_invokeOnDestroy)

// ─── invokeOnEnable ────────────────────────────────────────────────────────
// JS: scriptBridge.invokeOnEnable(compId)
static bool js_ScriptBridge_invokeOnEnable(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 1) {
        uint32_t compId = 0;
        bool ok = sevalue_to_native(args[0], &compId, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing compId argument");
        cobj->invokeOnEnable(compId);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_invokeOnEnable)

// ─── invokeOnDisable ───────────────────────────────────────────────────────
// JS: scriptBridge.invokeOnDisable(compId)
static bool js_ScriptBridge_invokeOnDisable(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 1) {
        uint32_t compId = 0;
        bool ok = sevalue_to_native(args[0], &compId, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing compId argument");
        cobj->invokeOnDisable(compId);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_invokeOnDisable)

// ─── invokeOnLoad ──────────────────────────────────────────────────────────
// JS: scriptBridge.invokeOnLoad(compId)
static bool js_ScriptBridge_invokeOnLoad(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 1) {
        uint32_t compId = 0;
        bool ok = sevalue_to_native(args[0], &compId, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing compId argument");
        cobj->invokeOnLoad(compId);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_invokeOnLoad)

// ─── isInstanceOf ──────────────────────────────────────────────────────────
// JS: scriptBridge.isInstanceOf(compId, className) -> boolean
static bool js_ScriptBridge_isInstanceOf(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 2) {
        uint32_t compId = 0;
        bool ok = sevalue_to_native(args[0], &compId, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing compId argument");

        ccstd::string className;
        ok = sevalue_to_native(args[1], &className, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing className argument");

        bool result = cobj->isInstanceOf(compId, className);
        s.rval().setBoolean(result);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 2);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_isInstanceOf)

// ─── collectAssetRefs ──────────────────────────────────────────────────────
// JS: scriptBridge.collectAssetRefs(compId) -> jsb.Asset[]
static bool js_ScriptBridge_collectAssetRefs(se::State &s) { // NOLINT(readability-identifier-naming)
    auto *cobj = SE_THIS_OBJECT<cc::ScriptBridge>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    size_t argc = args.size();
    if (argc == 1) {
        uint32_t compId = 0;
        bool ok = sevalue_to_native(args[0], &compId, s.thisObject());
        SE_PRECONDITION2(ok, false, "Error processing compId argument");

        auto refs = cobj->collectAssetRefs(compId);
        se::HandleObject arrObj(se::Object::createArrayObject(static_cast<uint32_t>(refs.size())));
        for (size_t i = 0; i < refs.size(); ++i) {
            se::Value assetVal;
            native_ptr_to_seval<cc::Asset>(refs[i], &assetVal);
            arrObj->setArrayElement(static_cast<uint32_t>(i), assetVal);
        }
        s.rval().setObject(arrObj);
        return true;
    }
    SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
    return false;
}
SE_BIND_FUNC(js_ScriptBridge_collectAssetRefs)

// ─── Registration ──────────────────────────────────────────────────────────
bool register_all_script_bridge(se::Object *obj) { // NOLINT(readability-identifier-naming)
    // Get the jsb namespace
    se::Value nsVal;
    if (!obj->getProperty("jsb", &nsVal)) {
        se::HandleObject jsobj(se::Object::createPlainObject());
        nsVal.setObject(jsobj);
        obj->setProperty("jsb", nsVal);
    }
    se::Object *ns = nsVal.toObject();

    // Create ScriptBridge class in jsb namespace
    // No constructor — singleton accessed via getInstance()
    auto *cls = se::Class::create("ScriptBridge", ns, nullptr, nullptr);

    cls->defineStaticFunction("getInstance", _SE(js_ScriptBridge_getInstance));

    cls->defineFunction("init", _SE(js_ScriptBridge_init));
    cls->defineFunction("shutdown", _SE(js_ScriptBridge_shutdown));
    cls->defineFunction("registerScriptClass", _SE(js_ScriptBridge_registerScriptClass));
    cls->defineFunction("registerScriptInstance", _SE(js_ScriptBridge_registerScriptInstance));
    cls->defineFunction("unregisterScriptInstance", _SE(js_ScriptBridge_unregisterScriptInstance));
    cls->defineFunction("invokeStartBatch", _SE(js_ScriptBridge_invokeStartBatch));
    cls->defineFunction("invokeUpdateBatch", _SE(js_ScriptBridge_invokeUpdateBatch));
    cls->defineFunction("invokeLateUpdateBatch", _SE(js_ScriptBridge_invokeLateUpdateBatch));
    cls->defineFunction("invokeOnDestroy", _SE(js_ScriptBridge_invokeOnDestroy));
    cls->defineFunction("invokeOnEnable", _SE(js_ScriptBridge_invokeOnEnable));
    cls->defineFunction("invokeOnDisable", _SE(js_ScriptBridge_invokeOnDisable));
    cls->defineFunction("invokeOnLoad", _SE(js_ScriptBridge_invokeOnLoad));
    cls->defineFunction("isInstanceOf", _SE(js_ScriptBridge_isInstanceOf));
    cls->defineFunction("collectAssetRefs", _SE(js_ScriptBridge_collectAssetRefs));

    cls->install();

    JSBClassType::registerClass<cc::ScriptBridge>(cls);

    __jsb_cc_ScriptBridge_proto = cls->getProto();
    __jsb_cc_ScriptBridge_class = cls;

    se::ScriptEngine::getInstance()->clearException();

    return true;
}
