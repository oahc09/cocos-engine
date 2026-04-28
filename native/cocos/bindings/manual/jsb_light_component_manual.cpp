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

#include "jsb_light_component_manual.h"

#include "bindings/jswrapper/SeApi.h"
#include "bindings/manual/jsb_classtype.h"
#include "bindings/manual/jsb_conversions.h"
#include "bindings/manual/jsb_conversions_spec.h"
#include "core/components/LightComponent.h"
#include "math/Vec3.h"
#include "scene/Light.h"
#include "scene/DirectionalLight.h"
#include "scene/SphereLight.h"
#include "scene/SpotLight.h"
#include "scene/PointLight.h"
#include "scene/RangedDirectionalLight.h"

// =====================================================================
// DirectionalLightComponent
// =====================================================================
namespace {
se::Object *__jsb_cc_DirectionalLightComponent_proto = nullptr; // NOLINT
se::Class  *__jsb_cc_DirectionalLightComponent_class  = nullptr; // NOLINT
} // namespace

static bool js_DirectionalLightComponent_finalize(se::State &s) { return true; } // NOLINT
SE_BIND_FINALIZE_FUNC(js_DirectionalLightComponent_finalize)

// --- color (Vec3) ---
static bool js_DirectionalLightComponent_getColor(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    return Vec3_to_seval(cobj->getColor(), &s.rval());
}
SE_BIND_PROP_GET(js_DirectionalLightComponent_getColor)

static bool js_DirectionalLightComponent_setColor(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cc::Vec3 color;
    sevalue_to_native(args[0], &color, s.thisObject());
    cobj->setColor(color);
    return true;
}
SE_BIND_PROP_SET(js_DirectionalLightComponent_setColor)

// --- illuminance ---
static bool js_DirectionalLightComponent_getIlluminance(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getIlluminance());
    return true;
}
SE_BIND_PROP_GET(js_DirectionalLightComponent_getIlluminance)

