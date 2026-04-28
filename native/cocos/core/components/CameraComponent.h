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

#include "core/components/Component.h"
#include "base/Ptr.h"

namespace cc {

namespace scene {
class Camera;
class RenderScene;
}

/**
 * @en Camera component for C++ fast-path.
 * @zh C++ 快速路径的相机组件。
 */
class CameraComponent : public Component {
public:
    CameraComponent() = default;
    ~CameraComponent() override;

    // === Component identity ===
    bool isBuiltin() const override { return true; }
    uint32_t getComponentTypeId() const override { return BUILTIN_CAMERA; }
    bool hasUpdateMethod() const override { return true; }

    // === Lifecycle ===
    void onLoad() override;
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;
    void update(float dt) override;

    // === Camera properties (mirror TS @property) ===
    void setProjection(int32_t projection);  // 0=ORTHO, 1=PERSPECTIVE
    int32_t getProjection() const { return _projection; }

    void setNear(float nearVal) { _near = nearVal; syncToRenderCamera(); }
    float getNear() const { return _near; }

    void setFar(float farVal) { _far = farVal; syncToRenderCamera(); }
    float getFar() const { return _far; }

    void setFov(float fov) { _fov = fov; syncToRenderCamera(); }
    float getFov() const { return _fov; }

    void setViewport(float x, float y, float width, float height);

    // === RenderScene access ===
    scene::Camera* getRenderCamera() const { return _renderCamera.get(); }

    // === Serialization ===
    void deserializeBinary(const uint8_t* data, uint32_t size) override;
    ccstd::vector<Asset*> getAssetProperties() override;

private:
    void syncToRenderCamera();

    int32_t _projection{1};  // PERSPECTIVE default
    float _near{1.0f};
    float _far{1000.0f};
    float _fov{45.0f};
    float _viewportX{0.0f};
    float _viewportY{0.0f};
    float _viewportWidth{1.0f};
    float _viewportHeight{1.0f};

    IntrusivePtr<scene::Camera> _renderCamera;
};

} // namespace cc
