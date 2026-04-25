# Cocos Creator v3.8.8 - 资产系统深度分析

> 生成日期: 2026-04-25
> 分析范围: Asset基类 / AssetManager加载管线 / 引用计数GC / Native桥接

---

## 1. Asset 基类 (`cocos/asset/assets/asset.ts`, 402行)

### 1.1 类层次

```
CCObject ← Eventify(CCObject) ← Asset
```

Asset 继承自 `Eventify(CCObject)`，增加了事件系统支持。

### 1.2 关键属性

| 属性 | 类型 | 说明 |
|------|------|------|
| `_uuid` | `string` | 资源唯一标识符（通过 `Object.defineProperty` 定义，不可枚举） |
| `_native` | `string` | 原生资源文件路径标识（`@serializable`） |
| `_nativeUrl` | `string` | 原生资源完整 URL（缓存计算结果） |
| `_file` | `any` | 底层原生资源对象（私有） |
| `_ref` | `number` | 引用计数（私有，初始为 0） |
| `isDefault` | `boolean` | 是否为默认资源（仅编辑器/预览） |
| `loaded` | `boolean` | 是否已加载（已废弃 v3.3） |

### 1.3 引用计数系统

```typescript
public addRef(): Asset { this._ref++; return this; }

public decRef(autoRelease = true): Asset {
    if (this._ref > 0) this._ref--;
    if (autoRelease) {
        assetManager.getReleaseManager().tryRelease(this);
    }
    return this;
}

public get refCount(): number { return this._ref; }
```

**核心机制**:
- `addRef()` / `decRef()` 手动管理引用计数
- `decRef()` 时若 `autoRelease=true`（默认），会调用 `ReleaseManager.tryRelease()`
- 引用计数归零不会立即销毁，由 `ReleaseManager` 决定是否释放

### 1.4 原生资源桥接 (`_native` / `nativeAsset`)

```typescript
// _native: 序列化存储的原始文件路径标识
// 例: ".png" / ".pvr" / ".astc" 等

// _nativeAsset (或 nativeAsset): 运行时的原生资源对象
get nativeAsset(): any { return this._file; }
set nativeAsset(obj) { this._file = obj; }

// 原生依赖声明
get _nativeDep(): { __isNative__: true, uuid, ext } | undefined {
    if (this._native) return { __isNative__: true, uuid: this._uuid, ext: this._native };
}
```

**Native 桥接流程**:
```
Asset._native = ".png"
    ↓
AssetManager 加载时检测 _nativeDep
    ↓
下载原生文件（如 .png / .pvr / .astc）
    ↓
创建原生资源对象（如 gfx.Texture）
    ↓
赋值到 Asset._nativeAsset (nativeAsset / _file)
```

### 1.5 nativeUrl 计算逻辑

```typescript
get nativeUrl(): string {
    if (!this._nativeUrl) {
        const name = this._native;
        if (name.charCodeAt(0) === 47) {       // '/' 开头
            return name.slice(1);               // 非库文件，去掉前缀
        }
        if (name.charCodeAt(0) === 46) {       // '.' 开头（如 .png）
            this._nativeUrl = getUrlWithUuid(this._uuid, { nativeExt: name, isNative: true });
        } else {                                // 完整文件名
            this._nativeUrl = getUrlWithUuid(this._uuid, { __nativeName__: name, ... });
        }
    }
    return this._nativeUrl;
}
```

### 1.6 destroy()

```typescript
public destroy(): boolean {
    debug(getError(12101, this._uuid));  // 仅打印日志，不阻止销毁
    return super.destroy();
}
```

### 1.7 onLoaded / initDefault / validate

- `onLoaded()`: 资源加载完成回调，子类可覆写（如 Prefab.onLoaded 展开嵌套节点）
- `initDefault(uuid)`: 初始化为默认资源
- `validate()`: 验证资源可用性

---

## 2. AssetManager 加载管线 (`cocos/asset/asset-manager/`)

### 2.1 整体架构

```
AssetManager
    ├── fetchPipeline: Pipeline     ← 预处理 + fetch
    ├── pipeline: Pipeline          ← 主加载管线（download → parse）
    ├── bundles: Map<string, Bundle> ← Bundle 管理
    ├── assets: Cache               ← 已加载资源缓存
    ├── generalCache: Cache         ← 通用缓存
    ├── downloader: Downloader      ← 文件下载器
    ├── parser: Parser              ← 资源解析器
    ├── packManager: PackManager    ← 打包管理器
    ├── releaseManager: ReleaseManager ← 释放管理器
    └── dependUtil: DependUtil      ← 依赖工具
```

