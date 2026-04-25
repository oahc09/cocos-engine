# C++ Asset 系统设计

> 版本: v2.0 (统一修订版)
> 日期: 2026-04-25
> 基于: AI/analysis-asset-system.md
> 目标: 将 Cocos Creator v3.8.8 的资产系统关键路径 C++ 化
> 总体规范: AI/design-cpp-master-spec.md v2.0
> 修订项: G-5 (线程模型), G-6 (错误传播), G-8 (GC协议), G-12 (Profiler), G-14 (回滚策略)

---

## 1. 现有 C++ 实现现状

### 1.1 已实现

| 组件 | 文件 | 状态 |
|------|------|------|
| Asset 基类 | `native/cocos/core/assets/Asset.h/.cpp` | ✅ 基本完成（uuid/native/refCount/destroy） |
| Texture2D | `native/cocos/core/assets/Texture2D.h/.cpp` | ✅ 完成（含 native 桥接） |
| Mesh | `native/cocos/core/assets/RenderingSubMesh.h/.cpp` | ✅ 完成（含 gfx::Buffer+IA） |
| Material | `native/cocos/core/assets/Material.h/.cpp` | ✅ 完成（17KB 头文件，复杂） |
| EffectAsset | `native/cocos/core/assets/EffectAsset.h/.cpp` | ✅ 完成（24.9KB 头文件） |
| SceneAsset | `native/cocos/core/assets/SceneAsset.h/.cpp` | ✅ 完成 |
| ImageAsset | `native/cocos/core/assets/ImageAsset.h/.cpp` | ✅ 完成 |
| Font | `native/cocos/core/assets/Font.h/.cpp` | ✅ 完成 |
| RefCounted | `native/cocos/base/RefCounted.h` | ✅ 完成 |
| IntrusivePtr | `native/cocos/base/Ptr.h` | ✅ 完成（智能指针） |
| DeferredReleasePool | `native/cocos/base/DeferredReleasePool.h` | ✅ 完成 |

### 1.2 TODO/未完成

| 功能 | 位置 | 说明 |
|------|------|------|
| Asset::decRef autoRelease | `Asset.cpp:87` | `//cjh TODO:` — 未实现自动释放 |
| getAssetUrlWithUuid | `Asset.cpp:33` | `//cjh TODO:` — 返回空字符串 |
| AssetManager | 无 C++ 实现 | 完全在 JS 侧 |
| Pipeline | 无 C++ 实现 | 完全在 JS 侧 |
| Bundle | 无 C++ 实现 | 完全在 JS 侧 |
| ReleaseManager | 无 C++ 实现 | 完全在 JS 侧 |
| DependUtil | 无 C++ 实现 | 完全在 JS 侧 |
| Downloader | 无 C++ 通用实现 | JSB 有 jsb.Downloader |

### 1.3 关键发现

**C++ 侧的 Asset 体系已经相当完善**，但存在以下断链：

1. **引用计数断裂**: C++ `_assetRefCount` 与 JS `_ref` 不同步
2. **无 C++ AssetManager**: 所有加载调度在 JS 侧
3. **无 C++ 释放管理**: `decRef` 的 autoRelease 逻辑未实现
4. **无 C++ 依赖追踪**: Bundle/DependUtil 完全在 JS

---

## 2. C++ 化架构设计

### 2.1 分层架构

```
┌───────────────────────────────────────────────────────────┐
│                    JS 资产管理层                           │
│  AssetManager / Bundle / Downloader / Parser / 依赖追踪   │
│  (编辑器模式 & 用户脚本资源管理)                            │
├───────────────────────────────────────────────────────────┤
│                    C++ 资产管理层                          │
│  NativeAssetManager / NativeBundle / NativePipeline       │
│  NativeReleaseManager / NativeDependUtil                  │
│  (运行时模式 & 内置资源快速路径)                            │
├───────────────────────────────────────────────────────────┤
│                    C++ 资源对象层                          │
│  Asset / Texture2D / Mesh / Material / SceneAsset ...    │
│  (已完成，保持现有实现)                                    │
└───────────────────────────────────────────────────────────┘
```

### 2.2 核心设计原则

