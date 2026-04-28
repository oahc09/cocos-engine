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
#include "core/assets/AssetManager.h"
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

    if (!validateHeader(header, size, result.errorMessage)) {
        return result;
    }

    // 2. Parse string table
    ccstd::vector<ccstd::string> strings;
    if (!parseStringTable(data, size, header.stringTableOffset, strings, result.errorMessage)) {
        return result;
    }

    // 3. Create scene
    auto *scene = createScene(data, size, header, strings);
    if (!scene) {
        result.errorMessage = "BinaryDeserializer: failed to create scene";
        return result;
    }

    result.scene.reset(scene);

    // 4. Create node tree
    ccstd::vector<CCObject *> instances;
    auto *rootNode = createNodeTree(data, header.rootNodeOffset, header, strings, instances, result.errorMessage);
    if (!rootNode) {
        if (result.errorMessage.empty()) {
            result.errorMessage = "BinaryDeserializer: failed to create root node";
        }
        result.scene.reset();
        return result;
    }
    scene->addChild(rootNode);

    // 5. Resolve asset references if a plausible footer is present.
    if (size >= sizeof(BinarySceneHeader) + sizeof(BinarySceneFooter)) {
        BinarySceneFooter footer{};
        memcpy(&footer, data + size - sizeof(BinarySceneFooter), sizeof(BinarySceneFooter));

        const uint64_t refsEnd = static_cast<uint64_t>(footer.assetRefTableOffset) +
                                 static_cast<uint64_t>(footer.assetRefCount) * sizeof(AssetRefEntry);
        const bool hasPlausibleFooter =
            (footer.assetRefTableOffset == 0 && footer.assetRefCount == 0) ||
            (footer.assetRefTableOffset < size - sizeof(BinarySceneFooter) && refsEnd <= size - sizeof(BinarySceneFooter));

        if (hasPlausibleFooter && footer.assetRefCount > 0) {
            if (!resolveReferences(instances, data, size,
                                   footer.assetRefTableOffset, footer.assetRefCount,
                                   strings, result.assets, result.errorMessage)) {
                result.scene.reset();
                return result;
            }
        }
    }

    result.success = true;
    return result;
}

// ===========================================================================
// Private: Header Validation
// ===========================================================================

bool BinaryDeserializer::validateHeader(const BinarySceneHeader &header, uint32_t size, ccstd::string &errorMessage) {
    // Check magic number
    if (memcmp(header.magic, "CCSC", 4) != 0) {
        errorMessage = "BinaryDeserializer: invalid header (bad magic)";
        return false;
    }

    // Check version
    if (header.version > BINARY_SCENE_VERSION) {
        errorMessage = "BinaryDeserializer: invalid header (unsupported version)";
        return false;
    }

    if (header.totalSize != size) {
        errorMessage = "BinaryDeserializer: file size mismatch";
        return false;
    }

    if (header.stringTableOffset < sizeof(BinarySceneHeader) || header.stringTableOffset >= header.totalSize) {
        errorMessage = "BinaryDeserializer: invalid header (string table offset out of bounds)";
        return false;
    }

    if (header.rootNodeOffset == 0 || header.rootNodeOffset >= header.totalSize) {
        errorMessage = "BinaryDeserializer: invalid header (root node offset out of bounds)";
        return false;
    }

    return true;
}

// ===========================================================================
// Private: String Table Parsing
// ===========================================================================

bool BinaryDeserializer::parseStringTable(const uint8_t *data, uint32_t size, uint32_t offset,
                                          ccstd::vector<ccstd::string> &strings,
                                          ccstd::string &errorMessage) {
    if (offset > size - sizeof(uint32_t)) {
        errorMessage = "BinaryDeserializer: string table header is truncated";
        return false;
    }

    const uint8_t *basePtr = data + offset;
    uint32_t count = 0;
    memcpy(&count, basePtr, sizeof(uint32_t));

    const uint64_t entryBytes = static_cast<uint64_t>(count) * sizeof(StringTableEntry);
    const uint64_t stringDataOffset = static_cast<uint64_t>(offset) + sizeof(uint32_t) + entryBytes;
    if (stringDataOffset > size) {
        errorMessage = "BinaryDeserializer: string table entries are truncated";
        return false;
    }

    strings.clear();
    strings.reserve(count);

    // Skip the count field (4 bytes)
    const auto *entries = reinterpret_cast<const StringTableEntry *>(basePtr + sizeof(uint32_t));

    // String data starts after the entry array
    const uint8_t *stringDataStart = reinterpret_cast<const uint8_t *>(entries + count);

    for (uint32_t i = 0; i < count; ++i) {
        const auto &entry = entries[i];
        const uint64_t strBegin = stringDataOffset + entry.offset;
        const uint64_t strEnd = strBegin + entry.length;
        if (strEnd > size) {
            errorMessage = "BinaryDeserializer: string table data is truncated";
            strings.clear();
            return false;
        }
        const char *strPtr = reinterpret_cast<const char *>(stringDataStart + entry.offset);
        strings.emplace_back(strPtr, entry.length);
    }

    return true;
}

