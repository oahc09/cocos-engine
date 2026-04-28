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

#include "base/Ptr.h"
#include "core/components/BuiltinTypeIds.h"
#include "core/components/Component.h"
#include "core/components/ComponentMacros.h"
#include "math/Vec3.h"

namespace cc {

namespace scene {
class Light;
class DirectionalLight;
class SphereLight;
class SpotLight;
class PointLight;
class RangedDirectionalLight;
class RenderScene;
} // namespace scene

/**
 * @en Base light component for C++ fast-path.
 *     Common properties: color, useColorTemperature, colorTemperature, visibility.
 * @zh C++ 快速路径灯光组件基类。
 *     公共属性：颜色、色温、可见性。
 */
class LightComponent : public Component {
    CC_COMPONENT_DECLARE(LightComponent, BUILTIN_LIGHT)

public:
    LightComponent() = default;
    ~LightComponent() override;

    // Color
    void setColor(const Vec3 &color);
    Vec3 getColor() const;

    void setUseColorTemperature(bool use);
    bool isUseColorTemperature() const;

    void setColorTemperature(float temp);
    float getColorTemperature() const;

    // Visibility
    void setVisibility(uint32_t visibility);
    uint32_t getVisibility() const;

    // Access render light
    virtual scene::Light *getRenderLight() const { return _renderLight.get(); }

protected:
    IntrusivePtr<scene::Light> _renderLight;
};

// ============================================================
// DirectionalLightComponent
// ============================================================
class DirectionalLightComponent final : public LightComponent {
    CC_COMPONENT_DECLARE(DirectionalLightComponent, BUILTIN_DIRECTIONAL_LIGHT)

public:
    DirectionalLightComponent() = default;
    ~DirectionalLightComponent() override;

    void onLoad() override;
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;
    void update(float dt) override;
    bool hasUpdateMethod() const override { return true; }

    // DirectionalLight-specific
    void setIlluminance(float value);
    float getIlluminance() const;

    // Shadow
    void setShadowEnabled(bool enabled);
    bool isShadowEnabled() const;
    void setShadowBias(float bias);
    float getShadowBias() const;
    void setShadowNormalBias(float bias);
    float getShadowNormalBias() const;
    void setShadowDistance(float distance);
    float getShadowDistance() const;

    // Override to return specific type
    scene::Light *getRenderLight() const override;

    // Serialization
    ccstd::vector<Asset *> getAssetProperties() override;

private:
    bool _shadowEnabled{false};
    float _shadowBias{0.0f};
    float _shadowNormalBias{0.0f};
    float _shadowDistance{50.0f};
};

// ============================================================
// SphereLightComponent
// ============================================================
class SphereLightComponent final : public LightComponent {
    CC_COMPONENT_DECLARE(SphereLightComponent, BUILTIN_SPHERE_LIGHT)

public:
    SphereLightComponent() = default;
    ~SphereLightComponent() override;

    void onLoad() override;
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;
    void update(float dt) override;
    bool hasUpdateMethod() const override { return true; }

    void setRange(float range);
    float getRange() const;
    void setSize(float size);
    float getSize() const;
    void setLuminance(float luminance);
    float getLuminance() const;

    scene::Light *getRenderLight() const override;
    ccstd::vector<Asset *> getAssetProperties() override;
};

// ============================================================
// SpotLightComponent
// ============================================================
class SpotLightComponent final : public LightComponent {
    CC_COMPONENT_DECLARE(SpotLightComponent, BUILTIN_SPOT_LIGHT)

public:
    SpotLightComponent() = default;
    ~SpotLightComponent() override;

    void onLoad() override;
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;
    void update(float dt) override;
    bool hasUpdateMethod() const override { return true; }

    void setRange(float range);
    float getRange() const;
    void setSize(float size);
    float getSize() const;
    void setSpotAngle(float angle);
    float getSpotAngle() const;
    void setLuminance(float luminance);
    float getLuminance() const;

    // Shadow
    void setShadowEnabled(bool enabled);
    bool isShadowEnabled() const;

    scene::Light *getRenderLight() const override;
    ccstd::vector<Asset *> getAssetProperties() override;
};

// ============================================================
// PointLightComponent
// ============================================================
class PointLightComponent final : public LightComponent {
    CC_COMPONENT_DECLARE(PointLightComponent, BUILTIN_POINT_LIGHT)

public:
    PointLightComponent() = default;
    ~PointLightComponent() override;

    void onLoad() override;
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;
    void update(float dt) override;
    bool hasUpdateMethod() const override { return true; }

    void setRange(float range);
    float getRange() const;
    void setLuminance(float luminance);
    float getLuminance() const;

    scene::Light *getRenderLight() const override;
    ccstd::vector<Asset *> getAssetProperties() override;
};

// ============================================================
// RangedDirectionalLightComponent
// ============================================================
class RangedDirectionalLightComponent final : public LightComponent {
    CC_COMPONENT_DECLARE(RangedDirectionalLightComponent, BUILTIN_RANGED_DIRECTIONAL_LIGHT)

public:
    RangedDirectionalLightComponent() = default;
    ~RangedDirectionalLightComponent() override;

    void onLoad() override;
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;
    void update(float dt) override;
    bool hasUpdateMethod() const override { return true; }

    void setIlluminance(float value);
    float getIlluminance() const;

    scene::Light *getRenderLight() const override;
    ccstd::vector<Asset *> getAssetProperties() override;
};

} // namespace cc