1. **双模式共存**: JS AssetManager（编辑器）+ C++ NativeAssetManager（运行时）
2. **引用计数统一**: C++ 侧为权威引用计数源，JS 侧通过桥接同步
3. **内置资源 C++ 全权管理**: Texture2D/Mesh/Audio 的加载/解析/释放全在 C++
4. **逻辑资源保持 JS**: Prefab/AnimationClip 等继续由 JS 管理

---

## 3. 引用计数统一系统

### 3.1 当前问题

```
JS 侧: asset._ref = 2       ← JS 引用计数
C++ 侧: asset._assetRefCount = 0  ← C++ 引用计数，未同步
```

**根因**: C++ Asset 的 `_assetRefCount` 从未被 JS 侧的 `addRef()`/`decRef()` 同步更新。

### 3.2 统一方案

> **G-5 线程模型约束**: AssetRefManager 仅在主线程访问，无需锁或原子操作。
> 详见 master spec §1.4。

```cpp
// native/cocos/core/assets/AssetRefManager.h

class AssetRefManager {
public:
    static AssetRefManager& getInstance();

    // 统一引用计数操作（权威源）
    void addRef(Asset* asset);
    void decRef(Asset* asset, bool autoRelease = true);
    uint32_t getRefCount(Asset* asset) const;

    // 批量操作
    void addRefBatch(const ccstd::vector<Asset*>& assets);
    void decRefBatch(const ccstd::vector<Asset*>& assets, bool autoRelease = true);

    // 引用计数变化回调（通知 JS 侧）
    using RefCountChangedCallback = std::function<void(Asset*, uint32_t oldCount, uint32_t newCount)>;
    void setRefCountChangedCallback(const RefCountChangedCallback& cb);

private:
    // uuid → 引用计数（全局唯一）
    ccstd::unordered_map<ccstd::string, uint32_t> _refCounts;
    RefCountChangedCallback _callback;
};
```

### 3.3 JSB 桥接

```cpp
// JS 侧的 addRef/decRef 桥接到 C++
// native/cocos/bindings/manual/jsb_asset_manual.cpp

static bool js_asset_addRef(se::State& s) {
    auto* asset = static_cast<Asset*>(s.nativeThisObject());
    AssetRefManager::getInstance().addRef(asset);
    return true;
}

static bool js_asset_decRef(se::State& s) {
    auto* asset = static_cast<Asset*>(s.nativeThisObject());
    bool autoRelease = true;
    if (s.args().size() > 0) autoRelease = s.args()[0].toBoolean();
    AssetRefManager::getInstance().decRef(asset, autoRelease);
    return true;
}
```

### 3.4 C++ 侧的 addRef/decRef 实现

```cpp
void AssetRefManager::addRef(Asset* asset) {
    auto& count = _refCounts[asset->getUuid()];
    uint32_t oldCount = count++;
    asset->_assetRefCount = count;
    if (_callback) _callback(asset, oldCount, count);
}

void AssetRefManager::decRef(Asset* asset, bool autoRelease) {
    auto it = _refCounts.find(asset->getUuid());
    if (it == _refCounts.end() || it->second == 0) return;

    uint32_t oldCount = it->second--;
    asset->_assetRefCount = it->second;

    if (autoRelease && it->second == 0) {
        NativeReleaseManager::getInstance().tryRelease(asset);
    }

    if (_callback) _callback(asset, oldCount, it->second);
}
```

---

## 4. C++ NativeAssetManager

### 4.1 类设计

