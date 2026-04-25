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

#include "core/serialization/BinaryDeserializer.h"
#include "core/platform/Debug.h"
#include "core/scene-graph/Scene.h"
#include "core/scene-graph/Node.h"
#include "core/serialization/TypeRegistry.h"

#include <cstring>

namespace cc {

// ===========================================================================
// Public API
// ===========================================================================

BinaryDeserializeResult BinaryDeserializer::deserialize(const uint8_t *data, uint32_t size) {
    BinaryDeserializeResult result;

    if (!data || size < sizeof(BinarySceneHeader)) {
        result.errorMessage = "BinaryDeserializer: data is null or too small";
        return result;
    }

    // 1. Read and validate header
    BinarySceneHeader header;
    memcpy(&header, data, sizeof(BinarySceneHeader));

    if (!validateHeader(header)) {
        result.errorMessage = "BinaryDeserializer: invalid header (bad magic or unsupported version)";
        return result;
    }

    if (header.totalSize > size) {
        result.errorMessage = "BinaryDeserializer: file size mismatch";
        return result;
    }

    // 2. Parse string table
    // String table format: [uint32_t count][StringTableEntry[count]][string data...]
    const uint8_t *stringTablePtr = data + header.stringTableOffset;
    uint32_t stringCount = 0;
    memcpy(&stringCount, stringTablePtr, sizeof(uint32_t));

    auto strings = parseStringTable(data, header.stringTableOffset, stringCount);

    // 3. Create scene
    auto *scene = createScene(data, size, header, strings);
    if (!scene) {
        result.errorMessage = "BinaryDeserializer: failed to create scene";
        return result;
    }

    result.scene.reset(scene);

    // 4. Create node tree
    ccstd::vector<CCObject *> instances;
    auto *rootNode = createNodeTree(data, header.rootNodeOffset, header, strings, instances);
    if (rootNode) {
        scene->addChild(rootNode);
    }

    // 5. Resolve asset references
    // TODO: Read footer for assetRefTable offset/count (M4-S2)
    // For now, resolveReferences is a no-op
    // BinarySceneFooter footer;
    // memcpy(&footer, data + size - sizeof(BinarySceneFooter), sizeof(BinarySceneFooter));
    // resolveReferences(instances, data, footer.assetRefTableOffset, footer.assetRefCount, strings);

    result.success = true;
    return result;
}

// ===========================================================================
// Private: Header Validation
// ===========================================================================

bool BinaryDeserializer::validateHeader(const BinarySceneHeader &header) {
    // Check magic number
    if (memcmp(header.magic, "CCSC", 4) != 0) {
        return false;
    }

    // Check version
    if (header.version > BINARY_SCENE_VERSION) {
        // Future version — cannot deserialize
        // TODO: Apply migration functions (M4-S3)
        return false;
    }

    return true;
}

// ===========================================================================
// Private: String Table Parsing
// ===========================================================================

ccstd::vector<ccstd::string> BinaryDeserializer::parseStringTable(const uint8_t *data, uint32_t offset, uint32_t count) {
    ccstd::vector<ccstd::string> strings;
    strings.reserve(count);

    const uint8_t *basePtr = data + offset;

    // Skip the count field (4 bytes)
    const auto *entries = reinterpret_cast<const StringTableEntry *>(basePtr + sizeof(uint32_t));

    // String data starts after the entry array
    const uint8_t *stringDataStart = reinterpret_cast<const uint8_t *>(entries + count);

    for (uint32_t i = 0; i < count; ++i) {
        const auto &entry = entries[i];
        const char *strPtr = reinterpret_cast<const char *>(stringDataStart + entry.offset);
        strings.emplace_back(strPtr, entry.length);
    }

    return strings;
}

// ===========================================================================
// Private: Scene Creation
// ===========================================================================

Scene *BinaryDeserializer::createScene(const uint8_t * /*data*/, uint32_t /*size*/,
                                        const BinarySceneHeader & /*header*/,
                                        const ccstd::vector<ccstd::string> & /*strings*/) {
    // Create a new Scene object
    auto *scene = ccnew Scene();
    // TODO: Read scene-level properties from binary data (name, autoReleaseAssets, etc.)
    return scene;
}

// ===========================================================================
// Private: Node Tree Creation
// ===========================================================================

Node *BinaryDeserializer::createNodeTree(const uint8_t *data, uint32_t offset,
                                          const BinarySceneHeader &header,
                                          const ccstd::vector<ccstd::string> &strings,
                                          ccstd::vector<CCObject *> &instances) {
    if (offset == 0 || offset >= header.totalSize) {
        return nullptr;
    }

    // Read NodeEntry header
    NodeEntry nodeEntry;
    memcpy(&nodeEntry, data + offset, sizeof(NodeEntry));

    // Create node
    auto *node = ccnew Node();
    instances.push_back(node);

    // Set name
    if (nodeEntry.nameStringIndex < strings.size()) {
        node->setName(strings[nodeEntry.nameStringIndex]);
    }

    // Set layer
    node->setLayer(nodeEntry.layer);

    // Read transform data (position/rotation/scale) - follows NodeEntry
    const float *transformData = reinterpret_cast<const float *>(data + offset + sizeof(NodeEntry));

    // Position: 3 floats
    float posX = transformData[0];
    float posY = transformData[1];
    float posZ = transformData[2];
    node->setPosition(posX, posY, posZ);

    // Rotation: 4 floats (quaternion)
    float rotX = transformData[3];
    float rotY = transformData[4];
    float rotZ = transformData[5];
    float rotW = transformData[6];
    node->setRotation(rotX, rotY, rotZ, rotW);

    // Scale: 3 floats
    float scaleX = transformData[7];
    float scaleY = transformData[8];
    float scaleZ = transformData[9];
    node->setScale(scaleX, scaleY, scaleZ);

    // Read component data for this node
    uint32_t componentDataOffset = offset + sizeof(NodeEntry) + 10 * sizeof(float);
    for (uint32_t i = 0; i < nodeEntry.componentCount; ++i) {
        ComponentEntry compEntry;
        memcpy(&compEntry, data + componentDataOffset, sizeof(ComponentEntry));
        componentDataOffset += sizeof(ComponentEntry);

        // Create component via TypeRegistry
        auto *comp = TypeRegistry::getInstance().create(compEntry.classId);
        if (comp) {
            // Deserialize component properties
            if (compEntry.propertyDataSize > 0) {
                TypeRegistry::getInstance().deserialize(
                    compEntry.classId, comp,
                    data + componentDataOffset, compEntry.propertyDataSize);
            }
            auto *compNode = dynamic_cast<Component *>(comp);
            if (compNode) {
                node->addComponent(compNode);
            }
        }

        componentDataOffset += compEntry.propertyDataSize;
    }

    // Recurse into children
    uint32_t childOffset = componentDataOffset;
    for (uint32_t i = 0; i < nodeEntry.childCount; ++i) {
        // Read child offset from the data
        uint32_t childNodeOffset = 0;
        memcpy(&childNodeOffset, data + childOffset, sizeof(uint32_t));
        childOffset += sizeof(uint32_t);

        auto *childNode = createNodeTree(data, childNodeOffset, header, strings, instances);
        if (childNode) {
            node->addChild(childNode);
        }
    }

    return node;
}

// ===========================================================================
// Private: Reference Resolution
// ===========================================================================

void BinaryDeserializer::resolveReferences(const ccstd::vector<CCObject *> &instances,
                                            const uint8_t *data, uint32_t assetRefOffset,
                                            uint32_t assetRefCount,
                                            const ccstd::vector<ccstd::string> &strings) {
    // TODO: Implement full reference resolution in M4-S2
    // 1. Read AssetRefEntry array from data + assetRefOffset
    // 2. For each entry, look up the asset by UUID via AssetManager
    // 3. Set the asset reference on the owning instance's property
    // Currently stubbed — assets will need to be loaded separately

    for (uint32_t i = 0; i < assetRefCount; ++i) {
        AssetRefEntry entry;
        memcpy(&entry, data + assetRefOffset + i * sizeof(AssetRefEntry), sizeof(AssetRefEntry));

        if (entry.ownerInstanceIndex >= instances.size()) {
            continue;
        }

        // Resolve asset path from string table
        if (entry.assetPathStringIndex >= strings.size()) {
            continue;
        }

        const auto &assetPath = strings[entry.assetPathStringIndex];

        // TODO: Look up asset by path/UUID and set on the component property
        // auto* asset = AssetManager::getInstance()->getAsset(assetPath);
        // if (asset && entry.ownerInstanceIndex < instances.size()) {
        //     auto* owner = instances[entry.ownerInstanceIndex];
        //     owner->setAssetProperty(entry.propertyIndex, asset);
        // }
    }
}

} // namespace cc
