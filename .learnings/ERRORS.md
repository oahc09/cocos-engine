# Error Log

## [ERR-20260523-001] brv-query-unavailable

**Logged**: 2026-05-23T19:30:56+08:00
**Priority**: medium
**Status**: pending
**Area**: tooling

### Summary
ByteRover query could not be used in this workspace session.

### Details
Running `brv query ...` failed because `brv` was not on PATH. Running the npm shim at `%APPDATA%\npm\brv.cmd` first failed with access denied inside the sandbox, then succeeded outside the sandbox but reported no provider connected. Attempting `brv providers connect byterover` reported that authentication is required.

### Suggested Action
Authenticate ByteRover with `brv login` or ensure a provider is connected before relying on `brv-query` project context.

### Metadata
- Source: command_failure
- Related Files: AGENTS.md
- Tags: byterover, tooling, context

---

## [ERR-20260524-006] msbuild-cl-task-output-pipe-oom

**Logged**: 2026-05-24T16:30:00+08:00
**Priority**: medium
**Status**: pending
**Area**: build

### Summary
`cmake --build build --config Debug --target cocos_engine` reached compilation but failed inside the MSBuild `CL` task output pipe with `System.OutOfMemoryException`.

### Details
The failure did not report a C++ compile error. MSBuild crashed while processing compiler output: `TrackedVCToolTask.SarifToolOutputPipe.ProcessHeader/ProcessMessage`.

### Suggested Action
Retry large Visual Studio builds with lower parallelism, or build only the touched translation unit/target when possible before treating this as a source regression.

### Metadata
- Source: command_failure
- Related Files: native/cocos/renderer/pipeline/PipelineUBO.cpp
- Tags: msbuild, cl, oom, d3d12-shadow

---

## [ERR-20260524-006] powershell-rg-regex-quote

**Logged**: 2026-05-24T14:30:00+08:00
**Priority**: low
**Status**: resolved
**Area**: tooling

### Summary
PowerShell treated part of an `rg` regex as a command because the pattern used escaped double quotes inside a double-quoted string.

### Details
`rg -n "chunkIndex=\"(62[0-8]|...)\"|..."` failed with `62[0-8]` not recognized. Re-running with a single-quoted PowerShell string fixed the search.

### Suggested Action
Use single quotes for `rg` patterns containing regex alternation and embedded double quotes in PowerShell.

### Metadata
- Source: command_failure
- Related Files: AI/analysics/d3d12-shadow.xml
- Tags: powershell, rg, quoting

---

## [ERR-20260523-003] powershell-regex-quote-pipeline

**Logged**: 2026-05-23T21:20:00+08:00
**Priority**: low
**Status**: pending
**Area**: tooling

### Summary
PowerShell treated part of a regex as a command when filtering XML chunks after an `rg` pipeline.

### Details
The command used nested double quotes around `chunkIndex=\"(...)\"` inside a PowerShell string. The quoting broke before `Select-String`, and PowerShell attempted to execute `138[0-9]` as a command.

### Suggested Action
For XML chunk filtering in PowerShell, prefer single-quoted patterns or avoid the extra pipe and use `Get-Content | Select-String -Pattern 'chunkIndex="(138[0-9]|...)'`.

### Metadata
- Source: command_failure
- Related Files: AI/analysics/d3d12-shadow.xml
- Tags: powershell, regex, renderdoc

---

## [ERR-20260523-002] diff-check-crlf-noise

**Logged**: 2026-05-23T19:43:00+08:00
**Priority**: low
**Status**: pending
**Area**: tooling

### Summary
`git diff --check` reports CRLF endings in `D3D12Framebuffer.cpp` as trailing whitespace.

### Details
The file already participates in a CRLF-style diff against `HEAD`. Normalizing the whole file to LF makes the whitespace check pass but creates broad line-ending churn. For targeted D3D12 fixes, keep the local file style and validate with build output plus `git diff --ignore-space-at-eol` for semantic review.

### Suggested Action
Handle `D3D12Framebuffer.cpp` line endings in a separate cleanup if the repository wants `git diff --check` to pass for that file.

