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

#include <cstdint>

namespace cc {

enum BuiltinTypeIds : uint32_t {
    BUILTIN_UNKNOWN = 0,
    BUILTIN_CAMERA = 1,
    BUILTIN_SPRITE = 2,
    BUILTIN_LABEL = 3,
    BUILTIN_MESH_RENDERER = 4,
    BUILTIN_SKINNED_MESH_RENDERER = 5,
    BUILTIN_RIGID_BODY = 6,
    BUILTIN_RIGID_BODY_3D = 7,
    BUILTIN_COLLIDER = 8,
    BUILTIN_AUDIO_SOURCE = 9,
    BUILTIN_PARTICLE_SYSTEM = 10,
    BUILTIN_ANIMATION = 11,
    BUILTIN_UI_TRANSFORM = 12,
    BUILTIN_LAYOUT = 13,
    BUILTIN_BUTTON = 14,
    BUILTIN_LIGHT = 15,
    BUILTIN_TRANSFORM = 16,
    BUILTIN_WIDGET = 17,
    BUILTIN_CANVAS = 18,
    BUILTIN_COUNT,
    // 用户脚本 TypeId 从 10000 开始
};

} // namespace cc