static bool js_DirectionalLightComponent_setIlluminance(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cobj->setIlluminance(args[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_DirectionalLightComponent_setIlluminance)

// --- shadowEnabled ---
static bool js_DirectionalLightComponent_getShadowEnabled(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setBoolean(cobj->isShadowEnabled());
    return true;
}
SE_BIND_PROP_GET(js_DirectionalLightComponent_getShadowEnabled)

static bool js_DirectionalLightComponent_setShadowEnabled(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cobj->setShadowEnabled(args[0].toBoolean());
    return true;
}
SE_BIND_PROP_SET(js_DirectionalLightComponent_setShadowEnabled)

// --- shadowBias ---
static bool js_DirectionalLightComponent_getShadowBias(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getShadowBias());
    return true;
}
SE_BIND_PROP_GET(js_DirectionalLightComponent_getShadowBias)

static bool js_DirectionalLightComponent_setShadowBias(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cobj->setShadowBias(args[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_DirectionalLightComponent_setShadowBias)

// --- shadowNormalBias ---
static bool js_DirectionalLightComponent_getShadowNormalBias(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getShadowNormalBias());
    return true;
}
SE_BIND_PROP_GET(js_DirectionalLightComponent_getShadowNormalBias)

static bool js_DirectionalLightComponent_setShadowNormalBias(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cobj->setShadowNormalBias(args[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_DirectionalLightComponent_setShadowNormalBias)

// --- shadowDistance ---
static bool js_DirectionalLightComponent_getShadowDistance(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getShadowDistance());
    return true;
}
SE_BIND_PROP_GET(js_DirectionalLightComponent_getShadowDistance)

static bool js_DirectionalLightComponent_setShadowDistance(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cobj->setShadowDistance(args[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_DirectionalLightComponent_setShadowDistance)

// --- getRenderLight ---
static bool js_DirectionalLightComponent_getRenderLight(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::DirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    auto *light = cobj->getRenderLight();
    if (light) {
        native_ptr_to_seval<cc::scene::Light>(light, &s.rval());
    } else {
        s.rval().setNull();
    }
    return true;
}
SE_BIND_FUNC(js_DirectionalLightComponent_getRenderLight)

// =====================================================================
// SphereLightComponent
// =====================================================================
namespace {
se::Object *__jsb_cc_SphereLightComponent_proto = nullptr; // NOLINT
se::Class  *__jsb_cc_SphereLightComponent_class  = nullptr; // NOLINT
} // namespace

static bool js_SphereLightComponent_finalize(se::State &s) { return true; } // NOLINT
SE_BIND_FINALIZE_FUNC(js_SphereLightComponent_finalize)

static bool js_SphereLightComponent_getColor(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SphereLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    return Vec3_to_seval(cobj->getColor(), &s.rval());
}
SE_BIND_PROP_GET(js_SphereLightComponent_getColor)

static bool js_SphereLightComponent_setColor(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SphereLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cc::Vec3 color;
    sevalue_to_native(args[0], &color, s.thisObject());
    cobj->setColor(color);
    return true;
}
SE_BIND_PROP_SET(js_SphereLightComponent_setColor)

static bool js_SphereLightComponent_getRange(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SphereLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getRange());
    return true;
}
SE_BIND_PROP_GET(js_SphereLightComponent_getRange)

static bool js_SphereLightComponent_setRange(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SphereLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cobj->setRange(s.args()[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_SphereLightComponent_setRange)

static bool js_SphereLightComponent_getSize(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SphereLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getSize());
    return true;
}
SE_BIND_PROP_GET(js_SphereLightComponent_getSize)

static bool js_SphereLightComponent_setSize(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SphereLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cobj->setSize(s.args()[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_SphereLightComponent_setSize)

static bool js_SphereLightComponent_getLuminance(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SphereLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getLuminance());
    return true;
}
SE_BIND_PROP_GET(js_SphereLightComponent_getLuminance)

static bool js_SphereLightComponent_setLuminance(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SphereLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cobj->setLuminance(s.args()[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_SphereLightComponent_setLuminance)

static bool js_SphereLightComponent_getRenderLight(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SphereLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    auto *light = cobj->getRenderLight();
    if (light) native_ptr_to_seval<cc::scene::Light>(light, &s.rval());
    else s.rval().setNull();
    return true;
}
SE_BIND_FUNC(js_SphereLightComponent_getRenderLight)

// =====================================================================
// SpotLightComponent
// =====================================================================
namespace {
se::Object *__jsb_cc_SpotLightComponent_proto = nullptr; // NOLINT
se::Class  *__jsb_cc_SpotLightComponent_class  = nullptr; // NOLINT
} // namespace

static bool js_SpotLightComponent_finalize(se::State &s) { return true; } // NOLINT
SE_BIND_FINALIZE_FUNC(js_SpotLightComponent_finalize)

static bool js_SpotLightComponent_getColor(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    return Vec3_to_seval(cobj->getColor(), &s.rval());
}
SE_BIND_PROP_GET(js_SpotLightComponent_getColor)

static bool js_SpotLightComponent_setColor(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cc::Vec3 color;
    sevalue_to_native(s.args()[0], &color, s.thisObject());
    cobj->setColor(color);
    return true;
}
SE_BIND_PROP_SET(js_SpotLightComponent_setColor)

static bool js_SpotLightComponent_getRange(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getRange());
    return true;
}
SE_BIND_PROP_GET(js_SpotLightComponent_getRange)

static bool js_SpotLightComponent_setRange(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cobj->setRange(s.args()[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_SpotLightComponent_setRange)

static bool js_SpotLightComponent_getSize(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getSize());
    return true;
}
SE_BIND_PROP_GET(js_SpotLightComponent_getSize)

static bool js_SpotLightComponent_setSize(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cobj->setSize(s.args()[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_SpotLightComponent_setSize)

static bool js_SpotLightComponent_getSpotAngle(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getSpotAngle());
    return true;
}
SE_BIND_PROP_GET(js_SpotLightComponent_getSpotAngle)

static bool js_SpotLightComponent_setSpotAngle(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cobj->setSpotAngle(s.args()[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_SpotLightComponent_setSpotAngle)

static bool js_SpotLightComponent_getLuminance(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getLuminance());
    return true;
}
SE_BIND_PROP_GET(js_SpotLightComponent_getLuminance)

static bool js_SpotLightComponent_setLuminance(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cobj->setLuminance(s.args()[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_SpotLightComponent_setLuminance)

static bool js_SpotLightComponent_getShadowEnabled(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setBoolean(cobj->isShadowEnabled());
    return true;
}
SE_BIND_PROP_GET(js_SpotLightComponent_getShadowEnabled)

static bool js_SpotLightComponent_setShadowEnabled(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cobj->setShadowEnabled(s.args()[0].toBoolean());
    return true;
}
SE_BIND_PROP_SET(js_SpotLightComponent_setShadowEnabled)

static bool js_SpotLightComponent_getRenderLight(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::SpotLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    auto *light = cobj->getRenderLight();
    if (light) native_ptr_to_seval<cc::scene::Light>(light, &s.rval());
    else s.rval().setNull();
    return true;
}
SE_BIND_FUNC(js_SpotLightComponent_getRenderLight)

// =====================================================================
// PointLightComponent
// =====================================================================
namespace {
se::Object *__jsb_cc_PointLightComponent_proto = nullptr; // NOLINT
se::Class  *__jsb_cc_PointLightComponent_class  = nullptr; // NOLINT
} // namespace

static bool js_PointLightComponent_finalize(se::State &s) { return true; } // NOLINT
SE_BIND_FINALIZE_FUNC(js_PointLightComponent_finalize)

static bool js_PointLightComponent_getColor(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::PointLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    return Vec3_to_seval(cobj->getColor(), &s.rval());
}
SE_BIND_PROP_GET(js_PointLightComponent_getColor)

static bool js_PointLightComponent_setColor(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::PointLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cc::Vec3 color;
    sevalue_to_native(s.args()[0], &color, s.thisObject());
    cobj->setColor(color);
    return true;
}
SE_BIND_PROP_SET(js_PointLightComponent_setColor)

static bool js_PointLightComponent_getRange(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::PointLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getRange());
    return true;
}
SE_BIND_PROP_GET(js_PointLightComponent_getRange)

static bool js_PointLightComponent_setRange(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::PointLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cobj->setRange(s.args()[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_PointLightComponent_setRange)

static bool js_PointLightComponent_getLuminance(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::PointLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getLuminance());
    return true;
}
SE_BIND_PROP_GET(js_PointLightComponent_getLuminance)

static bool js_PointLightComponent_setLuminance(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::PointLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cobj->setLuminance(s.args()[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_PointLightComponent_setLuminance)

static bool js_PointLightComponent_getRenderLight(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::PointLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    auto *light = cobj->getRenderLight();
    if (light) native_ptr_to_seval<cc::scene::Light>(light, &s.rval());
    else s.rval().setNull();
    return true;
}
SE_BIND_FUNC(js_PointLightComponent_getRenderLight)

// =====================================================================
// RangedDirectionalLightComponent
// =====================================================================
namespace {
se::Object *__jsb_cc_RangedDirLightComponent_proto = nullptr; // NOLINT
se::Class  *__jsb_cc_RangedDirLightComponent_class  = nullptr; // NOLINT
} // namespace

static bool js_RangedDirLightComponent_finalize(se::State &s) { return true; } // NOLINT
SE_BIND_FINALIZE_FUNC(js_RangedDirLightComponent_finalize)

static bool js_RangedDirLightComponent_getColor(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::RangedDirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    return Vec3_to_seval(cobj->getColor(), &s.rval());
}
SE_BIND_PROP_GET(js_RangedDirLightComponent_getColor)

static bool js_RangedDirLightComponent_setColor(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::RangedDirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cc::Vec3 color;
    sevalue_to_native(s.args()[0], &color, s.thisObject());
    cobj->setColor(color);
    return true;
}
SE_BIND_PROP_SET(js_RangedDirLightComponent_setColor)

static bool js_RangedDirLightComponent_getIlluminance(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::RangedDirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setFloat(cobj->getIlluminance());
    return true;
}
SE_BIND_PROP_GET(js_RangedDirLightComponent_getIlluminance)

static bool js_RangedDirLightComponent_setIlluminance(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::RangedDirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    cobj->setIlluminance(s.args()[0].toFloat());
    return true;
}
SE_BIND_PROP_SET(js_RangedDirLightComponent_setIlluminance)

static bool js_RangedDirLightComponent_getRenderLight(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::RangedDirectionalLightComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    auto *light = cobj->getRenderLight();
    if (light) native_ptr_to_seval<cc::scene::Light>(light, &s.rval());
    else s.rval().setNull();
    return true;
}
SE_BIND_FUNC(js_RangedDirLightComponent_getRenderLight)

// =====================================================================
// Registration
// =====================================================================
static bool register_DirectionalLightComponent(se::Object *obj) { // NOLINT
    auto *cls = se::Class::create("DirectionalLightComponent", obj, nullptr, nullptr);

    cls->defineProperty("color",           _SE(js_DirectionalLightComponent_getColor),           _SE(js_DirectionalLightComponent_setColor));
    cls->defineProperty("illuminance",     _SE(js_DirectionalLightComponent_getIlluminance),     _SE(js_DirectionalLightComponent_setIlluminance));
    cls->defineProperty("shadowEnabled",   _SE(js_DirectionalLightComponent_getShadowEnabled),   _SE(js_DirectionalLightComponent_setShadowEnabled));
    cls->defineProperty("shadowBias",      _SE(js_DirectionalLightComponent_getShadowBias),      _SE(js_DirectionalLightComponent_setShadowBias));
    cls->defineProperty("shadowNormalBias",_SE(js_DirectionalLightComponent_getShadowNormalBias),_SE(js_DirectionalLightComponent_setShadowNormalBias));
    cls->defineProperty("shadowDistance",  _SE(js_DirectionalLightComponent_getShadowDistance),  _SE(js_DirectionalLightComponent_setShadowDistance));

    cls->defineFunction("getRenderLight",  _SE(js_DirectionalLightComponent_getRenderLight));

    cls->defineFinalizeFunction(_SE(js_DirectionalLightComponent_finalize));
    cls->install();

    __jsb_cc_DirectionalLightComponent_proto = cls->getProto();
    __jsb_cc_DirectionalLightComponent_class = cls;
    JSBClassType::registerClass<cc::DirectionalLightComponent>(cls);
    se::ScriptEngine::getInstance()->clearException();
    return true;
}

static bool register_SphereLightComponent(se::Object *obj) { // NOLINT
    auto *cls = se::Class::create("SphereLightComponent", obj, nullptr, nullptr);

    cls->defineProperty("color",     _SE(js_SphereLightComponent_getColor),     _SE(js_SphereLightComponent_setColor));
    cls->defineProperty("range",     _SE(js_SphereLightComponent_getRange),     _SE(js_SphereLightComponent_setRange));
    cls->defineProperty("size",      _SE(js_SphereLightComponent_getSize),      _SE(js_SphereLightComponent_setSize));
    cls->defineProperty("luminance", _SE(js_SphereLightComponent_getLuminance), _SE(js_SphereLightComponent_setLuminance));

    cls->defineFunction("getRenderLight", _SE(js_SphereLightComponent_getRenderLight));

    cls->defineFinalizeFunction(_SE(js_SphereLightComponent_finalize));
    cls->install();

    __jsb_cc_SphereLightComponent_proto = cls->getProto();
    __jsb_cc_SphereLightComponent_class = cls;
    JSBClassType::registerClass<cc::SphereLightComponent>(cls);
    se::ScriptEngine::getInstance()->clearException();
    return true;
}

static bool register_SpotLightComponent(se::Object *obj) { // NOLINT
    auto *cls = se::Class::create("SpotLightComponent", obj, nullptr, nullptr);

    cls->defineProperty("color",        _SE(js_SpotLightComponent_getColor),        _SE(js_SpotLightComponent_setColor));
    cls->defineProperty("range",        _SE(js_SpotLightComponent_getRange),        _SE(js_SpotLightComponent_setRange));
    cls->defineProperty("size",         _SE(js_SpotLightComponent_getSize),         _SE(js_SpotLightComponent_setSize));
    cls->defineProperty("spotAngle",    _SE(js_SpotLightComponent_getSpotAngle),    _SE(js_SpotLightComponent_setSpotAngle));
    cls->defineProperty("luminance",    _SE(js_SpotLightComponent_getLuminance),    _SE(js_SpotLightComponent_setLuminance));
    cls->defineProperty("shadowEnabled",_SE(js_SpotLightComponent_getShadowEnabled),_SE(js_SpotLightComponent_setShadowEnabled));

    cls->defineFunction("getRenderLight", _SE(js_SpotLightComponent_getRenderLight));

    cls->defineFinalizeFunction(_SE(js_SpotLightComponent_finalize));
    cls->install();

    __jsb_cc_SpotLightComponent_proto = cls->getProto();
    __jsb_cc_SpotLightComponent_class = cls;
    JSBClassType::registerClass<cc::SpotLightComponent>(cls);
    se::ScriptEngine::getInstance()->clearException();
    return true;
}

static bool register_PointLightComponent(se::Object *obj) { // NOLINT
    auto *cls = se::Class::create("PointLightComponent", obj, nullptr, nullptr);

    cls->defineProperty("color",     _SE(js_PointLightComponent_getColor),     _SE(js_PointLightComponent_setColor));
    cls->defineProperty("range",     _SE(js_PointLightComponent_getRange),     _SE(js_PointLightComponent_setRange));
    cls->defineProperty("luminance", _SE(js_PointLightComponent_getLuminance), _SE(js_PointLightComponent_setLuminance));

    cls->defineFunction("getRenderLight", _SE(js_PointLightComponent_getRenderLight));

    cls->defineFinalizeFunction(_SE(js_PointLightComponent_finalize));
    cls->install();

    __jsb_cc_PointLightComponent_proto = cls->getProto();
    __jsb_cc_PointLightComponent_class = cls;
    JSBClassType::registerClass<cc::PointLightComponent>(cls);
    se::ScriptEngine::getInstance()->clearException();
    return true;
}

static bool register_RangedDirLightComponent(se::Object *obj) { // NOLINT
    auto *cls = se::Class::create("RangedDirectionalLightComponent", obj, nullptr, nullptr);

    cls->defineProperty("color",       _SE(js_RangedDirLightComponent_getColor),       _SE(js_RangedDirLightComponent_setColor));
    cls->defineProperty("illuminance", _SE(js_RangedDirLightComponent_getIlluminance), _SE(js_RangedDirLightComponent_setIlluminance));

    cls->defineFunction("getRenderLight", _SE(js_RangedDirLightComponent_getRenderLight));

    cls->defineFinalizeFunction(_SE(js_RangedDirLightComponent_finalize));
    cls->install();

    __jsb_cc_RangedDirLightComponent_proto = cls->getProto();
    __jsb_cc_RangedDirLightComponent_class = cls;
    JSBClassType::registerClass<cc::RangedDirectionalLightComponent>(cls);
    se::ScriptEngine::getInstance()->clearException();
    return true;
}

// ─── Master registration ──────────────────────────────────────────────────
bool register_all_light_components(se::Object *obj) { // NOLINT(readability-identifier-naming)
    register_DirectionalLightComponent(obj);
    register_SphereLightComponent(obj);
    register_SpotLightComponent(obj);
    register_PointLightComponent(obj);
    register_RangedDirLightComponent(obj);
    return true;
}