```cpp
// native/cocos/core/assets/NativeAssetManager.h

class NativeAssetManager : public RefCounted {
public:
    static NativeAssetManager* getInstance();

    // === 加载接口 ===

    // 通过 UUID 加载资源
    void load(const ccstd::string& uuid,
              const std::function<void(Asset*)>& onComplete);

    // 通过路径加载资源
    void loadByPath(const ccstd::string& path,
                    const ccstd::string& bundleName,
                    const std::function<void(Asset*)>& onComplete);

    // 批量加载
    void loadBatch(const ccstd::vector<ccstd::string>& uuids,
                   const std::function<void(const ccstd::vector<Asset*>&)>& onComplete);

    // 加载场景
    void loadScene(const ccstd::string& sceneName,
                   const ccstd::string& bundleName,
                   const std::function<void(SceneAsset*)>& onComplete);

    // === 缓存接口 ===

    Asset* getAsset(const ccstd::string& uuid) const;
    void addAsset(const ccstd::string& uuid, Asset* asset);
    void removeAsset(const ccstd::string& uuid);

    // === 释放接口 ===

    void release(Asset* asset);
    void releaseBatch(const ccstd::vector<Asset*>& assets);
    void releaseUnused();

    // === Bundle 管理 ===

    NativeBundle* createBundle(const ccstd::string& name, const ccstd::string& root);
    NativeBundle* getBundle(const ccstd::string& name) const;
    void removeBundle(const ccstd::string& name);

private:
    NativeAssetManager();

    // 管线
    IntrusivePtr<NativePipeline> _pipeline;
    IntrusivePtr<NativePipeline> _fetchPipeline;

    // Bundle 管理
    ccstd::unordered_map<ccstd::string, IntrusivePtr<NativeBundle>> _bundles;

    // 资源缓存
    ccstd::unordered_map<ccstd::string, IntrusivePtr<Asset>> _assets;

    // 依赖追踪
    IntrusivePtr<NativeDependUtil> _dependUtil;

    // 释放管理
    IntrusivePtr<NativeReleaseManager> _releaseManager;
};
```

### 4.2 加载管线

```cpp
// native/cocos/core/assets/NativePipeline.h

class NativePipeline : public RefCounted {
public:
    using PipeFunc = std::function<bool(/*Task*/void*)>;  // sync pipe
    using AsyncPipeFunc = std::function<void(/*Task*/void*, std::function<void(bool)>)>;  // async pipe

    void addPipe(PipeFunc pipe);
    void addAsyncPipe(AsyncPipeFunc pipe);

    // 同步执行
    bool syncExecute(void* task);

    // 异步执行
    void asyncExecute(void* task, const std::function<void(bool)>& onComplete);

private:
    ccstd::vector<PipeFunc> _syncPipes;
    ccstd::vector<AsyncPipeFunc> _asyncPipes;
};
```

### 4.3 加载流程（C++ 快速路径）

```
NativeAssetManager::load(uuid)
    ↓
[1. 检查缓存]
    ├── _assets[uuid] 存在 → addRef → 直接返回
    └── 不存在 → 继续
    ↓
[2. 查找 Bundle]
    ├── 遍历 _bundles → 找到包含 uuid 的 Bundle
    └── 未找到 → 错误回调
    ↓
[3. fetchPipeline]
    ├── 预处理: 验证参数、构建下载 URL
    └── fetch: 获取文件路径（本地/远程）
    ↓
[4. pipeline 主管线]
    ├── download: 读取文件内容
    │   ├── .json → 读取文本
    │   ├── .bin → 读取二进制
    │   ├── .png/.astc → 读取图片数据
    │   └── .scene.bin → 读取二进制场景
    ↓
    └── parse: 解析为运行时对象
        ├── 内置资源 → C++ 直接解析
        │   ├── Texture2D → ImageAsset → gfx.Texture
        │   ├── Mesh → gfx.Buffer + IA
        │   ├── Material → gfx.Material
        │   └── SceneAsset → BinaryDeserializer
        └── 逻辑资源 → 委托 JS 侧解析
            ├── Prefab → JS 反序列化
            ├── AnimationClip → JS 解析
            └── 用户脚本 → JS 解析
    ↓
[5. 依赖解析]
    ├── 解析资源依赖的 UUID 列表
    ├── 递归加载未加载的依赖
    └── 建立依赖图
    ↓
[6. 缓存 & 回调]
    ├── _assets[uuid] = asset
    ├── _dependUtil.parse(uuid, deps)
    └── onComplete(asset)
```

---

## 5. C++ NativeBundle

### 5.1 类设计

