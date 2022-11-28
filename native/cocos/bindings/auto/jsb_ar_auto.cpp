// clang-format off

/* ----------------------------------------------------------------------------
 * This file was automatically generated by SWIG (https://www.swig.org).
 * Version 4.1.0
 *
 * Do not make changes to this file unless you know what you are doing - modify
 * the SWIG interface file instead.
 * ----------------------------------------------------------------------------- */

/****************************************************************************
 Copyright (c) 2022 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated engine source code (the "Software"), a limited,
 worldwide, royalty-free, non-assignable, revocable and non-exclusive license
 to use Cocos Creator solely to develop games on your target platforms. You shall
 not use Cocos Creator software for developing other software or tools that's
 used for developing games. You are not granted to publish, distribute,
 sublicense, and/or sell copies of Cocos Creator.

 The software or tools in this License Agreement are licensed, not sold.
 Xiamen Yaji Software Co., Ltd. reserves all rights not expressly granted to you.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#elif defined(__GNUC__) || defined(__GNUG__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4101)
#endif


#define SWIG_STD_MOVE(OBJ) std::move(OBJ)


#include <stdio.h>


#include "bindings/jswrapper/SeApi.h"
#include "bindings/manual/jsb_conversions.h"
#include "bindings/manual/jsb_global.h"


#include "bindings/auto/jsb_ar_auto.h"

using namespace cc::ar;



se::Class* __jsb_cc_ar_ARModule_class = nullptr;
se::Object* __jsb_cc_ar_ARModule_proto = nullptr;
SE_DECLARE_FINALIZE_FUNC(js_delete_cc_ar_ARModule) 

static bool js_cc_ar_ARModule_get_static(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *result = 0 ;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    result = (cc::ar::ARModule *)cc::ar::ARModule::get();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    SE_HOLD_RETURN_VALUE(result, s.thisObject(), s.rval()); 
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_get_static) 

static bool js_new_cc_ar_ARModule(se::State& s) // NOLINT(readability-identifier-naming)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    
    cc::ar::ARModule *result;
    result = (cc::ar::ARModule *)new cc::ar::ARModule();
    
    
    auto *ptr = JSB_MAKE_PRIVATE_OBJECT_WITH_INSTANCE(result);
    s.thisObject()->setPrivateObject(ptr);
    return true;
}
SE_BIND_CTOR(js_new_cc_ar_ARModule, __jsb_cc_ar_ARModule_class, js_delete_cc_ar_ARModule)

static bool js_delete_cc_ar_ARModule(se::State& s)
{
    return true;
}
SE_BIND_FINALIZE_FUNC(js_delete_cc_ar_ARModule) 

static bool js_cc_ar_ARModule_config(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    int arg2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    (arg1)->config(arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_config) 

static bool js_cc_ar_ARModule_getSupportMask(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    int result;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = (int)(arg1)->getSupportMask();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject()); 
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getSupportMask) 

static bool js_cc_ar_ARModule_start(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    (arg1)->start();
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_start) 

static bool js_cc_ar_ARModule_onResume(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    (arg1)->onResume();
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_onResume) 

static bool js_cc_ar_ARModule_onPause(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    (arg1)->onPause();
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_onPause) 

static bool js_cc_ar_ARModule_update(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    (arg1)->update();
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_update) 

static bool js_cc_ar_ARModule_getAPIState(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    int result;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = (int)(arg1)->getAPIState();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject()); 
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getAPIState) 

static bool js_cc_ar_ARModule_setCameraId(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    std::string *arg2 = 0 ;
    std::string temp2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &temp2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    arg2 = &temp2;
    
    (arg1)->setCameraId((std::string const &)*arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_setCameraId) 

static bool js_cc_ar_ARModule_getCameraId(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    std::string *result = 0 ;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = (std::string *) &((cc::ar::ARModule const *)arg1)->getCameraId();
    
    ok &= nativevalue_to_se(*result, s.rval(), s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    SE_HOLD_RETURN_VALUE(*result, s.thisObject(), s.rval()); 
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getCameraId) 

static bool js_cc_ar_ARModule_getCameraPose(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    Pose result;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = ((cc::ar::ARModule const *)arg1)->getCameraPose();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject() /*ctx*/);
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    SE_HOLD_RETURN_VALUE(result, s.thisObject(), s.rval());
    
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getCameraPose) 

static bool js_cc_ar_ARModule_getCameraViewMatrix(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    Matrix result;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = ((cc::ar::ARModule const *)arg1)->getCameraViewMatrix();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject() /*ctx*/);
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    SE_HOLD_RETURN_VALUE(result, s.thisObject(), s.rval());
    
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getCameraViewMatrix) 