### Metadata
- Source: command_failure
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12Framebuffer.cpp
- Tags: git, line-endings, d3d12

---

## [ERR-20260524-001] cmake-build-timeout

**Logged**: 2026-05-24T00:00:00+08:00
**Priority**: low
**Status**: pending
**Area**: tooling

### Summary
`cmake --build build --config Debug --target cocos_engine --parallel 1` exceeded the default 120s tool timeout.

### Details
The build command timed out before returning success or failure, so it should be rerun with a longer timeout before drawing conclusions about the patch.

### Suggested Action
Use a longer command timeout for `cocos_engine` verification builds in this workspace, especially after touching renderer files.

### Metadata
- Source: command_failure
- Related Files: native/cocos/renderer/pipeline/shadow/ShadowFlow.cpp
- Tags: cmake, build, timeout

---

## [ERR-20260524-002] d3d12-shader-regex-include

**Logged**: 2026-05-24T12:55:00+08:00
**Priority**: low
**Status**: resolved
**Area**: build

### Summary
Removing D3D12 shadow-depth regex fixups also removed `<regex>`, but `D3D12Shader.cpp` still uses `std::regex` for entry point candidate scanning.

### Details
`cmake --build build/d3d12-poc --config Release` failed with `std::regex` and `std::sregex_iterator` not found at `D3D12Shader.cpp:104`. Restoring `<regex>` fixed the build.

### Suggested Action
When deleting a regex-based helper, search the whole file for `std::regex` before removing the header.

### Metadata
- Source: command_failure
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12Shader.cpp
- Tags: cmake, d3d12, include

---

## [ERR-20260524-003] root-getpipeline-runtime-type

**Logged**: 2026-05-24T13:20:00+08:00
**Priority**: low
**Status**: resolved
**Area**: build

### Summary
`Root::getPipeline()` returns `render::PipelineRuntime *`, not `pipeline::RenderPipeline *`.

### Details
`cmake --build build/d3d12-poc --config Release` failed after adding scene macro helpers with parameter type `pipeline::RenderPipeline *`. Changing the helper parameter to `render::PipelineRuntime *` fixed the build.

### Suggested Action
When editing native scene code that uses `Root::getPipeline()`, type helpers against `render::PipelineRuntime *` unless the call site already has a concrete `pipeline::RenderPipeline *`.

### Metadata
- Source: command_failure
- Related Files: native/cocos/scene/Shadow.cpp, native/cocos/scene/DirectionalLight.cpp
- Tags: cmake, pipeline, type

---

## [ERR-20260524-004] brv-query-command-unavailable

**Logged**: 2026-05-24T00:00:00+08:00
**Priority**: low
**Status**: pending
**Area**: tooling

### Summary
ByteRover context lookup could not run from this PowerShell session.

### Details
`brv query "cocos-engine shadowmap RenderDoc rdc depth write gles3"` failed because `brv` was not recognized. Retrying via `$env:APPDATA\npm\brv.cmd` failed with `ResourceUnavailable` / `拒绝访问`.

### Suggested Action
Use the approved ByteRover command path only after the local npm shim permissions are fixed, or query through another configured MCP surface if available.

### Metadata
- Source: command_failure
- Related Files: AGENTS.md
- Tags: brv, byterover, powershell, permissions

---

## [ERR-20260524-005] brv-query-provider-not-connected

**Logged**: 2026-05-24T14:00:00+08:00
**Priority**: low
**Status**: pending
**Area**: tooling

### Summary
ByteRover query can start through the npm shim when run outside the sandbox, but no provider is connected.

### Details
`& "$env:APPDATA\npm\brv.cmd" query "D3D12 backend gfx-d3d12 setup notes implementation known issues"` returned `No provider connected. Run "brv providers connect byterover" to use the free built-in provider, or connect another provider.`

### Suggested Action
Connect the ByteRover provider before relying on `brv query` for project context.

### Metadata
- Source: command_failure
- Related Files: AGENTS.md
- Tags: brv, byterover, provider

---