```cpp
// native/cocos/core/assets/NativeBundle.h

class NativeBundle : public RefCounted {
public:
    NativeBundle(const ccstd::string& name, const ccstd::string& root);
    ~NativeBundle() override;

    // 加载资源
    void load(const ccstd::string& path,
              const std::function<void(Asset*)>& onComplete);
    void loadDir(const ccstd::string& dir,
                 const std::function<void(const ccstd::vector<Asset*>&)>& onComplete);
    void loadScene(const ccstd::string& sceneName,
                   const std::function<void(SceneAsset*)>& onComplete);

    // 同步获取已加载资源
    Asset* get(const ccstd::string& path) const;
    SceneInfo getSceneInfo(const ccstd::string& sceneName) const;

    // 释放
    void release(const ccstd::string& path);

    // 配置
    bool init(const ccstd::string& configJson);

    inline const ccstd::string& getName() const { return _name; }
    inline const ccstd::string& getBasePath() const { return _basePath; }

private:
    ccstd::string _name;
    ccstd::string _basePath;

    // 资源配置表 (uuid → 路径/类型信息)
    struct AssetConfig {
        ccstd::string path;
        ccstd::string uuid;
        uint32_t classId;    // C++ 类型 ID
        bool isScene;
        ccstd::string nativeExt;
    };
    ccstd::unordered_map<ccstd::string, AssetConfig> _configs;  // uuid → config
    ccstd::unordered_map<ccstd::string, ccstd::string> _pathToUuid;  // path → uuid

    // 场景配置
    struct SceneInfo {
        ccstd::string url;
        ccstd::string uuid;
    };
    ccstd::unordered_map<ccstd::string, SceneInfo> _scenes;  // name → info
};
```

### 5.2 Bundle 配置解析

```cpp
bool NativeBundle::init(const ccstd::string& configJson) {
    // 解析 config.json（构建时生成的资源配置）
    // 格式：
    // {
    //   "paths": { "uuid": { "path": "...", "ctype": 4, ... } },
    //   "scenes": { "sceneName": { "url": "...", "uuid": "..." } }
    // }

    auto doc = rapidjson::Document();
    doc.Parse(configJson.c_str());

    const auto& paths = doc["paths"];
    for (auto it = paths.MemberBegin(); it != paths.MemberEnd(); ++it) {
        const auto& uuid = it->name.GetString();
        const auto& info = it->value;
        AssetConfig cfg;
        cfg.uuid = uuid;
        cfg.path = info["path"].GetString();
        cfg.classId = info["ctype"].GetUint();
        cfg.isScene = info.HasMember("isScene") && info["isScene"].GetBool();
        if (info.HasMember("native")) cfg.nativeExt = info["native"].GetString();
        _configs[uuid] = cfg;
        _pathToUuid[cfg.path] = uuid;
    }

    const auto& scenes = doc["scenes"];
    for (auto it = scenes.MemberBegin(); it != scenes.MemberEnd(); ++it) {
        SceneInfo si;
        si.url = it->value["url"].GetString();
        si.uuid = it->value["uuid"].GetString();
        _scenes[it->name.GetString()] = si;
    }

    return true;
}
```

---

## 6. C++ NativeReleaseManager

### 6.1 类设计

```cpp
// native/cocos/core/assets/NativeReleaseManager.h

class NativeReleaseManager : public RefCounted {
public:
    static NativeReleaseManager& getInstance();

    // 尝试释放资源
    void tryRelease(Asset* asset);

    // 场景切换时的批量释放
    void releaseSceneAssets(Scene* oldScene, bool autoRelease);

    // 强制释放未使用的资源
    void releaseUnused();

    // 遍历场景中所有引用的 Asset
    ccstd::vector<ccstd::string> collectSceneAssetRefs(Scene* scene);

private:
    // 引用遍历
    void visitNode(Node* node, ccstd::unordered_set<ccstd::string>& deps);
    void visitComponent(Component* comp, ccstd::unordered_set<ccstd::string>& deps);
    void visitAsset(Asset* asset, ccstd::unordered_set<ccstd::string>& deps);

    // 释放策略
    bool canRelease(Asset* asset) const;

    // 递减依赖资源的引用计数
    void decrementDependRefs(Asset* asset);
};
```

### 6.2 引用遍历实现

