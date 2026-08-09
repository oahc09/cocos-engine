# D3D12MemoryAllocator 阶段 A 接入计划

## 目标

在 `gfx-d3d12` 后端创建全局 `D3D12MA::Allocator` 实例并纳入设备生命周期。本阶段**不迁移任何资源分配**（17 处 `CreateCommittedResource` 保持不动），仅建立基础设施，确保对渲染零影响。

## 1. Vendor 源文件（锁定 v3.2.0）

从 `GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator` 的 v3.2.0 tag 获取两个文件，复制到 `native/cocos/renderer/gfx-d3d12/`：

- `include/D3D12MemAlloc.h` → `D3D12MemAlloc.h`
- `src/D3D12MemAlloc.cpp` → `D3D12MemAlloc.cpp`

获取方式：浅克隆 tag 后复制（需要网络，执行时会请求授权）；若网络不可用则由用户手动放置这两个文件。

同时在文件头注释之外保留原始许可信息（MIT，文件自带）。

## 2. CMake 注册

[native/CMakeLists.txt](file:///d:/cocos_custome/cocos-engine/native/CMakeLists.txt) 第 1978 行起的 `if(CC_USE_D3D12)` 源码块，按字母序在 `D3D12InputAssembler.cpp` 之后插入：

```cmake
cocos/renderer/gfx-d3d12/D3D12MemAlloc.h
cocos/renderer/gfx-d3d12/D3D12MemAlloc.cpp
```

已确认这是唯一列出 gfx-d3d12 源文件的构建清单（无 vcxproj/其他 CMake 引用）。链接库 `d3d12 dxgi dxguid d3dcompiler`（3647 行）已满足 D3D12MA 依赖，无需新增。

## 3. D3D12Device 集成

### D3D12Device.h
- 前向声明 `namespace D3D12MA { class Allocator; }`（不向公共头文件暴露 D3D12MemAlloc.h）
- 新增公共访问器：`D3D12MA::Allocator *getMemoryAllocator() const;`

### D3D12Device.cpp
- `#include "D3D12MemAlloc.h"`
- `Impl`（约 165 行）新增两个成员：
  - `Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;`（当前 adapter 是 doInit 的局部变量，D3D12MA 需要它做 budget 查询）
  - `D3D12MA::ComPtr<D3D12MA::Allocator> memoryAllocator;`
- **创建**（doInit，约 2109 行 `D3D12CreateDevice` 块之后）：
  - 选中 `bestAdapter` 成功路径：存入 `_impl->adapter`；WARP 回退路径 adapter 保持 null（D3D12MA 允许 `pAdapter = nullptr`）
  - `_impl->d3dDevice` 确认非空后创建：

```cpp
D3D12MA::ALLOCATOR_DESC allocDesc{};
allocDesc.pDevice = _impl->d3dDevice.Get();
allocDesc.pAdapter = _impl->adapter.Get();   // WARP 时为 nullptr
allocDesc.PreferredBlockSize = 0;              // 默认块大小
allocDesc.Flags = 0;                           // 阶段 A 不开启发式，阶段 B 按实测调
D3D12MA::CreateAllocator(&allocDesc, &allocPtr);
```

  - 失败仅 `CC_LOG_ERROR` 并继续（allocator 为 null 时后续阶段自动回退 committed 路径，不阻塞设备初始化）
- **销毁**（doDestroy，第 474 行 `_impl->d3dDevice.Reset()` 之前）：`_impl->memoryAllocator.Reset();`——必须在 device 释放前，且在所有 GPU 资源释放后（阶段 A 无分配，顺序风险为零；阶段 B 起依赖此顺序约定）

## 4. 静态契约测试

[D3D12HotPathStaticTest.py](file:///d:/cocos_custome/cocos-engine/native/cocos/renderer/gfx-d3d12/D3D12HotPathStaticTest.py) 新增测试并注册到 main：

- `test_d3d12ma_allocator_lifecycle`：
  - `D3D12Device.cpp` 包含 `#include "D3D12MemAlloc.h"` 与 `D3D12MA::CreateAllocator`
  - `doInit` 中 allocator 创建出现在 `D3D12CreateDevice` 之后
  - `doDestroy` 中 `memoryAllocator.Reset()` 出现在 `d3dDevice.Reset()` 之前
- 校验 vendor 文件存在于 gfx-d3d12 目录（`D3D12MemAlloc.h` / `D3D12MemAlloc.cpp`）

## 5. 验证

- 运行 `python native/cocos/renderer/gfx-d3d12/D3D12HotPathStaticTest.py`，全部通过
- 编译验证（由用户在本地 VS 环境执行）：预期无新增错误；vendor 文件可能产生警告，引擎未启用 /WX，可接受
- 运行验证（用户）：启动任意场景，确认启动日志出现 adapter 选择信息且无 D3D12MA 相关报错；画面与接入前一致（本阶段无任何分配路径变化）

## 假设与边界

- 不使用 Agility SDK 特性（`CreateResource3`、GPU upload heaps），标准 Windows SDK 即可编译
- D3D12MA 默认 assert 使用标准 `assert()`，无需映射到 CC_ASSERT
- 阶段 A 完成后不删除现有 `sharedUploadPool` 等实验代码（阶段 D 再处理）
- tag 名以仓库实际为准（预期为 `v3.2.0`），克隆失败时回退到 master 并在注释中记录 commit hash