// ===========================================================================
// Private: Scene Creation
// ===========================================================================

Scene *BinaryDeserializer::createScene(const uint8_t *data, uint32_t /*size*/,
                                        const BinarySceneHeader &header,
                                        const ccstd::vector<ccstd::string> &strings) {
    // Create a new Scene object
    auto *scene = ccnew Scene();
    const uint32_t sceneEntryOffset = sizeof(BinarySceneHeader);
    const bool hasSceneEntry =
        header.stringTableOffset >= sceneEntryOffset + sizeof(SceneEntry);
    if (hasSceneEntry) {
        SceneEntry sceneEntry{};
        memcpy(&sceneEntry, data + sceneEntryOffset, sizeof(SceneEntry));
        if (sceneEntry.nameStringIndex < strings.size()) {
            scene->setName(strings[sceneEntry.nameStringIndex]);
        }
        scene->setAutoReleaseAssets(sceneEntry.autoReleaseAssets != 0);
    }
    return scene;
}

// ===========================================================================
// Private: Node Tree Creation
// ===========================================================================

Node *BinaryDeserializer::createNodeTree(const uint8_t *data, uint32_t offset,
                                          const BinarySceneHeader &header,
                                          const ccstd::vector<ccstd::string> &strings,
                                          ccstd::vector<CCObject *> &instances,
                                          ccstd::string &errorMessage) {
    if (offset == 0 || offset >= header.totalSize) {
        errorMessage = "BinaryDeserializer: root node offset is invalid";
        return nullptr;
    }

    constexpr uint32_t kNodeTransformBytes = 10U * sizeof(float);
    const uint64_t minNodeBytes = static_cast<uint64_t>(sizeof(NodeEntry)) + kNodeTransformBytes;
    if (static_cast<uint64_t>(offset) + minNodeBytes > header.totalSize) {
        errorMessage = "BinaryDeserializer: node entry is truncated";
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
    uint32_t componentDataOffset = offset + sizeof(NodeEntry) + kNodeTransformBytes;
    for (uint32_t i = 0; i < nodeEntry.componentCount; ++i) {
        if (componentDataOffset > header.totalSize - sizeof(ComponentEntry)) {
            errorMessage = "BinaryDeserializer: component entry is truncated";
            return nullptr;
        }

        ComponentEntry compEntry;
        memcpy(&compEntry, data + componentDataOffset, sizeof(ComponentEntry));
        componentDataOffset += sizeof(ComponentEntry);

        if (compEntry.propertyDataSize > header.totalSize - componentDataOffset) {
            errorMessage = "BinaryDeserializer: component property data is truncated";
            return nullptr;
        }

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
        if (childOffset > header.totalSize - sizeof(uint32_t)) {
            errorMessage = "BinaryDeserializer: child offset table is truncated";
            return nullptr;
        }

        // Read child offset from the data
        uint32_t childNodeOffset = 0;
        memcpy(&childNodeOffset, data + childOffset, sizeof(uint32_t));
        childOffset += sizeof(uint32_t);

        auto *childNode = createNodeTree(data, childNodeOffset, header, strings, instances, errorMessage);
        if (childNode) {
            node->addChild(childNode);
        } else {
            return nullptr;
        }
    }

    return node;
}

// ===========================================================================
// Private: Reference Resolution
// ===========================================================================

bool BinaryDeserializer::resolveReferences(const ccstd::vector<CCObject *> &instances,
                                            const uint8_t *data, uint32_t size,
                                            uint32_t assetRefOffset,
                                            uint32_t assetRefCount,
                                            const ccstd::vector<ccstd::string> &strings,
                                            ccstd::vector<IntrusivePtr<Asset>> &assets,
                                            ccstd::string &errorMessage) {
    const uint64_t refsEnd = static_cast<uint64_t>(assetRefOffset) +
                             static_cast<uint64_t>(assetRefCount) * sizeof(AssetRefEntry);
    if (assetRefOffset >= size || refsEnd > size) {
        errorMessage = "BinaryDeserializer: asset reference table is truncated";
        return false;
    }

    auto &assetManager = AssetManager::getInstance();

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
        Asset *asset = assetManager.getCachedAsset(assetPath);
        if (asset == nullptr) {
            continue;
        }

        bool alreadyAdded = false;
        for (const auto &existing : assets) {
            if (existing.get() == asset) {
                alreadyAdded = true;
                break;
            }
        }
        if (!alreadyAdded) {
            assets.emplace_back(asset);
        }
    }

    return true;
}

} // namespace cc
