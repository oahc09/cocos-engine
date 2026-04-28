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

namespace cc {

class Mesh;
class Material;

namespace scene {
class Model;
class RenderScene;
} // namespace scene

/**
 * @en Mesh renderer component for C++ fast-path.
 *     Wraps scene::Model for 3D mesh rendering.
 * @zh C++ 快速路径的网格渲染器组件。封装 scene::Model 用于 3D 网格渲染。
 */
class MeshRendererComponent : public Component {
    CC_COMPONENT_DECLARE(MeshRendererComponent, BUILTIN_MESH_RENDERER)

public:
    MeshRendererComponent() = default;
    ~MeshRendererComponent() override;

    // === Lifecycle ===
    void onLoad() override;
    void onEnable() override;
    void onDisable() override;
    void onDestroy() override;
    void update(float dt) override;

    // === Update detection ===
    bool hasUpdateMethod() const override { return true; }

    // === Mesh/Material access ===
    void setMesh(Mesh *mesh);
    Mesh *getMesh() const { return _mesh.get(); }

    void setMaterial(Material *material, uint32_t subModelIndex = 0);
    Material *getMaterial(uint32_t subModelIndex = 0) const;

    // === RenderScene access ===
    scene::Model *getRenderModel() const { return _renderModel.get(); }

    // === Shadow ===
    void setShadowCastingMode(int32_t mode);
    int32_t getShadowCastingMode() const { return _shadowCastingMode; }

    void setReceiveShadow(bool receive);
    bool isReceiveShadow() const { return _receiveShadow; }

    // === Serialization ===
    void deserializeBinary(const uint8_t *data, uint32_t size) override;
    ccstd::vector<Asset *> getAssetProperties() override;

private:
    void syncToRenderModel();

    IntrusivePtr<Mesh> _mesh;
    ccstd::vector<IntrusivePtr<Material>> _materials;

    IntrusivePtr<scene::Model> _renderModel;

    int32_t _shadowCastingMode{0}; // 0=OFF, 1=ON
    bool _receiveShadow{true};
};

} // namespace cc