```cpp
void NativeReleaseManager::visitNode(Node* node,
                                      ccstd::unordered_set<ccstd::string>& deps) {
    // 遍历所有组件
    for (auto& comp : node->_components) {
        visitComponent(comp.get(), deps);
    }
    // 递归子节点
    for (auto& child : node->_children) {
        visitNode(child.get(), deps);
    }
}

void NativeReleaseManager::visitComponent(Component* comp,
                                           ccstd::unordered_set<ccstd::string>& deps) {
    // 内置组件: C++ 侧直接遍历已知 Asset 属性
    // - MeshRenderer: _mesh, _materials[]
    // - Sprite: _spriteFrame, _texture
    // - AudioSource: _clip
    // - Animation: _clips[]

    // 用户脚本组件: 通过 JS 回调遍历
    if (comp->isScriptComponent()) {
        // 委托 JS 侧的引用遍历
        collectScriptAssetRefs(comp, deps);
    } else {
        // C++ 内置组件的直接 Asset 属性遍历
        auto assetProps = comp->getAssetProperties();
        for (auto* asset : assetProps) {
            if (asset && !asset->getUuid().empty()) {
                visitAsset(asset, deps);
            }
        }
    }
}

void NativeReleaseManager::visitAsset(Asset* asset,
                                       ccstd::unordered_set<ccstd::string>& deps) {
    const auto& uuid = asset->getUuid();
    if (uuid.empty() || deps.count(uuid)) return;
    deps.insert(uuid);

    // 递归处理依赖资源
    auto* dependUtil = NativeAssetManager::getInstance()->getDependUtil();
    auto dependUuids = dependUtil->getDepends(uuid);
    for (const auto& depUuid : dependUuids) {
        auto* depAsset = NativeAssetManager::getInstance()->getAsset(depUuid);
        if (depAsset) {
            deps.insert(depUuid);
        }
    }
}
```

### 6.3 释放策略

```cpp
void NativeReleaseManager::tryRelease(Asset* asset) {
    if (!canRelease(asset)) return;

    // 1. 递减依赖资源的引用计数
    decrementDependRefs(asset);

    // 2. 从缓存中移除
    NativeAssetManager::getInstance()->removeAsset(asset->getUuid());

    // 3. 销毁资源
    asset->destroy();
}

bool NativeReleaseManager::canRelease(Asset* asset) const {
    // 引用计数归零 + 不在场景引用中
    if (AssetRefManager::getInstance().getRefCount(asset) > 0) {
        return false;
    }

    // 检查是否有场景节点引用此资源
    // （遍历所有活跃场景的节点 → 成本高，需要优化）
    // 优化方案: 维护 Asset → Node 反向索引

    return true;
}

void NativeReleaseManager::decrementDependRefs(Asset* asset) {
    auto* dependUtil = NativeAssetManager::getInstance()->getDependUtil();
    auto dependUuids = dependUtil->getDepends(asset->getUuid());

    for (const auto& depUuid : dependUuids) {
        auto* depAsset = NativeAssetManager::getInstance()->getAsset(depUuid);
        if (depAsset) {
            AssetRefManager::getInstance().decRef(depAsset, true);
        }
    }
}
```

---

## 7. C++ NativeDependUtil

### 7.1 类设计

```cpp
// native/cocos/core/assets/NativeDependUtil.h

class NativeDependUtil : public RefCounted {
public:
    // 记录资源的依赖关系
    void parse(const ccstd::string& uuid,
               const ccstd::vector<ccstd::string>& depends);

    // 获取资源的所有依赖 UUID
    ccstd::vector<ccstd::string> getDepends(const ccstd::string& uuid) const;

    // 获取资源的所有直接和间接依赖（传递闭包）
    ccstd::vector<ccstd::string> getAllDepends(const ccstd::string& uuid) const;

    // 移除依赖记录
    void remove(const ccstd::string& uuid);

    // 检查资源是否被其他资源依赖
    bool isReferencedBy(const ccstd::string& uuid,
                        const ccstd::string& byUuid) const;

private:
    // uuid → 直接依赖列表
    ccstd::unordered_map<ccstd::string, ccstd::vector<ccstd::string>> _depends;

    // uuid → 被依赖列表（反向索引）
    ccstd::unordered_map<ccstd::string, ccstd::vector<ccstd::string>> _referencedBy;
};
```

---

## 8. 内置资源 C++ 全权管理

### 8.1 Texture2D 完整 C++ 加载路径

