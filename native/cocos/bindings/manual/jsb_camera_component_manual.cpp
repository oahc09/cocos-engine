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

#include "jsb_camera_component_manual.h"

#include "bindings/jswrapper/SeApi.h"
#include "bindings/manual/jsb_classtype.h"
#include "bindings/manual/jsb_conversions.h"
#include "core/components/CameraComponent.h"
#include "scene/Camera.h"

namespace {
se::Object *__jsb_cc_CameraComponent_proto = nullptr; // NOLINT
se::Class  *__jsb_cc_CameraComponent_class  = nullptr; // NOLINT
} // namespace

// ─── finalize (GC callback) ────────────────────────────────────────────────
static bool js_CameraComponent_finalize(se::State &s) { // NOLINT
    // Component lifecycle is managed by engine; do not delete here.
    return true;
}
SE_BIND_FINALIZE_FUNC(js_CameraComponent_finalize)

// ─── Property: projection ──────────────────────────────────────────────────
static bool js_CameraComponent_getProjection(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::CameraComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setInt32(cobj->getProjection());
    return true;
}
SE_BIND_PROP_GET(js_CameraComponent_getProjection)

static bool js_CameraComponent_setProjection(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::CameraComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cobj->setProjection(args[0].toInt32());
    return true;
}
SE_BIND_PROP_SET(js_CameraComponent_setProjection)

// ─── Property: near ────────────────────────────────────────────────────────
static bool js_CameraComponent_getNear(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::CameraComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getNear());
    return true;
}
SE_BIND_PROP_GET(js_CameraComponent_getNear)

static bool js_CameraComponent_setNear(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::CameraComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cobj->setNear(args[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_CameraComponent_setNear)

// ─── Property: far ─────────────────────────────────────────────────────────
static bool js_CameraComponent_getFar(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::CameraComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getFar());
    return true;
}
SE_BIND_PROP_GET(js_CameraComponent_getFar)

static bool js_CameraComponent_setFar(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::CameraComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cobj->setFar(args[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_CameraComponent_setFar)

// ─── Property: fov ─────────────────────────────────────────────────────────
static bool js_CameraComponent_getFov(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::CameraComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getFov());
    return true;
}
SE_BIND_PROP_GET(js_CameraComponent_getFov)

static bool js_CameraComponent_setFov(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::CameraComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cobj->setFov(args[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_CameraComponent_setFov)

// ─── Method: setViewport(x, y, width, height) ──────────────────────────────
static bool js_CameraComponent_setViewport(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::CameraComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(args.size() >= 4, false, "Invalid number of arguments: %d, expected 4", (int)args.size());
    cobj->setViewport(args[0].toFloat(), args[1].toFloat(), args[2].toFloat(), args[3].toFloat());
    return true;
}
SE_BIND_FUNC(js_CameraComponent_setViewport)

// ─── Method: getRenderCamera() ─────────────────────────────────────────────
static bool js_CameraComponent_getRenderCamera(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::CameraComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    auto *camera = cobj->getRenderCamera();
    if (camera) {
        native_ptr_to_seval<cc::scene::Camera>(camera, &s.rval());
    } else {
        s.rval().setNull();
    }
    return true;
}
SE_BIND_FUNC(js_CameraComponent_getRenderCamera)

// ─── Registration ──────────────────────────────────────────────────────────
bool register_all_camera_component(se::Object *obj) { // NOLINT(readability-identifier-naming)
    auto *cls = se::Class::create("CameraComponent", obj, nullptr, nullptr);

    cls->defineProperty("projection", _SE(js_CameraComponent_getProjection), _SE(js_CameraComponent_setProjection));
    cls->defineProperty("near", _SE(js_CameraComponent_getNear), _SE(js_CameraComponent_setNear));
    cls->defineProperty("far", _SE(js_CameraComponent_getFar), _SE(js_CameraComponent_setFar));
    cls->defineProperty("fov", _SE(js_CameraComponent_getFov), _SE(js_CameraComponent_setFov));

    cls->defineFunction("setViewport", _SE(js_CameraComponent_setViewport));
    cls->defineFunction("getRenderCamera", _SE(js_CameraComponent_getRenderCamera));

    cls->defineFinalizeFunction(_SE(js_CameraComponent_finalize));

    cls->install();

    __jsb_cc_CameraComponent_proto = cls->getProto();
    __jsb_cc_CameraComponent_class = cls;

    JSBClassType::registerClass<cc::CameraComponent>(cls);

    se::ScriptEngine::getInstance()->clearException();

    return true;
}
