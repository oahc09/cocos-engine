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

#include "core/components/LightComponent.h"

#include "core/Root.h"
#include "core/scene-graph/Node.h"
#include "core/scene-graph/Scene.h"
#include "scene/DirectionalLight.h"
#include "scene/Light.h"
#include "scene/PointLight.h"
#include "scene/RangedDirectionalLight.h"
#include "scene/RenderScene.h"
#include "scene/SphereLight.h"
#include "scene/SpotLight.h"
#include "renderer/pipeline/Define.h"

namespace cc {

// ============================================================
// LightComponent (base)
// ============================================================
LightComponent::~LightComponent() = default;

void LightComponent::setColor(const Vec3 &color) {
    if (_renderLight) _renderLight->setColor(color);
}

Vec3 LightComponent::getColor() const {
    return _renderLight ? _renderLight->getColor() : Vec3(1.0F, 1.0F, 1.0F);
}

void LightComponent::setUseColorTemperature(bool use) {
    if (_renderLight) _renderLight->setUseColorTemperature(use);
}

bool LightComponent::isUseColorTemperature() const {
    return _renderLight ? _renderLight->isUseColorTemperature() : false;
}

void LightComponent::setColorTemperature(float temp) {
    if (_renderLight) _renderLight->setColorTemperature(temp);
}

float LightComponent::getColorTemperature() const {
    return _renderLight ? _renderLight->getColorTemperature() : 6550.0F;
}

void LightComponent::setVisibility(uint32_t visibility) {
    if (_renderLight) _renderLight->setVisibility(visibility);
}

uint32_t LightComponent::getVisibility() const {
    return _renderLight ? _renderLight->getVisibility() : 0;
}

// ============================================================
// DirectionalLightComponent
// ============================================================
DirectionalLightComponent::~DirectionalLightComponent() = default;

void DirectionalLightComponent::onLoad() {
    if (!_node) return;
    Scene *scene = _node->getScene();
    if (!scene) return;
    scene::RenderScene *renderScene = scene->getRenderScene();
    if (!renderScene) return;

    auto *light = ccnew scene::DirectionalLight();
    light->initialize();
    light->setNode(_node);
    _renderLight.reset(light);

    // Apply stored shadow state
    light->setShadowEnabled(_shadowEnabled);
    light->setShadowBias(_shadowBias);
    light->setShadowNormalBias(_shadowNormalBias);
    light->setShadowDistance(_shadowDistance);

    renderScene->addDirectionalLight(light);
    // First directional light becomes main light
    if (!renderScene->getMainLight()) {
        renderScene->setMainLight(light);
    }
}

void DirectionalLightComponent::onEnable() {
    if (_renderLight) _renderLight->setVisibility(pipeline::CAMERA_DEFAULT_MASK);
}

void DirectionalLightComponent::onDisable() {
    if (_renderLight) _renderLight->setVisibility(0);
}

void DirectionalLightComponent::onDestroy() {
    if (_renderLight) {
        auto *dl = static_cast<scene::DirectionalLight *>(_renderLight.get());
        Scene *scene = _node ? _node->getScene() : nullptr;
        if (scene) {
            scene::RenderScene *rs = scene->getRenderScene();
            if (rs) {
                rs->removeDirectionalLight(dl);
            }
        }
        _renderLight->destroy();
        _renderLight.reset();
    }
}

void DirectionalLightComponent::update(float /*dt*/) {
    if (_renderLight) _renderLight->update();
}

void DirectionalLightComponent::setIlluminance(float value) {
    auto *dl = static_cast<scene::DirectionalLight *>(_renderLight.get());
    if (dl) dl->setIlluminance(value);
}

float DirectionalLightComponent::getIlluminance() const {
    auto *dl = static_cast<scene::DirectionalLight *>(_renderLight.get());
    return dl ? dl->getIlluminance() : 0.0F;
}

void DirectionalLightComponent::setShadowEnabled(bool enabled) {
    _shadowEnabled = enabled;
    auto *dl = static_cast<scene::DirectionalLight *>(_renderLight.get());
    if (dl) dl->setShadowEnabled(enabled);
}

bool DirectionalLightComponent::isShadowEnabled() const {
    return _shadowEnabled;
}

void DirectionalLightComponent::setShadowBias(float bias) {
    _shadowBias = bias;
    auto *dl = static_cast<scene::DirectionalLight *>(_renderLight.get());
    if (dl) dl->setShadowBias(bias);
}

float DirectionalLightComponent::getShadowBias() const {
    return _shadowBias;
}

void DirectionalLightComponent::setShadowNormalBias(float bias) {
    _shadowNormalBias = bias;
    auto *dl = static_cast<scene::DirectionalLight *>(_renderLight.get());
    if (dl) dl->setShadowNormalBias(bias);
}

float DirectionalLightComponent::getShadowNormalBias() const {
    return _shadowNormalBias;
}

void DirectionalLightComponent::setShadowDistance(float distance) {
    _shadowDistance = distance;
    auto *dl = static_cast<scene::DirectionalLight *>(_renderLight.get());
    if (dl) dl->setShadowDistance(distance);
}

float DirectionalLightComponent::getShadowDistance() const {
    return _shadowDistance;
}

scene::Light *DirectionalLightComponent::getRenderLight() const {
    return _renderLight.get();
}

ccstd::vector<Asset *> DirectionalLightComponent::getAssetProperties() {
    return {}; // Lights don't hold asset references
}

// ============================================================
// SphereLightComponent
// ============================================================
SphereLightComponent::~SphereLightComponent() = default;

void SphereLightComponent::onLoad() {
    if (!_node) return;
    Scene *scene = _node->getScene();
    if (!scene) return;
    scene::RenderScene *renderScene = scene->getRenderScene();
    if (!renderScene) return;

    auto *light = ccnew scene::SphereLight();
    light->initialize();
    light->setNode(_node);
    _renderLight.reset(light);

    renderScene->addSphereLight(light);
}

void SphereLightComponent::onEnable() {
    if (_renderLight) _renderLight->setVisibility(pipeline::CAMERA_DEFAULT_MASK);
}

void SphereLightComponent::onDisable() {
    if (_renderLight) _renderLight->setVisibility(0);
}

void SphereLightComponent::onDestroy() {
    if (_renderLight) {
        auto *sl = static_cast<scene::SphereLight *>(_renderLight.get());
        Scene *scene = _node ? _node->getScene() : nullptr;
        if (scene) {
            scene::RenderScene *rs = scene->getRenderScene();
            if (rs) rs->removeSphereLight(sl);
        }
        _renderLight->destroy();
        _renderLight.reset();
    }
}

void SphereLightComponent::update(float /*dt*/) {
    if (_renderLight) _renderLight->update();
}

void SphereLightComponent::setRange(float range) {
    auto *sl = static_cast<scene::SphereLight *>(_renderLight.get());
    if (sl) sl->setRange(range);
}

float SphereLightComponent::getRange() const {
    auto *sl = static_cast<scene::SphereLight *>(_renderLight.get());
    return sl ? sl->getRange() : 0.0F;
}

void SphereLightComponent::setSize(float size) {
    auto *sl = static_cast<scene::SphereLight *>(_renderLight.get());
    if (sl) sl->setSize(size);
}

float SphereLightComponent::getSize() const {
    auto *sl = static_cast<scene::SphereLight *>(_renderLight.get());
    return sl ? sl->getSize() : 0.0F;
}

void SphereLightComponent::setLuminance(float luminance) {
    auto *sl = static_cast<scene::SphereLight *>(_renderLight.get());
    if (sl) sl->setLuminance(luminance);
}

float SphereLightComponent::getLuminance() const {
    auto *sl = static_cast<scene::SphereLight *>(_renderLight.get());
    return sl ? sl->getLuminance() : 0.0F;
}

scene::Light *SphereLightComponent::getRenderLight() const {
    return _renderLight.get();
}

ccstd::vector<Asset *> SphereLightComponent::getAssetProperties() {
    return {};
}

// ============================================================
// SpotLightComponent
// ============================================================
SpotLightComponent::~SpotLightComponent() = default;

void SpotLightComponent::onLoad() {
    if (!_node) return;
    Scene *scene = _node->getScene();
    if (!scene) return;
    scene::RenderScene *renderScene = scene->getRenderScene();
    if (!renderScene) return;

    auto *light = ccnew scene::SpotLight();
    light->initialize();
    light->setNode(_node);
    _renderLight.reset(light);

    renderScene->addSpotLight(light);
}

void SpotLightComponent::onEnable() {
    if (_renderLight) _renderLight->setVisibility(pipeline::CAMERA_DEFAULT_MASK);
}

void SpotLightComponent::onDisable() {
    if (_renderLight) _renderLight->setVisibility(0);
}

void SpotLightComponent::onDestroy() {
    if (_renderLight) {
        auto *sl = static_cast<scene::SpotLight *>(_renderLight.get());
        Scene *scene = _node ? _node->getScene() : nullptr;
        if (scene) {
            scene::RenderScene *rs = scene->getRenderScene();
            if (rs) rs->removeSpotLight(sl);
        }
        _renderLight->destroy();
        _renderLight.reset();
    }
}

void SpotLightComponent::update(float /*dt*/) {
    if (_renderLight) _renderLight->update();
}

void SpotLightComponent::setRange(float range) {
    auto *sl = static_cast<scene::SpotLight *>(_renderLight.get());
    if (sl) sl->setRange(range);
}

float SpotLightComponent::getRange() const {
    auto *sl = static_cast<scene::SpotLight *>(_renderLight.get());
    return sl ? sl->getRange() : 0.0F;
}

void SpotLightComponent::setSize(float size) {
    auto *sl = static_cast<scene::SpotLight *>(_renderLight.get());
    if (sl) sl->setSize(size);
}

float SpotLightComponent::getSize() const {
    auto *sl = static_cast<scene::SpotLight *>(_renderLight.get());
    return sl ? sl->getSize() : 0.0F;
}

void SpotLightComponent::setSpotAngle(float angle) {
    auto *sl = static_cast<scene::SpotLight *>(_renderLight.get());
    if (sl) sl->setSpotAngle(angle);
}

float SpotLightComponent::getSpotAngle() const {
    auto *sl = static_cast<scene::SpotLight *>(_renderLight.get());
    return sl ? sl->getSpotAngle() : 0.0F;
}

void SpotLightComponent::setLuminance(float luminance) {
    auto *sl = static_cast<scene::SpotLight *>(_renderLight.get());
    if (sl) sl->setLuminance(luminance);
}

float SpotLightComponent::getLuminance() const {
    auto *sl = static_cast<scene::SpotLight *>(_renderLight.get());
    return sl ? sl->getLuminance() : 0.0F;
}

void SpotLightComponent::setShadowEnabled(bool enabled) {
    auto *sl = static_cast<scene::SpotLight *>(_renderLight.get());
    if (sl) sl->setShadowEnabled(enabled);
}

bool SpotLightComponent::isShadowEnabled() const {
    auto *sl = static_cast<scene::SpotLight *>(_renderLight.get());
    return sl ? sl->isShadowEnabled() : false;
}

scene::Light *SpotLightComponent::getRenderLight() const {
    return _renderLight.get();
}

ccstd::vector<Asset *> SpotLightComponent::getAssetProperties() {
    return {};
}

// ============================================================
// PointLightComponent
// ============================================================
PointLightComponent::~PointLightComponent() = default;

void PointLightComponent::onLoad() {
    if (!_node) return;
    Scene *scene = _node->getScene();
    if (!scene) return;
    scene::RenderScene *renderScene = scene->getRenderScene();
    if (!renderScene) return;

    auto *light = ccnew scene::PointLight();
    light->initialize();
    light->setNode(_node);
    _renderLight.reset(light);

    renderScene->addPointLight(light);
}

void PointLightComponent::onEnable() {
    if (_renderLight) _renderLight->setVisibility(pipeline::CAMERA_DEFAULT_MASK);
}

void PointLightComponent::onDisable() {
    if (_renderLight) _renderLight->setVisibility(0);
}

void PointLightComponent::onDestroy() {
    if (_renderLight) {
        auto *pl = static_cast<scene::PointLight *>(_renderLight.get());
        Scene *scene = _node ? _node->getScene() : nullptr;
        if (scene) {
            scene::RenderScene *rs = scene->getRenderScene();
            if (rs) rs->removePointLight(pl);
        }
        _renderLight->destroy();
        _renderLight.reset();
    }
}

void PointLightComponent::update(float /*dt*/) {
    if (_renderLight) _renderLight->update();
}

void PointLightComponent::setRange(float range) {
    auto *pl = static_cast<scene::PointLight *>(_renderLight.get());
    if (pl) pl->setRange(range);
}

float PointLightComponent::getRange() const {
    auto *pl = static_cast<scene::PointLight *>(_renderLight.get());
    return pl ? pl->getRange() : 0.0F;
}

void PointLightComponent::setLuminance(float luminance) {
    auto *pl = static_cast<scene::PointLight *>(_renderLight.get());
    if (pl) pl->setLuminance(luminance);
}

float PointLightComponent::getLuminance() const {
    auto *pl = static_cast<scene::PointLight *>(_renderLight.get());
    return pl ? pl->getLuminance() : 0.0F;
}

scene::Light *PointLightComponent::getRenderLight() const {
    return _renderLight.get();
}

ccstd::vector<Asset *> PointLightComponent::getAssetProperties() {
    return {};
}

// ============================================================
// RangedDirectionalLightComponent
// ============================================================
RangedDirectionalLightComponent::~RangedDirectionalLightComponent() = default;

void RangedDirectionalLightComponent::onLoad() {
    if (!_node) return;
    Scene *scene = _node->getScene();
    if (!scene) return;
    scene::RenderScene *renderScene = scene->getRenderScene();
    if (!renderScene) return;

    auto *light = ccnew scene::RangedDirectionalLight();
    light->initialize();
    light->setNode(_node);
    _renderLight.reset(light);

    renderScene->addRangedDirLight(light);
}

void RangedDirectionalLightComponent::onEnable() {
    if (_renderLight) _renderLight->setVisibility(pipeline::CAMERA_DEFAULT_MASK);
}

void RangedDirectionalLightComponent::onDisable() {
    if (_renderLight) _renderLight->setVisibility(0);
}

void RangedDirectionalLightComponent::onDestroy() {
    if (_renderLight) {
        auto *rl = static_cast<scene::RangedDirectionalLight *>(_renderLight.get());
        Scene *scene = _node ? _node->getScene() : nullptr;
        if (scene) {
            scene::RenderScene *rs = scene->getRenderScene();
            if (rs) rs->removeRangedDirLight(rl);
        }
        _renderLight->destroy();
        _renderLight.reset();
    }
}

void RangedDirectionalLightComponent::update(float /*dt*/) {
    if (_renderLight) _renderLight->update();
}

void RangedDirectionalLightComponent::setIlluminance(float value) {
    auto *rl = static_cast<scene::RangedDirectionalLight *>(_renderLight.get());
    if (rl) rl->setIlluminance(value);
}

float RangedDirectionalLightComponent::getIlluminance() const {
    auto *rl = static_cast<scene::RangedDirectionalLight *>(_renderLight.get());
    return rl ? rl->getIlluminance() : 0.0F;
}

scene::Light *RangedDirectionalLightComponent::getRenderLight() const {
    return _renderLight.get();
}

ccstd::vector<Asset *> RangedDirectionalLightComponent::getAssetProperties() {
    return {};
}

} // namespace cc