### 2.2 Pipeline 管线系统 (`pipeline.ts`, 280行)

Pipeline 是资源加载的核心执行模型，支持同步和异步两种模式：

```typescript
class Pipeline {
    pipes: Function[];  // 管道函数数组

    // 同步执行
    sync(task): any {
        for (pipe of pipes) {
            result = pipe(task);
            if (result) return result;  // 出错即停
            task.input = task.output;   // 上一级输出 → 下一级输入
        }
        return task.output;
    }

    // 异步执行
    async(task): void {
        this._flow(0, task);
    }

    _flow(index, task): void {
        pipe = pipes[index];
        pipe(task, (result) => {
            if (result) {
                task.dispatch('complete', result);  // 出错
            } else if (++index < pipes.length) {
                task.input = task.output;
                this._flow(index, task);            // 继续下一级
            } else {
                task.dispatch('complete', null, task.output);  // 完成
            }
        });
    }
}
```

**管道函数签名**:
```typescript
// 异步管道
type AsyncPipe = (task: Task, done: (err?: Error | null) => void) => void;

// 同步管道
type SyncPipe = (task: Task) => Error | void;
```

### 2.3 主加载管线

引擎启动时构建的两条管线：

**fetchPipeline**: `preprocess → fetch`
- 预处理请求参数
- 获取远程/本地文件内容

**pipeline**（主管线）: `download → parse`
- **download**: 下载文件（JSON/二进制/图片等）
- **parse**: 解析为运行时对象

### 2.4 加载流程详解

```
assetManager.loadRemote(url, options, callback)
    ↓
[创建 Task]
    ↓
[fetchPipeline]
    ├── preprocess: 验证参数、转换 URL
    └── fetch: 获取文件内容
    ↓
[transformPipeline]
    └── 转换 URL 到可下载格式
    ↓
[pipeline] 主加载管线
    ├── download: 下载文件
    │   ├── JSON 文件 → downloader.downloadFile()
    │   ├── 图片文件 → downloadDomImage() / downloadFile()
    │   ├── 音频文件 → downloadFile()
    │   └── 原生文件 → downloader.downloadFile()
    ↓
    └── parse: 解析为运行时对象
        ├── JSON → deserialize → Asset 实例
        ├── 图片 → createImageBitmap() → ImageBitmap/HTMLImageElement
        ├── 文本 → 直接返回
        └── 二进制 → ArrayBuffer
    ↓
[缓存]
    ├── assets.add(uuid, asset)  ← 缓存已加载资源
    └── dependUtil.parse(uuid, deps) ← 记录依赖关系
    ↓
[callback(null, asset)]
```

### 2.5 Bundle 系统 (`bundle.ts`)

```typescript
class Bundle {
    name: string;                    // Bundle 名称
    config: IConfig;                 // 资源配置（uuid→路径映射）
    basePath: string;                // 基础路径
    
    load(paths, type, onProgress, onComplete);
    loadDir(dir, type, onProgress, onComplete);
    loadScene(sceneName, onProgress, onComplete);
    get(path, type);                 // 同步获取已加载资源
    getSceneInfo(sceneName);         // 获取场景信息
    release(path, type);             // 释放资源
}
```

**内置 Bundle**:
- `resources`: 内置资源包
- `main`: 主包
- `remote`: 远程资源包

### 2.6 依赖追踪 (`depend-util.ts`)

```typescript
class DependUtil {
    // uuid → 依赖的 uuid 列表
    parse(uuid, deps): string[];
    getDepends(uuid): string[];
    remove(uuid): void;
}
```

每个资源加载时，解析其依赖的其他资源 UUID，构建依赖图。

---

## 3. ReleaseManager 释放管理器 (`release-manager.ts`)

### 3.1 引用遍历机制

```typescript
// 从 Node 遍历找到所有引用的 Asset
function visitNode(node, deps: string[]): void {
    for (comp of node._components) visitComponent(comp, deps);
    for (child of node._children) visitNode(child, deps);
}

function visitComponent(comp, deps: string[]): void {
    for (propName of Object.getOwnPropertyNames(comp)) {
        const value = comp[propName];
        if (value instanceof Asset) visitAsset(value, deps);
        // 递归检查数组和字典
    }
}

function visitAsset(asset, deps: string[]): void {
    if (!asset._uuid) return;  // 跳过程序生成的资源
    deps.push(asset._uuid);
}
```

### 3.2 释放策略

