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

#include "jsb_mesh_renderer_manual.h"

#include "3d/assets/Mesh.h"
#include "bindings/jswrapper/SeApi.h"
#include "bindings/manual/jsb_classtype.h"
#include "bindings/manual/jsb_conversions.h"
#include "core/assets/Material.h"
#include "core/components/MeshRendererComponent.h"
#include "scene/Model.h"

namespace {
se::Object *__jsb_cc_MeshRendererComponent_proto = nullptr; // NOLINT
se::Class  *__jsb_cc_MeshRendererComponent_class  = nullptr; // NOLINT
} // namespace

// ─── finalize (GC callback) ────────────────────────────────────────────────
static bool js_MeshRendererComponent_finalize(se::State &s) { // NOLINT
    // Component lifecycle is managed by engine; do not delete here.
    return true;
}
SE_BIND_FINALIZE_FUNC(js_MeshRendererComponent_finalize)

// ─── Property: shadowCastingMode ────────────────────────────────────────────
static bool js_MeshRendererComponent_getShadowCastingMode(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::MeshRendererComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setInt32(cobj->getShadowCastingMode());
    return true;
}
SE_BIND_PROP_GET(js_MeshRendererComponent_getShadowCastingMode)

static bool js_MeshRendererComponent_setShadowCastingMode(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::MeshRendererComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cobj->setShadowCastingMode(args[0].toInt32());
    return true;
}
SE_BIND_PROP_SET(js_MeshRendererComponent_setShadowCastingMode)

// ─── Property: receiveShadow ────────────────────────────────────────────────
static bool js_MeshRendererComponent_getReceiveShadow(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::MeshRendererComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    s.rval().setBoolean(cobj->isReceiveShadow());
    return true;
}
SE_BIND_PROP_GET(js_MeshRendererComponent_getReceiveShadow)

static bool js_MeshRendererComponent_setReceiveShadow(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::MeshRendererComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cobj->setReceiveShadow(args[0].toBoolean());
    return true;
}
SE_BIND_PROP_SET(js_MeshRendererComponent_setReceiveShadow)

// ─── Method: setMesh(mesh) ──────────────────────────────────────────────────
static bool js_MeshRendererComponent_setMesh(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::MeshRendererComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cc::Mesh *mesh = nullptr;
    if (args[0].isObject()) {
        mesh = static_cast<cc::Mesh *>(args[0].toObject()->getPrivateData());
    }
    cobj->setMesh(mesh);
    return true;
}
SE_BIND_FUNC(js_MeshRendererComponent_setMesh)

// ─── Method: getMesh() ──────────────────────────────────────────────────────
static bool js_MeshRendererComponent_getMesh(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::MeshRendererComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    auto *mesh = cobj->getMesh();
    if (mesh) {
        native_ptr_to_seval<cc::Mesh>(mesh, &s.rval());
    } else {
        s.rval().setNull();
    }
    return true;
}
SE_BIND_FUNC(js_MeshRendererComponent_getMesh)

// ─── Method: setMaterial(material, subModelIndex?) ──────────────────────────
static bool js_MeshRendererComponent_setMaterial(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::MeshRendererComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    SE_PRECONDITION2(!args.empty(), false, "Invalid number of arguments");
    cc::Material *material = nullptr;
    if (args[0].isObject()) {
        material = static_cast<cc::Material *>(args[0].toObject()->getPrivateData());
    }
    uint32_t subIdx = 0;
    if (args.size() > 1) {
        subIdx = static_cast<uint32_t>(args[1].toUint32());
    }
    cobj->setMaterial(material, subIdx);
    return true;
}
SE_BIND_FUNC(js_MeshRendererComponent_setMaterial)

// ─── Method: getMaterial(subModelIndex?) ────────────────────────────────────
static bool js_MeshRendererComponent_getMaterial(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::MeshRendererComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    const auto &args = s.args();
    uint32_t subIdx = 0;
    if (!args.empty()) {
        subIdx = static_cast<uint32_t>(args[0].toUint32());
    }
    auto *material = cobj->getMaterial(subIdx);
    if (material) {
        native_ptr_to_seval<cc::Material>(material, &s.rval());
    } else {
        s.rval().setNull();
    }
    return true;
}
SE_BIND_FUNC(js_MeshRendererComponent_getMaterial)

// ─── Method: getRenderModel() ───────────────────────────────────────────────
static bool js_MeshRendererComponent_getRenderModel(se::State &s) { // NOLINT
    auto *cobj = SE_THIS_OBJECT<cc::MeshRendererComponent>(s);
    SE_PRECONDITION2(cobj, false, "Invalid Native Object");
    auto *model = cobj->getRenderModel();
    if (model) {
        native_ptr_to_seval<cc::scene::Model>(model, &s.rval());
    } else {
        s.rval().setNull();
    }
    return true;
}
SE_BIND_FUNC(js_MeshRendererComponent_getRenderModel)

// ─── Registration ──────────────────────────────────────────────────────────
bool register_all_mesh_renderer(se::Object *obj) { // NOLINT(readability-identifier-naming)
    auto *cls = se::Class::create("MeshRendererComponent", obj, nullptr, nullptr);

    cls->defineProperty("shadowCastingMode", _SE(js_MeshRendererComponent_getShadowCastingMode), _SE(js_MeshRendererComponent_setShadowCastingMode));
    cls->defineProperty("receiveShadow", _SE(js_MeshRendererComponent_getReceiveShadow), _SE(js_MeshRendererComponent_setReceiveShadow));

    cls->defineFunction("setMesh", _SE(js_MeshRendererComponent_setMesh));
    cls->defineFunction("getMesh", _SE(js_MeshRendererComponent_getMesh));
    cls->defineFunction("setMaterial", _SE(js_MeshRendererComponent_setMaterial));
    cls->defineFunction("getMaterial", _SE(js_MeshRendererComponent_getMaterial));
    cls->defineFunction("getRenderModel", _SE(js_MeshRendererComponent_getRenderModel));

    cls->defineFinalizeFunction(_SE(js_MeshRendererComponent_finalize));

    cls->install();

    __jsb_cc_MeshRendererComponent_proto = cls->getProto();
    __jsb_cc_MeshRendererComponent_class = cls;

    JSBClassType::registerClass<cc::MeshRendererComponent>(cls);

    se::ScriptEngine::getInstance()->clearException();

    return true;
}