static bool js_cc_ar_ARModule_getCameraProjectionMatrix(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    Matrix result;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = ((cc::ar::ARModule const *)arg1)->getCameraProjectionMatrix();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject() /*ctx*/);
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    SE_HOLD_RETURN_VALUE(result, s.thisObject(), s.rval());
    
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getCameraProjectionMatrix) 

static bool js_cc_ar_ARModule_getCameraTexCoords(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    TexCoords result;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = ((cc::ar::ARModule const *)arg1)->getCameraTexCoords();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject() /*ctx*/);
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    SE_HOLD_RETURN_VALUE(result, s.thisObject(), s.rval());
    
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getCameraTexCoords) 

static bool js_cc_ar_ARModule_setDisplayGeometry(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    uint32_t arg2 ;
    uint32_t arg3 ;
    uint32_t arg4 ;
    
    if(argc != 3) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 3);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    
    
    ok &= sevalue_to_native(args[1], &arg3, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    
    
    ok &= sevalue_to_native(args[2], &arg4, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    
    ((cc::ar::ARModule const *)arg1)->setDisplayGeometry(arg2,arg3,arg4);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_setDisplayGeometry) 

static bool js_cc_ar_ARModule_setCameraClip(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    float arg2 ;
    float arg3 ;
    
    if(argc != 2) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 2);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    
    ok &= sevalue_to_native(args[1], &arg3, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    ((cc::ar::ARModule const *)arg1)->setCameraClip(arg2,arg3);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_setCameraClip) 

static bool js_cc_ar_ARModule_setCameraTextureName(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    int arg2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    ((cc::ar::ARModule const *)arg1)->setCameraTextureName(arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_setCameraTextureName) 

static bool js_cc_ar_ARModule_getCameraTextureRef(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    void *result = 0 ;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = (void *)((cc::ar::ARModule const *)arg1)->getCameraTextureRef();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    SE_HOLD_RETURN_VALUE(result, s.thisObject(), s.rval()); 
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getCameraTextureRef) 

static bool js_cc_ar_ARModule_getCameraDepthBuffer(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    uint8_t *result = 0 ;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = (uint8_t *)((cc::ar::ARModule const *)arg1)->getCameraDepthBuffer();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    SE_HOLD_RETURN_VALUE(result, s.thisObject(), s.rval()); 
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getCameraDepthBuffer) 

static bool js_cc_ar_ARModule_getMainLightDirection(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    LightVal result;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = ((cc::ar::ARModule const *)arg1)->getMainLightDirection();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject() /*ctx*/);
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    SE_HOLD_RETURN_VALUE(result, s.thisObject(), s.rval());
    
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getMainLightDirection) 

static bool js_cc_ar_ARModule_getMainLightIntensity(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    LightVal result;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = ((cc::ar::ARModule const *)arg1)->getMainLightIntensity();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject() /*ctx*/);
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    SE_HOLD_RETURN_VALUE(result, s.thisObject(), s.rval());
    
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getMainLightIntensity) 

static bool js_cc_ar_ARModule_tryHitAttachAnchor(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    int arg2 ;
    int result;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    result = (int)((cc::ar::ARModule const *)arg1)->tryHitAttachAnchor(arg2);
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject()); 
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_tryHitAttachAnchor) 

static bool js_cc_ar_ARModule_getAnchorPose(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    int arg2 ;
    Pose result;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    result = ((cc::ar::ARModule const *)arg1)->getAnchorPose(arg2);
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject() /*ctx*/);
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    SE_HOLD_RETURN_VALUE(result, s.thisObject(), s.rval());
    
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getAnchorPose) 

static bool js_cc_ar_ARModule_tryHitTest(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    float arg2 ;
    float arg3 ;
    uint32_t arg4 ;
    bool result;
    
    if(argc != 3) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 3);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    
    ok &= sevalue_to_native(args[1], &arg3, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    
    ok &= sevalue_to_native(args[2], &arg4, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    
    result = (bool)((cc::ar::ARModule const *)arg1)->tryHitTest(arg2,arg3,arg4);
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject());
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_tryHitTest) 

static bool js_cc_ar_ARModule_getHitResult(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    Pose result;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = ((cc::ar::ARModule const *)arg1)->getHitResult();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject() /*ctx*/);
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    SE_HOLD_RETURN_VALUE(result, s.thisObject(), s.rval());
    
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getHitResult) 

static bool js_cc_ar_ARModule_getHitId(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    int result;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = (int)((cc::ar::ARModule const *)arg1)->getHitId();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject()); 
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getHitId) 

static bool js_cc_ar_ARModule_getHitType(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    int result;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    result = (int)((cc::ar::ARModule const *)arg1)->getHitType();
    
    ok &= nativevalue_to_se(result, s.rval(), s.thisObject()); 
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_getHitType) 