```
ReleaseManager.tryRelease(asset)
    ↓
[检查引用计数]
    ├── _ref > 0 → 不释放
    └── _ref === 0
        ↓
    [检查场景引用]
        ├── 场景中仍有节点引用 → 不释放
        └── 无场景引用
            ↓
        [递减依赖资源的引用计数]
            ↓
        asset.destroy()
```

### 3.3 自动释放机制

场景切换时：
1. 遍历旧场景中所有节点的组件
2. 收集所有引用的 Asset UUID
3. 对每个 Asset 执行 `decRef()`
4. 若 `scene.autoReleaseAssets === true`，强制释放场景静态引用的资源

---

## 4. Native 资源桥接详解

### 4.1 Texture2D 的 Native 桥接

```
[JS 侧] Texture2D._native = ".png"
    ↓
AssetManager 检测 _nativeDep → 下载 .png 文件
    ↓
[Web] 创建 HTMLImageElement / ImageBitmap
[Native] 调用 jsb.Downloader 下载文件
    ↓
[Native] C++ 侧创建 gfx::Texture
    ↓
赋值到 Texture2D._nativeAsset (即 _file)
    ↓
Texture2D._gfxTexture → 引用 native 对象
```

### 4.2 Mesh 的 Native 桥接

```
[JS 侧] Mesh._native = ".bin"
    ↓
下载二进制数据 → ArrayBuffer
    ↓
[Native] C++ 侧创建 gfx::Buffer + gfx::InputAssembler
    ↓
赋值到 Mesh._nativeAsset
```

### 4.3 资源类型的 Native 处理模式

| 资源类型 | _native 扩展名 | Native 对象 |
|----------|---------------|-------------|
| Texture2D | .png/.pvr/.astc/.etc2 | gfx.Texture |
| Mesh | .bin | gfx.Buffer + IA |
| Audio | .mp3/.wav | AudioBuffer |
| AnimationClip | (无native) | 纯 JS |
| Material | (无native) | 纯 JS → gfx.Material |
| Prefab | (无native) | 纯 JS |

### 4.4 JSB 环境下的资源生命周期

```
[JSb] JS Asset 实例
    ↕ 双向引用
[JSB] C++ Native Asset 实例
```

- JS 对象通过 `_nativeAsset` / `_file` 持有 native 对象引用
- Native 对象通过 jsb 桥接持有 JS 对象的弱引用
- JS 对象 GC 时，native 对象通过 `destruct()` 释放
- 跨层数据传递通过 `_tempFloatArray` (Float32Array) 共享内存

---

## 5. C++ 化设计关键考量

### 5.1 可 Native 化的部分

| 子系统 | 可行性 | 说明 |
|--------|--------|------|
| 引用计数系统 | ✅ 高 | 简单整数加减 |
| Asset 基类核心属性 | ✅ 高 | uuid/native/ref |
| Pipeline 执行框架 | ✅ 高 | 标准管线模式 |
| Downloader | ✅ 高 | 已有 C++ 实现（jsb.Downloader） |
| Bundle 配置管理 | ✅ 高 | JSON 配置解析 |
| 依赖图 | ✅ 高 | UUID 索引 + 图遍历 |
| ReleaseManager | ✅ 高 | 引用遍历 + 释放策略 |

### 5.2 需要 JS 桥接的部分

| 子系统 | 原因 |
|--------|------|
| 反序列化 | 依赖 CCClass 反射 |
| 用户脚本资源引用 | JS 组件中的 Asset 属性 |
| Prefab 解析 | 依赖 JS Node/Component 创建 |
| 场景资源引用追踪 | 需要遍历 JS 组件属性 |

### 5.3 建议 C++ 资产系统架构

```
C++ AssetManager
    ├── C++ Pipeline (download → parse → init)
    ├── C++ RefCountedAsset 基类
    ├── C++ Bundle 管理
    ├── C++ 依赖图
    └── C++ ReleaseManager
        ↕ 桥接
JS AssetManager
    ├── JS 反序列化
    ├── JS 用户脚本属性引用
    └── JS 资源事件通知
```

**核心策略**:
1. **内置资源** (Texture2D/Mesh/Audio): C++ 全权管理，JS 仅持有轻量代理
2. **逻辑资源** (Prefab/AnimationClip): JS 管理，C++ 不参与
3. **引用计数统一**: C++ 侧维护全局引用计数，JS 侧的 addRef/decRef 通过桥接同步
4. **释放策略下沉**: ReleaseManager 核心逻辑 C++ 化，JS 仅提供引用遍历回调
