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

#include "core/components/MeshRendererComponent.h"

#include "3d/assets/Mesh.h"
#include "core/Root.h"
#include "core/assets/Material.h"
#include "core/scene-graph/Node.h"
#include "core/scene-graph/Scene.h"
#include "scene/Model.h"
#include "scene/RenderScene.h"

namespace cc {

MeshRendererComponent::~MeshRendererComponent() = default;

void MeshRendererComponent::onLoad() {
    if (!_node) {
        return;
    }

    Scene *scene = _node->getScene();
    if (!scene) {
        return;
    }

    scene::RenderScene *renderScene = scene->getRenderScene();
    if (!renderScene) {
        return;
    }

    // Create the render-layer Model
    _renderModel = ccnew scene::Model();
    _renderModel->setNode(_node);
    _renderModel->setTransform(_node);

    // Apply shadow state that may have been set before onLoad
    _renderModel->setCastShadow(_shadowCastingMode != 0);
    _renderModel->setReceiveShadow(_receiveShadow);

    // If mesh was set before onLoad, apply it
    syncToRenderModel();

    // Attach to RenderScene
    renderScene->addModel(_renderModel.get());
}

void MeshRendererComponent::onEnable() {
    if (_renderModel) {
        _renderModel->setEnabled(true);
    }
}

void MeshRendererComponent::onDisable() {
    if (_renderModel) {
        _renderModel->setEnabled(false);
    }
}

void MeshRendererComponent::onDestroy() {
    if (_renderModel) {
        // Detach from RenderScene before destroying
        Scene *scene = _node ? _node->getScene() : nullptr;
        if (scene) {
            scene::RenderScene *renderScene = scene->getRenderScene();
            if (renderScene) {
                renderScene->removeModel(_renderModel.get());
            }
        }
        _renderModel->destroy();
        _renderModel.reset();
    }
    _mesh.reset();
    _materials.clear();
}

void MeshRendererComponent::update(float /*dt*/) {
    if (_renderModel) {
        _renderModel->updateTransform(0); // Update from node world matrix
    }
}

void MeshRendererComponent::setMesh(Mesh *mesh) {
    _mesh = mesh; // IntrusivePtr handles ref count
    syncToRenderModel();
}

void MeshRendererComponent::setMaterial(Material *material, uint32_t subModelIndex) {
    if (subModelIndex >= _materials.size()) {
        _materials.resize(subModelIndex + 1);
    }
    _materials[subModelIndex] = material;
    syncToRenderModel();
}

Material *MeshRendererComponent::getMaterial(uint32_t subModelIndex) const {
    if (subModelIndex < _materials.size()) {
        return _materials[subModelIndex].get();
    }
    return nullptr;
}

void MeshRendererComponent::setShadowCastingMode(int32_t mode) {
    _shadowCastingMode = mode;
    if (_renderModel) {
        _renderModel->setCastShadow(mode != 0);
    }
}

void MeshRendererComponent::setReceiveShadow(bool receive) {
    _receiveShadow = receive;
    if (_renderModel) {
        _renderModel->setReceiveShadow(receive);
    }
}

void MeshRendererComponent::syncToRenderModel() {
    if (!_renderModel || !_mesh) {
        return;
    }

    // Set the mesh on the model (creating submodels).
    // NOTE: This is a simplified Phase E2 implementation. A full version would:
    //   1. Get RenderingSubMeshes from the Mesh asset
    //   2. Call _renderModel->initSubModel(i, subMesh, material) for each sub-mesh
    //   3. Handle bounds creation via createBoundingShape()
    // For now we only establish the basic reference.
    // Detailed submodel setup will be implemented in a follow-up phase.
}

void MeshRendererComponent::deserializeBinary(const uint8_t * /*data*/, uint32_t /*size*/) {
    // Stub: mesh/material references are resolved by UUID in the deserialization
    // pipeline. Binary payload layout will be defined in a later phase.
}

ccstd::vector<Asset *> MeshRendererComponent::getAssetProperties() {
    ccstd::vector<Asset *> refs;
    if (_mesh) {
        refs.push_back(_mesh.get());
    }
    for (auto &mat : _materials) {
        if (mat) {
            refs.push_back(mat.get());
        }
    }
    return refs;
}

} // namespace cc