static bool js_cc_ar_ARModule_enablePlane(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    bool arg2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2);
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    ((cc::ar::ARModule const *)arg1)->enablePlane(arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_enablePlane) 

static bool js_cc_ar_ARModule_setPlaneDetectionMode(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    int arg2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    ((cc::ar::ARModule const *)arg1)->setPlaneDetectionMode(arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_setPlaneDetectionMode) 

static bool js_cc_ar_ARModule_setPlaneMaxTrackingNumber(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    int arg2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    ((cc::ar::ARModule const *)arg1)->setPlaneMaxTrackingNumber(arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_setPlaneMaxTrackingNumber) 

static bool js_cc_ar_ARModule_enableSceneMesh(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    bool arg2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2);
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    ((cc::ar::ARModule const *)arg1)->enableSceneMesh(arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_enableSceneMesh) 

static bool js_cc_ar_ARModule_endRequireSceneMesh(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    
    if(argc != 0) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 0);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    ((cc::ar::ARModule const *)arg1)->endRequireSceneMesh();
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_endRequireSceneMesh) 

static bool js_cc_ar_ARModule_enableImageTracking(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    bool arg2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2);
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    ((cc::ar::ARModule const *)arg1)->enableImageTracking(arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_enableImageTracking) 

static bool js_cc_ar_ARModule_addImageToLib(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    std::string *arg2 = 0 ;
    std::string temp2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &temp2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    arg2 = &temp2;
    
    ((cc::ar::ARModule const *)arg1)->addImageToLib((std::string const &)*arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_addImageToLib) 

static bool js_cc_ar_ARModule_addImageToLibWithSize(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    std::string *arg2 = 0 ;
    float arg3 ;
    std::string temp2 ;
    
    if(argc != 2) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 2);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &temp2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    arg2 = &temp2;
    
    
    ok &= sevalue_to_native(args[1], &arg3, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    ((cc::ar::ARModule const *)arg1)->addImageToLibWithSize((std::string const &)*arg2,arg3);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_addImageToLibWithSize) 

static bool js_cc_ar_ARModule_setImageMaxTrackingNumber(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    int arg2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    ((cc::ar::ARModule const *)arg1)->setImageMaxTrackingNumber(arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_setImageMaxTrackingNumber) 

static bool js_cc_ar_ARModule_enableObjectTracking(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    bool arg2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2);
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    ((cc::ar::ARModule const *)arg1)->enableObjectTracking(arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_enableObjectTracking) 

static bool js_cc_ar_ARModule_addObjectToLib(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    std::string *arg2 = 0 ;
    std::string temp2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &temp2, s.thisObject());
    SE_PRECONDITION2(ok, false, "Error processing arguments");
    arg2 = &temp2;
    
    ((cc::ar::ARModule const *)arg1)->addObjectToLib((std::string const &)*arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_addObjectToLib) 

static bool js_cc_ar_ARModule_enableFaceTracking(se::State& s)
{
    CC_UNUSED bool ok = true;
    const auto& args = s.args();
    size_t argc = args.size();
    cc::ar::ARModule *arg1 = (cc::ar::ARModule *) NULL ;
    bool arg2 ;
    
    if(argc != 1) {
        SE_REPORT_ERROR("wrong number of arguments: %d, was expecting %d", (int)argc, 1);
        return false;
    }
    arg1 = SE_THIS_OBJECT<cc::ar::ARModule>(s);
    if (nullptr == arg1) return true;
    
    ok &= sevalue_to_native(args[0], &arg2);
    SE_PRECONDITION2(ok, false, "Error processing arguments"); 
    ((cc::ar::ARModule const *)arg1)->enableFaceTracking(arg2);
    
    
    return true;
}
SE_BIND_FUNC(js_cc_ar_ARModule_enableFaceTracking) 

bool js_register_cc_ar_ARModule(se::Object* obj) {
    auto* cls = se::Class::create("ARModule", obj, nullptr, _SE(js_new_cc_ar_ARModule)); 
    
    
    cls->defineFunction("config", _SE(js_cc_ar_ARModule_config)); 
    cls->defineFunction("getSupportMask", _SE(js_cc_ar_ARModule_getSupportMask)); 
    cls->defineFunction("start", _SE(js_cc_ar_ARModule_start)); 
    cls->defineFunction("onResume", _SE(js_cc_ar_ARModule_onResume)); 
    cls->defineFunction("onPause", _SE(js_cc_ar_ARModule_onPause)); 
    cls->defineFunction("update", _SE(js_cc_ar_ARModule_update)); 
    cls->defineFunction("getAPIState", _SE(js_cc_ar_ARModule_getAPIState)); 
    cls->defineFunction("setCameraId", _SE(js_cc_ar_ARModule_setCameraId)); 
    cls->defineFunction("getCameraId", _SE(js_cc_ar_ARModule_getCameraId)); 
    cls->defineFunction("getCameraPose", _SE(js_cc_ar_ARModule_getCameraPose)); 
    cls->defineFunction("getCameraViewMatrix", _SE(js_cc_ar_ARModule_getCameraViewMatrix)); 
    cls->defineFunction("getCameraProjectionMatrix", _SE(js_cc_ar_ARModule_getCameraProjectionMatrix)); 
    cls->defineFunction("getCameraTexCoords", _SE(js_cc_ar_ARModule_getCameraTexCoords)); 
    cls->defineFunction("setDisplayGeometry", _SE(js_cc_ar_ARModule_setDisplayGeometry)); 
    cls->defineFunction("setCameraClip", _SE(js_cc_ar_ARModule_setCameraClip)); 
    cls->defineFunction("setCameraTextureName", _SE(js_cc_ar_ARModule_setCameraTextureName)); 
    cls->defineFunction("getCameraTextureRef", _SE(js_cc_ar_ARModule_getCameraTextureRef)); 
    cls->defineFunction("getCameraDepthBuffer", _SE(js_cc_ar_ARModule_getCameraDepthBuffer)); 
    cls->defineFunction("getMainLightDirection", _SE(js_cc_ar_ARModule_getMainLightDirection)); 
    cls->defineFunction("getMainLightIntensity", _SE(js_cc_ar_ARModule_getMainLightIntensity)); 
    cls->defineFunction("tryHitAttachAnchor", _SE(js_cc_ar_ARModule_tryHitAttachAnchor)); 
    cls->defineFunction("getAnchorPose", _SE(js_cc_ar_ARModule_getAnchorPose)); 
    cls->defineFunction("tryHitTest", _SE(js_cc_ar_ARModule_tryHitTest)); 
    cls->defineFunction("getHitResult", _SE(js_cc_ar_ARModule_getHitResult)); 
    cls->defineFunction("getHitId", _SE(js_cc_ar_ARModule_getHitId)); 
    cls->defineFunction("getHitType", _SE(js_cc_ar_ARModule_getHitType)); 
    cls->defineFunction("enablePlane", _SE(js_cc_ar_ARModule_enablePlane)); 
    cls->defineFunction("setPlaneDetectionMode", _SE(js_cc_ar_ARModule_setPlaneDetectionMode)); 
    cls->defineFunction("setPlaneMaxTrackingNumber", _SE(js_cc_ar_ARModule_setPlaneMaxTrackingNumber)); 
    cls->defineFunction("enableSceneMesh", _SE(js_cc_ar_ARModule_enableSceneMesh)); 
    cls->defineFunction("endRequireSceneMesh", _SE(js_cc_ar_ARModule_endRequireSceneMesh)); 
    cls->defineFunction("enableImageTracking", _SE(js_cc_ar_ARModule_enableImageTracking)); 
    cls->defineFunction("addImageToLib", _SE(js_cc_ar_ARModule_addImageToLib)); 
    cls->defineFunction("addImageToLibWithSize", _SE(js_cc_ar_ARModule_addImageToLibWithSize)); 
    cls->defineFunction("setImageMaxTrackingNumber", _SE(js_cc_ar_ARModule_setImageMaxTrackingNumber)); 
    cls->defineFunction("enableObjectTracking", _SE(js_cc_ar_ARModule_enableObjectTracking)); 
    cls->defineFunction("addObjectToLib", _SE(js_cc_ar_ARModule_addObjectToLib)); 
    cls->defineFunction("enableFaceTracking", _SE(js_cc_ar_ARModule_enableFaceTracking)); 
    
    
    cls->defineStaticFunction("get", _SE(js_cc_ar_ARModule_get_static)); 
    
    
    cls->defineFinalizeFunction(_SE(js_delete_cc_ar_ARModule));
    
    
    cls->install();
    JSBClassType::registerClass<cc::ar::ARModule>(cls);
    
    __jsb_cc_ar_ARModule_proto = cls->getProto();
    __jsb_cc_ar_ARModule_class = cls;
    se::ScriptEngine::getInstance()->clearException();
    return true;
}




bool register_all_ar(se::Object* obj) {
    // Get the ns
    se::Value nsVal;
    if (!obj->getProperty("xr", &nsVal, true))
    {
        se::HandleObject jsobj(se::Object::createPlainObject());
        nsVal.setObject(jsobj);
        obj->setProperty("xr", nsVal);
    }
    se::Object* ns = nsVal.toObject();
    /* Register classes */
    js_register_cc_ar_ARModule(ns); 
    
    /* Register global variables & global functions */
    
    
    
    return true;
}


#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__) || defined(__GNUG__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
// clang-format on