```
NativeAssetManager::load(uuid)
    ↓
[1. 查找 Bundle 配置]
    → AssetConfig: path="textures/hero", nativeExt=".astc", classId=TEXTURE2D_ID
    ↓
[2. 下载阶段]
    ├── 读取 .json 元数据 → 解析宽高/格式/mipmap 数
    └── 读取 .astc 原生文件 → 二进制像素数据
    ↓
[3. C++ 直接创建]
    ├── auto* tex = ccnew Texture2D()
    ├── tex->setUuid(uuid)
    ├── tex->setImageData(data, width, height, format)
    │   ├── 创建 gfx::Texture → 上传 GPU
    │   └── 设置 _gfxTexture
    └── tex->onLoaded()
    ↓
[4. 缓存 & 返回]
    → _assets[uuid] = tex
    → onComplete(tex)
```

**优势**: 完全跳过 JS 侧的 Texture2D 构造、属性赋值、native 桥接。性能提升约 3-5×。

### 8.2 Mesh 完整 C++ 加载路径

```
NativeAssetManager::load(uuid)
    ↓
[1. 查找 Bundle 配置]
    → AssetConfig: path="models/character", nativeExt=".bin", classId=MESH_ID
    ↓
[2. 下载阶段]
    ├── 读取 .json 元数据 → 顶点属性/包围盒/子网格
    └── 读取 .bin 原生文件 → 顶点+索引数据
    ↓
[3. C++ 直接创建]
    ├── auto* mesh = ccnew Mesh()
    ├── mesh->setUuid(uuid)
    ├── mesh->setBinaryData(vb, ib, attributes)
    │   ├── 创建 gfx::Buffer (VB + IB)
    │   ├── 创建 gfx::InputAssembler
    │   └── 设置 _nativeAsset
    └── mesh->onLoaded()
    ↓
[4. 缓存 & 返回]
```

### 8.3 Component 的 Asset 属性访问接口

```cpp
// 为内置组件添加 Asset 属性查询接口
// 供 NativeReleaseManager 使用

class MeshRenderer : public Component {
public:
    // 新增: 返回此组件引用的所有 Asset
    ccstd::vector<Asset*> getAssetProperties() const override {
        ccstd::vector<Asset*> assets;
        if (_mesh) assets.push_back(_mesh);
        for (auto& mat : _materials) {
            if (mat) assets.push_back(mat);
        }
        return assets;
    }
};
```

---

## 9. JS ↔ C++ 资产系统桥接

### 9.1 双模式切换

```cpp
// 全局运行模式
enum class AssetManagerMode {
    JS_ONLY,        // 编辑器模式: 全部走 JS
    NATIVE_FAST,    // 运行时模式: 内置资源走 C++ 快速路径
    NATIVE_FULL,    // 原生模式: 全部走 C++（未来目标）
};

// 设置运行模式
void NativeAssetManager::setMode(AssetManagerMode mode);

// 加载请求路由
void NativeAssetManager::load(const ccstd::string& uuid,
                               const std::function<void(Asset*)>& onComplete) {
    if (_mode == AssetManagerMode::JS_ONLY) {
        // 委托给 JS AssetManager
        callJSLoad(uuid, onComplete);
        return;
    }

    // C++ 快速路径
    auto* cached = getAsset(uuid);
    if (cached) {
        AssetRefManager::getInstance().addRef(cached);
        onComplete(cached);
        return;
    }

    // 查找 Bundle 配置
    auto [bundle, config] = findAssetConfig(uuid);
    if (!config) {
        // 未找到配置，回退到 JS
        callJSLoad(uuid, onComplete);
        return;
    }

    if (TypeRegistry::getInstance()->hasType(config->classId)) {
        // 内置资源 → C++ 加载
        loadNativeAsset(bundle, *config, onComplete);
    } else {
        // 逻辑资源 → JS 加载
        callJSLoad(uuid, onComplete);
    }
}
```

### 9.2 JS 侧事件同步

```
C++ 侧加载完成 → 通知 JS 侧创建代理对象
    ↓
C++: asset = new Texture2D()
    ↓
JSB: se::Object* jsObj = createJSProxy(asset)
    ↓
JS: assetManager._assets[uuid] = jsObj  ← JS 侧缓存也更新
```

### 9.3 用户脚本中的 Asset 引用

用户脚本组件引用 Asset 时，走 JS → C++ 同步路径：

