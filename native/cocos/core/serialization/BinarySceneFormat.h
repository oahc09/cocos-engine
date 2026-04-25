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
#include "base/std/container/string.h"
#include "base/std/container/vector.h"

namespace cc {

// Magic number for binary scene files ("CCSC" in little-endian)
constexpr uint32_t BINARY_SCENE_MAGIC = 0x43534343;

// Current format version
constexpr uint16_t BINARY_SCENE_VERSION = 1;

// Flags
enum class SceneFormatFlags : uint16_t {
    NONE = 0,
    HAS_SCRIPT_COMPONENTS = 1 << 0, // Contains user script component JSON
    COMPRESSED_STRINGS = 1 << 1,    // String table is compressed
    BIG_ENDIAN = 1 << 2,           // Data is big-endian
};

struct BinarySceneHeader {
    char magic[4];              // "CCSC"
    uint16_t version;           // Format version
    uint16_t flags;             // SceneFormatFlags bitmask
    uint32_t stringTableOffset; // Offset to string table from file start
    uint32_t instanceTableOffset; // Offset to instance table from file start
    uint32_t rootNodeOffset;    // Offset to root node data from file start
    uint32_t totalSize;         // Total file size in bytes
    uint8_t reserved[8];
};
static_assert(sizeof(BinarySceneHeader) == 32, "BinarySceneHeader must be 32 bytes");

struct StringTableEntry {
    uint32_t offset; // Offset from start of string data section
    uint32_t length; // String length in bytes (not including null terminator)
};

struct InstanceTableEntry {
    uint32_t classId;       // TypeRegistry classId
    uint32_t instanceIndex; // 0-based instance index
    uint32_t parentIndex;   // Parent instance index (0xFFFFFFFF for root)
    uint32_t dataOffset;    // Offset to instance data from file start
    uint32_t dataSize;      // Size of instance data in bytes
};

struct NodeEntry {
    uint32_t instanceIndex;  // Matches InstanceTableEntry.instanceIndex
    uint32_t childCount;     // Number of children
    uint32_t componentCount; // Number of components attached to this node
    uint32_t nameStringIndex; // Index into string table for node name
    uint32_t layer;          // Node layer
    uint32_t lsfIndex;       // Local scale factor string index (if needed)
    // Position/rotation/scale follow as fixed-size binary (36 bytes total:
    //   3×float position + 4×float rotation + 3×float scale)
};

struct ComponentEntry {
    uint32_t classId;           // TypeRegistry classId
    uint32_t instanceIndex;     // Matches InstanceTableEntry.instanceIndex
    uint32_t nodeInstanceIndex; // Owning node's instance index
    uint32_t propertyDataSize; // Size of property binary data
    // Property data follows immediately after this header.
    // For builtin components: fixed-layout binary, zero-reflection.
    // For user script components: classPath string index + JSON blob.
};

struct AssetRefEntry {
    uint32_t ownerInstanceIndex;  // Instance that holds the reference
    uint32_t assetPathStringIndex; // String table index for asset UUID/path
    uint16_t assetType;           // Asset type hint (0=unknown, 1=texture, 2=mesh, etc.)
    uint16_t propertyIndex;       // Property index on the owner
};

struct BinarySceneFooter {
    uint32_t assetRefTableOffset;
    uint32_t assetRefCount;
    uint32_t checksum; // CRC32 of all data before this footer
    uint8_t reserved[20];
};
static_assert(sizeof(BinarySceneFooter) == 32, "BinarySceneFooter must be 32 bytes");

} // namespace cc
