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

#include "core/components/CameraComponent.h"
#include "core/scene-graph/Node.h"
#include "core/scene-graph/Scene.h"
#include "core/Root.h"
#include "scene/Camera.h"
#include "scene/RenderScene.h"
#include "renderer/gfx-base/GFXDevice.h"
#include "math/Geometry.h"

namespace cc {

CameraComponent::~CameraComponent() = default;

void CameraComponent::onLoad() {
    if (!_node) {
        return;
    }

    // Get the Scene this node belongs to
    Scene *scene = _node->getScene();
    if (!scene) {
        return;
    }

    scene::RenderScene *renderScene = scene->getRenderScene();
    if (!renderScene) {
        return;
    }

    // Create the render-layer Camera
    gfx::Device *device = Root::getInstance()->getDevice();
    if (!device) {
        return;
    }

    _renderCamera = ccnew scene::Camera(device);

    scene::ICameraInfo info;
    info.name = "CameraComponent";
    info.node = _node;
    info.projection = (_projection == 0)
                          ? scene::CameraProjection::ORTHO
                          : scene::CameraProjection::PERSPECTIVE;
    info.priority = 0;
    info.usage = scene::CameraUsage::GAME;

    _renderCamera->initialize(info);
    _renderCamera->setNearClip(_near);
    _renderCamera->setFarClip(_far);
    _renderCamera->setFov(mathutils::toRadian(_fov));

    // Set viewport
    _renderCamera->setViewport(Rect(
        _viewportX, _viewportY,
        _viewportWidth, _viewportHeight));

    // Attach to RenderScene
    renderScene->addCamera(_renderCamera.get());
}

void CameraComponent::onEnable() {
    if (_renderCamera) {
        _renderCamera->setEnabled(true);
    }
}

void CameraComponent::onDisable() {
    if (_renderCamera) {
        _renderCamera->setEnabled(false);
    }
}

void CameraComponent::onDestroy() {
    if (_renderCamera) {
        _renderCamera->destroy();
        _renderCamera.reset();
    }
}

void CameraComponent::update(float /*dt*/) {
    if (_renderCamera) {
        // scene::Camera::update() reads the node's world matrix and
        // recomputes view/projection when dirty.  We call it every frame
        // so the C++ camera stays in sync with the C++ node transform.
        _renderCamera->update();
    }
}

void CameraComponent::setProjection(int32_t projection) {
    _projection = projection;
    if (_renderCamera) {
        _renderCamera->setProjectionType(
            (projection == 0) ? scene::CameraProjection::ORTHO
                              : scene::CameraProjection::PERSPECTIVE);
    }
}

void CameraComponent::setViewport(float x, float y, float width, float height) {
    _viewportX = x;
    _viewportY = y;
    _viewportWidth = width;
    _viewportHeight = height;
    syncToRenderCamera();
}

void CameraComponent::syncToRenderCamera() {
    if (!_renderCamera) {
        return;
    }
    _renderCamera->setNearClip(_near);
    _renderCamera->setFarClip(_far);
    _renderCamera->setFov(mathutils::toRadian(_fov));
    _renderCamera->setViewport(Rect(
        _viewportX, _viewportY,
        _viewportWidth, _viewportHeight));
}

void CameraComponent::deserializeBinary(const uint8_t *data, uint32_t size) {
    // Stub: read the first 4 floats as near, far, fov, projection
    if (size >= sizeof(float) * 4) {
        const float *f = reinterpret_cast<const float *>(data);
        _near = f[0];
        _far = f[1];
        _fov = f[2];
        _projection = static_cast<int32_t>(f[3]);
    }
}

ccstd::vector<Asset *> CameraComponent::getAssetProperties() {
    // Camera component does not hold any Asset references directly.
    return {};
}

} // namespace cc