```typescript
// 用户脚本
@ccclass('PlayerController')
export class PlayerController extends Component {
    @property(Texture2D)
    public avatar: Texture2D | null = null;  // 引用 Asset

    // JS 侧的 Property setter 自动调用 addRef/decRef
}
```

**流程**:
1. JS 设置 `this.avatar = tex` → Property setter 触发
2. 旧值 `decRef()` → C++ `AssetRefManager::decRef()`
3. 新值 `addRef()` → C++ `AssetRefManager::addRef()`
4. 引用计数在 C++ 侧统一维护

---

## 10. 性能对比预估

### 10.1 资源加载时间（100 个 Texture2D 场景）

| 阶段 | JS 路径 | C++ 快速路径 | 加速比 |
|------|---------|-------------|--------|
| 文件下载 | 50ms | 50ms (相同) | 1× |
| JSON 解析 | 15ms | 5ms (C++ rapidjson) | 3× |
| 对象创建 | 20ms | 3ms (直接构造) | 7× |
| Native 桥接 | 30ms | 0ms (跳过) | ∞ |
| GPU 上传 | 25ms | 20ms (减少拷贝) | 1.25× |
| **总计** | **140ms** | **78ms** | **~1.8×** |

### 10.2 内存占用

| 指标 | JS 路径 | C++ 快速路径 | 节省 |
|------|---------|-------------|------|
| Asset JS 对象 | 800B/个 | 0 (无 JS 代理) | 100% |
| Native 对象 | 200B/个 | 200B/个 | 0% |
| 桥接开销 | 128B/个 | 0 | 100% |
| 100个 Texture2D | ~113KB | ~20KB | **82%** |

### 10.3 场景切换时的释放速度

| 操作 | JS ReleaseManager | C++ ReleaseManager | 加速比 |
|------|-------------------|-------------------|--------|
| 引用遍历(500节点) | 12ms | 2ms | 6× |
| 释放决策 | 5ms | 1ms | 5× |
| 批量 decRef | 8ms | 1ms | 8× |
| **总计** | **25ms** | **4ms** | **~6×** |

---

## 11. 实施路线图

### Phase 1: 引用计数统一 (1 周)

| 任务 | 优先级 |
|------|--------|
| 实现 AssetRefManager | P0 |
| JSB 桥接 addRef/decRef | P0 |
| 补全 Asset::decRef autoRelease 逻辑 | P0 |
| 内置组件添加 getAssetProperties() | P1 |
| 引用计数同步测试 | P0 |

### Phase 2: C++ AssetManager 核心 (3 周)

| 任务 | 优先级 | 依赖 |
|------|--------|------|
| 实现 NativePipeline | P0 | — |
| 实现 NativeBundle | P0 | — |
| 实现 NativeDependUtil | P0 | — |
| 实现 NativeAssetManager | P0 | Pipeline + Bundle + DependUtil |
| Texture2D C++ 完整加载路径 | P0 | NativeAssetManager |
| Mesh C++ 完整加载路径 | P1 | NativeAssetManager |
| 双模式切换逻辑 | P0 | NativeAssetManager |
| JSB 桥接 | P0 | 全部 |

### Phase 3: C++ ReleaseManager (2 周)

| 任务 | 优先级 | 依赖 |
|------|--------|------|
| 实现 NativeReleaseManager | P0 | AssetRefManager |
| 引用遍历（内置组件） | P0 | getAssetProperties |
| 引用遍历（用户脚本 JS 回调） | P1 | JSB 桥接 |
| 场景切换自动释放 | P1 | Director C++ 化 |
| Asset → Node 反向索引优化 | P2 | ReleaseManager |

### Phase 4: 集成测试 (2 周)

| 任务 | 优先级 |
|------|--------|
| 编辑器模式 JS_ONLY 验证 | P0 |
| 运行时模式 NATIVE_FAST 验证 | P0 |
| 引用计数一致性测试 | P0 |
| 内存泄漏检测 | P0 |
| 大规模场景压力测试 | P1 |
| A/B 性能基准 | P1 |

---

## 12. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 引用计数同步延迟导致短暂不一致 | 中 | 单线程模型，读写无需锁 (G-5) |
| 内置组件 getAssetProperties 遗漏属性 | 高 | 自动化测试覆盖所有内置组件 |
| 用户脚本的 Asset 引用无法在 C++ 追踪 | 高 | JS 回调机制兜底 |
| Bundle 配置格式变更 | 中 | 构建时生成标准格式，版本号控制 |
| C++ 侧加载失败无法回退 | 高 | loadNativeAsset 失败时自动降级到 JS 路径 (G-14) |
| 并发加载导致重复创建 | 中 | 单线程模型 + 二次缓存检查 |

---

## 13. 统一工程约束 (G-5~G-15 补全)

> 以下内容补全 v1.0 遗漏项，完整定义见 master spec 对应章节。

### 13.1 线程模型 (G-5)

- **主线程独占**: AssetRefManager / NativeAssetManager 仅在主线程访问
- **无锁设计**: 不需要 `std::mutex` 或 `std::atomic`
- **异步加载**: 通过回调回到主线程，与 JS 异步模型一致
- **Worker 线程**: 仅做 IO/解码，不触碰 Asset 对象

### 13.2 错误传播 (G-6)

| 错误源 | 处理方式 |
|--------|---------|
| C++ 资源加载崩溃 | `CC_ASSERT` (Debug) / 返回 `nullptr` (Release) |
| 二进制解析失败 | 自动降级到 JSON + JS 反序列化 |
| 异步加载失败 | `onComplete(error, null)` 回调 |
| 引用计数下溢 | Debug 模式 CC_ASSERT + 忽略操作 |

### 13.3 GC 交互协议 (G-8)

```
C++ Asset 引用计数 (权威源)       JS Asset 包装器 (GC 管理)
────────────────────────          ──────────────────────
AssetRefManager::addRef()         JS property setter → C++ addRef()
AssetRefManager::decRef()         JS property setter → C++ decRef()
    ↓ refCount == 0
NativeReleaseManager::tryRelease()
    ↓
Asset::destroy()
    ↓
Asset::_scriptObject = nullptr    → JS isValid() → false
    ↓
C++ Asset 析构                    → V8 GC 回收 se::Object
```

**关键**: C++ 引用计数为权威源 (INV-3)，JS 侧缓存必须与 C++ 同步 (INV-5)。

### 13.4 回滚策略 (G-14)

| 场景 | 回滚方案 |
|------|---------|
| C++ 资产加载崩溃 | `loadNativeAsset` 失败 → `callJSLoad` 兜底 |
| 引用计数不一致 | Debug 模式校验 + 重置功能 |
| 整体 C++ 化回退 | `NativeAssetManager::setMode(JS_ONLY)` 全局回退 |

### 13.5 Profiler 钩子 (G-12)

```cpp
// 资源加载性能记录
void NativeAssetManager::load(const ccstd::string& uuid, /* ... */) {
    EngineProfiler::beginSection("asset_load");
    // ... 加载逻辑 ...
    EngineProfiler::endSection();
}

// 场景切换释放性能记录
void NativeReleaseManager::releaseSceneAssets(Scene* oldScene, bool autoRelease) {
    EngineProfiler::beginSection("asset_release");
    // ... 释放逻辑 ...
    EngineProfiler::endSection();
}
```

详见 master spec §7.3

### 13.6 基准测试 (G-15)

```
test/benchmarks/
    └── bench-asset-load/          ← 资产加载基准
        ├── 100-texture.scene
        ├── 1000-texture.scene
        ├── mixed-assets.scene
        └── bench.cpp
```

详见 master spec §7.5

---

## 14. 统一实施时间线 (I-5 修订)

> **I-5 裁决**: 取消本文档 v1.0 的独立 Phase 1-4 时间线，统一到 master spec §8 的 26 周时间线。

| 阶段 | 对应 Master Phase | 资产系统工作 |
|------|------------------|------------|
| 基础设施 | Phase 0 (Week 1-3) | AssetRefManager 引用计数统一 + JSB 桥接 |
| 资产系统 | Phase 3 (Week 13-16) | NativePipeline + NativeBundle + NativeDependUtil + NativeAssetManager |
| 释放管理 | Phase 3 (Week 13-16) | NativeReleaseManager + 场景切换自动释放 |
| 集成 | Phase 5 (Week 23-26) | 引用计数一致性测试 + 内存泄漏检测 + 性能基准 |
