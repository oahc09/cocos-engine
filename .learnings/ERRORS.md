# Error Log

## [ERR-20260606-001] concurrent-header-edit-during-build

**Logged**: 2026-06-06T00:00:00+08:00
**Priority**: low
**Status**: resolved
**Area**: build

### Summary
A Release build briefly failed while an unrelated untracked header was being edited concurrently.

### Details
`Engine.cpp` included `CocosTracer.h`; the compiler observed an older `scene/Node.h` include, while the file read immediately afterward already contained the valid `core/scene-graph/Node.h` path. Re-running the same build succeeded.

### Suggested Action
When a build error references a concurrently edited untracked file, re-read the file and retry before attributing the failure to the current backend changes.

### Metadata
- Source: command_failure
- Related Files: native/cocos/engine/CocosTracer.h
- Tags: cmake, concurrent-edit, transient

---

## [ERR-20260712-005] oversized-apply-patch-context-mismatch

**Logged**: 2026-07-12T13:10:00+08:00
**Priority**: low
**Status**: resolved
**Area**: backend

### Summary
One oversized patch mixing a helper insertion with many repeated descriptor-handle replacements failed context verification.

### Error
```
apply_patch verification failed: Failed to find expected lines in D3D12DescriptorSet.cpp
```

### Context
- Operation: add D3D12 descriptor staging-heap recovery and replace all cached handle uses.
- The patch was rejected atomically, so the production file remained unchanged.

### Suggested Fix
Split structural insertions from repetitive mechanical replacements and verify each step independently.

### Metadata
- Reproducible: yes
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12DescriptorSet.cpp

### Resolution
- **Resolved**: 2026-07-12T13:10:00+08:00
- **Notes**: Continued with small atomic patches.

---

## [ERR-20260712-004] recursive-msbuild-discovery-timeout

**Logged**: 2026-07-12T11:15:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### Summary
Recursively scanning the complete Visual Studio installation tree for `MSBuild.exe` exceeded the command timeout.

### Error
```
Exit code: 124
command timed out after 20030 milliseconds
```

### Context
- Operation: locate MSBuild for the D3D12 unit-test build.

### Suggested Fix
Use `vswhere.exe -find MSBuild\\**\\Bin\\MSBuild.exe` or check known Visual Studio edition paths directly.

### Metadata
- Reproducible: yes
- Related Files: build/unit-test-d3d12/src/CocosTest.vcxproj

### Resolution
- **Resolved**: 2026-07-12T11:15:00+08:00
- **Notes**: Switched to fixed-path and vswhere discovery.

---

## [ERR-20260712-003] rg-no-match-nonzero

**Logged**: 2026-07-12T11:10:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### Summary
An exploratory `rg` command with no matches returned exit code 1 and made an otherwise successful compound read appear failed.

### Error
```
Exit code: 1
```

### Context
- Operation: search for existing `d3d12sdklayers` usage after successfully locating `waitForGpu`.
- For ripgrep, exit code 1 means no matches rather than an execution error.

### Suggested Fix
For optional searches, handle `$LASTEXITCODE -eq 1` explicitly or append a PowerShell fallback that returns success.

### Metadata
- Reproducible: yes
- Related Files: native/tests/unit-test/src/d3d12_render_pass_test.cpp

### Resolution
- **Resolved**: 2026-07-12T11:10:00+08:00
- **Notes**: Subsequent optional searches allow the no-match status.

---

## [ERR-20260712-002] rg-windows-glob-literal-path

**Logged**: 2026-07-12T11:00:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### Summary
Passing a Windows wildcard path directly to `rg` produced an invalid-path error.

### Error
```
rg: native/cocos/renderer/gfx-d3d12/D3D12Framebuffer.*: 文件名、目录名或卷标语法不正确。 (os error 123)
```

### Context
- Operation: inspect D3D12 framebuffer and render-target clear paths.
- PowerShell did not expand the wildcard into file arguments before `rg` processed it.

### Suggested Fix
Use `rg -g 'D3D12Framebuffer.*' <directory>` or pass explicit file paths on Windows.

### Metadata
- Reproducible: yes
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12Framebuffer.cpp

### Resolution
- **Resolved**: 2026-07-12T11:00:00+08:00
- **Notes**: Continued with explicit paths and directory-scoped glob filters.

---

## [ERR-20260711-006] oversized-diff-and-regex-quoting

**Logged**: 2026-07-11T21:00:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### Summary
A combined repository-inspection command produced an oversized truncated diff and a later `rg` expression lost its quoted string under PowerShell parsing.

### Error
```
Warning: truncated output
rg: regex parse error: unclosed group
```

### Context
- Operation: inspect the D3D12 shader cache migration implementation and tests.
- Multiple high-volume commands were grouped into one tool call, and a regex containing escaped quotes crossed JavaScript, PowerShell, and ripgrep parsing layers.

### Suggested Fix
Query narrow line ranges separately and prefer fixed-string searches (`rg -F`) for C++ string literals.

### Metadata
- Reproducible: yes
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12Shader.cpp

### Resolution
- **Resolved**: 2026-07-11T21:00:00+08:00
- **Notes**: Switched to bounded `Get-Content` ranges and separate fixed-string queries.

---

## [ERR-20260711-007] recursive-msbuild-search-timeout

**Logged**: 2026-07-11T21:10:00+08:00
**Priority**: low
**Status**: resolved
**Area**: build

### Summary
Recursively scanning the complete Visual Studio installation for `MSBuild.exe` exceeded the command timeout.

### Error
```
command timed out after 20024 milliseconds
```

### Context
- Operation: locate MSBuild for direct project compilation after the generated CMake tree could not regenerate.

### Suggested Fix
Use Visual Studio Installer's `vswhere.exe -find MSBuild\\**\\Bin\\MSBuild.exe` instead of filesystem recursion.

### Metadata
- Reproducible: yes
- Related Files: build/unit-test-d3d12/cocos_engine.vcxproj

### Resolution
- **Resolved**: 2026-07-11T21:10:00+08:00
- **Notes**: Switched to the Visual Studio `vswhere` query.

---

## [ERR-20260711-008] unsupported-gtest-brief-false-green

**Logged**: 2026-07-11T21:24:00+08:00
**Priority**: medium
**Status**: resolved
**Area**: tests

### Summary
The vendored GoogleTest does not support `--gtest_brief`; it printed help and exited zero without running the requested repeated tests.

### Error
```
GoogleTest help output; no [ RUN ] or [ PASSED ] records; exit code 0
```

### Context
- Operation: repeat the D3D12 cache persistence concurrency tests 50 times.
- A zero exit code was insufficient because argument parsing treated the unknown flag as a help request.

### Suggested Fix
Use only flags listed by the vendored GoogleTest, capture output in PowerShell, return its tail, and preserve `$LASTEXITCODE`.

### Metadata
- Reproducible: yes
- Related Files: build/unit-test-d3d12/src/Debug/CocosTest.exe

### Resolution
- **Resolved**: 2026-07-11T21:24:00+08:00
- **Notes**: Removed `--gtest_brief` and required explicit test-run/pass markers.

---

## [ERR-20260711-009] release-clcompile-time-budget

**Logged**: 2026-07-11T21:35:00+08:00
**Priority**: low
**Status**: resolved
**Area**: build

### Summary
The first Release `ClCompile` pass exceeded the 120-second command timeout without emitting a compiler error.

### Error
```
command timed out after 124054 milliseconds
```

### Context
- Operation: direct MSBuild Release verification of the D3D12 engine project.
- Debug incremental compilation had already succeeded; Release configuration required a larger first-pass budget.

### Suggested Fix
Use a longer total timeout and poll the yielded command in short intervals so progress communication remains responsive.

### Metadata
- Reproducible: unknown
- Related Files: build/unit-test-d3d12/cocos_engine.vcxproj

### Resolution
- **Resolved**: 2026-07-11T21:35:00+08:00
- **Notes**: Retried with a larger total budget and short polling intervals.

---

## [ERR-20260711-010] direct-clcompile-skips-object-directory-setup

**Logged**: 2026-07-11T21:37:00+08:00
**Priority**: low
**Status**: resolved
**Area**: build

### Summary
Directly invoking the Release `ClCompile` target in a unit-test tree that had never prepared Release directories failed before compiling the changed D3D12 files.

### Error
```
error C1083: cannot open compiler generated .obj file: No such file or directory
```

### Context
- `ClCompile` intentionally bypassed CMake regeneration, but also bypassed the directory-preparation targets required by a fresh configuration.

### Suggested Fix
Use an already prepared Release generation tree for source verification, or run the complete preparation target chain when regeneration is available.

### Metadata
- Reproducible: yes
- Related Files: build/unit-test-d3d12/cocos_engine.vcxproj

### Resolution
- **Resolved**: 2026-07-11T21:37:00+08:00
- **Notes**: Switched Release verification to the main generated engine project with existing object directories.

---

## [ERR-20260712-001] javascript-template-powershell-backtick

**Logged**: 2026-07-12T07:55:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### Summary
A PowerShell tab escape using a backtick terminated the surrounding JavaScript template literal before the command could run.

### Error
```
SyntaxError: Invalid or unexpected token
```

### Context
- Operation: stream first/last timestamps from the large D3D12 performance log.
- The command crossed JavaScript-template and PowerShell parsing layers.

### Suggested Fix
Avoid PowerShell backtick escapes inside JavaScript template literals; extract timestamps with a regular expression instead.

### Metadata
- Reproducible: yes
- Related Files: C:/Users/caosh/Desktop/preflog.txt

### Resolution
- **Resolved**: 2026-07-12T07:55:00+08:00
- **Notes**: Replaced tab splitting with regex timestamp extraction.

---

## [ERR-20260711-003] cpp-helper-declaration-order

**Logged**: 2026-07-11T19:46:00+08:00
**Priority**: low
**Status**: resolved
**Area**: backend

### Summary
A new DXBC canonicalization helper called a container-inspection function before that function was declared.

### Error
```
D3D12Shader.cpp: error C3861: 'inspectDXBCContainer': identifier not found
```

### Context
- Operation: compile the D3D12 Shader scheduling and cache fix.
- The helper implementation was correct but placed earlier in the anonymous namespace than its dependency.

### Suggested Fix
Add a forward declaration or order anonymous-namespace helpers by dependency before the first compile.

### Metadata
- Reproducible: yes
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12Shader.cpp

### Resolution
- **Resolved**: 2026-07-11T19:47:00+08:00
- **Notes**: Added the missing forward declaration and reran compilation.

---

## [ERR-20260711-004] unit-test-cmake-regeneration-incompatible

**Logged**: 2026-07-11T19:56:00+08:00
**Priority**: low
**Status**: resolved
**Area**: build

### Summary
Building the existing D3D12 unit-test solution through the aggregate `Build` target forced a CMake 4.3 regeneration that rejected the vendored googletest minimum-version declaration.

### Error
```
CMake Error: Compatibility with CMake < 3.5 has been removed
```

### Context
- The engine object compilation had already succeeded; only the generated test solution's regeneration step failed.
- The test executable initially remained linked to a stale `cocos_engine.lib`, which made the DXBC determinism test exercise the old implementation.

### Suggested Fix
For source-only verification in an existing generated Visual Studio tree, invoke MSBuild `_Lib` on `cocos_engine.vcxproj` and `_Link` on `CocosTest.vcxproj` with project references disabled. Regenerate only after updating the vendored CMake compatibility declarations.

### Metadata
- Reproducible: yes
- Related Files: build/unit-test-d3d12/cocos_engine.vcxproj, build/unit-test-d3d12/src/CocosTest.vcxproj

### Resolution
- **Resolved**: 2026-07-11T19:58:00+08:00
- **Notes**: Rebuilt the updated static library with `_Lib`, relinked with `_Link`, then passed all D3D12 tests.

---

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

## [ERR-20260605-001] debug-build-timeout-retry

**Logged**: 2026-06-05T00:00:00+08:00
**Priority**: low
**Status**: resolved
**Area**: build

### Summary
`cmake --build build --config Debug --target cocos_engine` may exceed a 120s command timeout even when the build is healthy.

### Details
The first Debug build attempt timed out after 124s without returning compiler diagnostics. Re-running the same command with a 300s timeout completed successfully in the warmed build and produced `build\Debug\cocos_engine.lib`.

### Suggested Action
Use a longer timeout for Debug engine builds in this workspace before treating a timeout as a build failure.

### Metadata
- Source: command_failure
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12Device.cpp
- Tags: cmake, debug-build, timeout

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
[ERR-20260606-002] multi-file-cleanup-patch-context-mismatch

**Logged**: 2026-06-06
**Context**: D3D12 backend cleanup
**Error**: A multi-file apply_patch failed because the expected D3D12CommandBuffer.cpp context no longer matched.
**Resolution**: Re-read the exact local snippets and apply smaller per-file patches. The failed patch was atomic and changed no files.
[ERR-20260606-003] ripgrep-windows-wildcard

**Logged**: 2026-06-06
**Context**: D3D12 static implementation scan
**Error**: `rg ... native/cocos/renderer/gfx-d3d12/*.cpp` failed because PowerShell/Windows did not expand the path wildcard for ripgrep.
**Resolution**: Use `rg -g "*.cpp" ... native/cocos/renderer/gfx-d3d12`.
[ERR-20260606-004] d3d12-format-mapper-ambiguous

**Logged**: 2026-06-06
**Context**: Release build after sharing the texture format mapper with D3D12Device
**Error**: `toD3D12Format` was still defined in an anonymous namespace while a public namespace declaration was added, making calls in D3D12Texture.cpp ambiguous.
**Resolution**: Rename the anonymous implementation and expose a namespace-level forwarding function.
[ERR-20260606-005] renderdoc-capture-discovery-restrictions

**Logged**: 2026-06-06
**Context**: Inspecting an opened RenderDoc capture
**Error**: Win32_Process command-line inspection was denied, a recursive D-drive RDC search timed out, and the sandboxed Codex executable could not query MCP configuration.
**Resolution**: Inspect RenderDoc process ports and search only likely capture directories; use the local RenderDoc Python/API tooling when the capture path is found.
[ERR-20260606-006] renderdoc-mcp-parallel-request-race

**Logged**: 2026-06-06
**Context**: RenderDoc EID inspection through the file-based MCP bridge
**Error**: Parallel bridge calls raced on the single shared request/response JSON files, causing missing response files.
**Resolution**: Send RenderDoc bridge calls strictly sequentially.
[ERR-20260606-007] renderdoc-mcp-pillow-unavailable

**Logged**: 2026-06-06
**Context**: Exporting RenderDoc texture data for visual inspection
**Error**: The renderdoc-mcp virtual environment does not include Pillow.
**Resolution**: Export raw RGBA bytes with the bridge environment and render them using the available Node image tooling.
[ERR-20260606-008] node-repl-windows-sandbox-exit

**Logged**: 2026-06-06
**Context**: Rendering exported RenderDoc RGBA data
**Error**: The Node REPL kernel exited during Windows sandbox setup refresh.
**Resolution**: Use the bundled workspace Python runtime and image libraries for diagnostic image conversion.
[ERR-20260606-009] renderdoc-convert-argument-order

**Logged**: 2026-06-06
**Context**: Converting the current RDC to XML
**Error**: Used `-f` as an output format flag, but RenderDoc defines it as the input filename.
**Resolution**: Use `-f <capture> -c xml -o <output>`.

[ERR-20260606-010] renderdoc-bridge-generic-call

**Logged**: 2026-06-06
**Context**: Inspecting texture mip contents through the RenderDoc MCP bridge
**Error**: Assumed capture operations were direct `RenderDocBridge` methods, but the client exposes only `call(method, params)`.
**Resolution**: Inspect `RenderDocBridge.call` and dispatch extension methods through that generic API.

[ERR-20260606-011] renderdoc-bridge-timeout-constructor

**Logged**: 2026-06-06
**Context**: Reopening a capture through the RenderDoc MCP bridge
**Error**: Passed `timeout` to `RenderDocBridge.__init__`, which only accepts compatibility host and port parameters.
**Resolution**: Construct with no arguments and assign `bridge.timeout` when a custom timeout is needed.

[ERR-20260606-012] renderdoc-open-capture-parameter

**Logged**: 2026-06-06
**Context**: Reopening a capture through the RenderDoc MCP bridge
**Error**: Used `filename` for `open_capture`; the extension requires `capture_path`.
**Resolution**: Read the extension method schema before dispatching bridge calls and pass `capture_path`.

[ERR-20260606-013] release-build-compiler-heap

**Logged**: 2026-06-06
**Context**: Release verification after the D3D12 sampler fix
**Error**: The default parallel MSBuild exhausted compiler heap space with C1060 errors in unrelated translation units.
**Resolution**: Re-run the Release target with `/m:1` to limit MSBuild concurrency.

[ERR-20260606-014] qrenderdoc-not-on-path

**Logged**: 2026-06-06
**Context**: Launching an isolated RenderDoc UI Python pixel-debug session
**Error**: `Get-Command qrenderdoc.exe` failed because the RenderDoc install directory is not on PATH.
**Resolution**: Reuse the executable path from the running process: `D:\Program Files\RenderDoc\qrenderdoc.exe`.

[ERR-20260606-015] powershell-select-string-byte-encoding

**Logged**: 2026-06-06
**Context**: Searching the RenderDoc executable for command-line option strings
**Error**: PowerShell 7 `Select-String` does not accept `-Encoding Byte`.
**Resolution**: Use a binary strings utility or inspect RenderDoc's documented/GUI script entry points instead of treating the executable as byte-encoded text.

[ERR-20260606-016] broad-user-directory-recursion-timeout

**Logged**: 2026-06-06
**Context**: Locating the previously used RenderDoc MCP bridge
**Error**: Recursively enumerating all directories under the Windows user profile exceeded the command timeout.
**Resolution**: Search only likely tool roots such as `.codex`, `.agents`, and temporary tool directories with `rg --files`.

[ERR-20260606-017] renderdoc-runtime-injection-no-capture

**Logged**: 2026-06-06
**Context**: Capturing the already running D3D12 test scene after mip diagnostics
**Error**: `renderdoccmd inject` reported success, but neither scripted F12 input path produced a capture.
**Resolution**: Launch the executable through `renderdoccmd capture` from process start and trigger capture after the target scene has loaded.

[ERR-20260607-001] renderdoc-bridge-open-timeout

**Logged**: 2026-06-07
**Context**: Inspecting MSAA state in the latest D3D12 capture
**Error**: The RenderDoc MCP bridge timed out while opening `C:\Users\caosh\Desktop\d3d12.rdc`; no qrenderdoc process was available to service the file-based request.
**Resolution**: Check the qrenderdoc process before using the bridge, and use RenderDoc's headless Python replay API when the UI bridge is unavailable.

[ERR-20260607-002] powershell-rg-wildcard-path

**Logged**: 2026-06-07
**Context**: Searching D3D12 pipeline state files from PowerShell
**Error**: Passed wildcard file paths such as `GFXPipelineState.*` directly to `rg`; Windows treated them as invalid paths.
**Resolution**: Search the containing directories and constrain matches with `-g` when using `rg` from PowerShell.

[ERR-20260607-003] d3d12-unit-test-full-build-timeout

**Logged**: 2026-06-07
**Context**: Establishing the RED phase for the D3D12 render-pass regression test
**Error**: The first complete `CocosTest` build exceeded the two-minute command timeout while compiling the engine dependency.
**Resolution**: Compile the unit-test project's `ClCompile` target first for fast test-source feedback, then use the warmed incremental build for final verification.

[ERR-20260607-004] powershell-get-childitem-multiple-filters

**Logged**: 2026-06-07
**Context**: Detecting the repository package-manager lock file
**Error**: Passed an array to PowerShell `Get-ChildItem -Filter`, which only accepts one string pattern.
**Resolution**: Enumerate files once and filter names with `Where-Object` when matching multiple exact filenames.

[ERR-20260607-005] computer-use-readonly-spinbox

**Logged**: 2026-06-07
**Context**: Selecting a particle instance in RenderDoc's Mesh Viewer through Computer Use
**Error**: `set_value` failed because the RenderDoc instance spinbox exposed a read-only UI Automation value.
**Resolution**: Use the spinbox's visible increment/decrement buttons through coordinate clicks, then verify the displayed instance and table values.

[ERR-20260607-006] computer-use-stale-renderdoc-coordinate

**Logged**: 2026-06-07
**Context**: Horizontally scrolling RenderDoc's Mesh Viewer after the event list layout changed
**Error**: Reusing an old scrollbar coordinate selected a different draw event instead of moving the table.
**Resolution**: Refresh the window screenshot immediately before coordinate drags in RenderDoc and verify the selected EID after each layout-changing action.

[ERR-20260607-007] renderdoc-bridge-ui-buffer-id

**Logged**: 2026-06-07
**Context**: Reading EID 1322's instance buffer through the RenderDoc MCP bridge
**Error**: `get_buffer_contents` rejected both `ResourceId::5762` and `5762`, although the RenderDoc UI displayed Buffer 5762 in the input assembler.
**Resolution**: Do not assume the UI resource number is accepted by this bridge method; obtain the bridge-side buffer identifier from an API that enumerates vertex buffers, or inspect the data through Mesh Viewer.

[ERR-20260607-008] windows-perl-not-on-path

**Logged**: 2026-06-07
**Context**: Normalizing line endings after patching native PSO manager files
**Error**: `perl -0777 -pi ...` failed because Perl was not installed or not on PATH in the Windows workspace.
**Resolution**: Use PowerShell/.NET text normalization or repo-provided formatting tools instead of assuming Unix text utilities are available on Windows.

[ERR-20260607-009] node-repl-top-level-const-redeclare

**Logged**: 2026-06-07
**Context**: Reading RenderDoc Mesh Viewer state through Computer Use
**Error**: A reused top-level `const tree` declaration failed with `Identifier 'tree' has already been declared`.
**Resolution**: Wrap one-off Node REPL inspection code in a local `{ ... }` block or store reusable values on `globalThis`.

[ERR-20260607-010] powershell-select-object-range

**Logged**: 2026-06-07
**Context**: Reading a line slice from `cocos/particle/enum.ts`
**Error**: `Select-Object -Index 55..80` failed because PowerShell did not convert the unparenthesized range expression to an integer array for the parameter.
**Resolution**: Use `(Get-Content $path)[55..80]` or `Select-Object -Index (55..80)` for line slices.

[ERR-20260607-011] rg-invalid-extra-path

**Logged**: 2026-06-07
**Context**: Searching native renderer sources for shader attribute filtering
**Error**: Included a nonexistent path `native/cocos/rendering`, causing `rg` to exit with an error even though other matches were found.
**Resolution**: Verify directory names with `rg --files` or omit speculative extra paths when broad-searching repository modules.

[ERR-20260607-012] apply-patch-stale-context

**Logged**: 2026-06-07
**Context**: Adding D3D12 vertex-input reflection to `D3D12PipelineState.cpp`
**Error**: The first patch used an approximate helper-function signature and failed because the file used `mode` rather than the expected parameter name.
**Resolution**: Read the exact surrounding function headers before large patches, then split helper insertion and logic replacement into smaller patches.

[ERR-20260607-013] rg-windows-glob-path

**Logged**: 2026-06-07
**Context**: Searching D3D12 renderer logs on Windows
**Error**: `rg ... native/cocos/renderer/gfx-d3d12/D3D12*.cpp` failed because the glob-like path was passed as a literal invalid Windows path.
**Resolution**: Search the directory and filter by pattern, or use `rg -g "D3D12*.cpp" ... native/cocos/renderer/gfx-d3d12`.

[ERR-20260608-001] rg-powershell-file-glob-argument

**Logged**: 2026-06-08
**Context**: Verifying D3D12 buffer and command-buffer regression patterns on Windows
**Error**: `rg ... native/cocos/renderer/gfx-d3d12/D3D12Buffer.* ...` failed because PowerShell passed the wildcard-like file path as an invalid literal path.
**Resolution**: Use explicit file paths or search the directory with `--glob "D3D12Buffer.*"` when constraining ripgrep matches on Windows.

## [ERR-20260710-001] d3d12-build-timeout-too-short

**Logged**: 2026-07-10T00:00:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### Summary
The D3D12 incremental build was launched with a one-second timeout and was terminated before compiler output was returned.

### Error
```
command timed out after 5040 milliseconds
```

### Context
- Command: `cmake --build build/d3d12-poc --config Release --target cocos_engine --parallel 8`
- The timeout was a tool-call configuration mistake, not evidence of a compile failure.

### Suggested Fix
Use a long-running build call with an appropriate timeout, or resume a yielded build session when available.

### Metadata
- Reproducible: yes
- Related Files: build/d3d12-poc/CMakeCache.txt

### Resolution
- **Resolved**: 2026-07-10T00:00:00+08:00
- **Notes**: Re-run the existing incremental build with a sufficient timeout.

---

## [ERR-20260711-001] summarize-cli-not-installed

**Logged**: 2026-07-11T18:50:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### Summary
The optional summarize skill could not process the D3D12 performance log because its CLI dependency is not installed.

### Error
```
summarize CLI is not installed
```

### Context
- Input file: `C:/Users/caosh/Desktop/preflog.txt`
- Exact keyword aggregation does not require the external summarizer.

### Suggested Fix
Use local PowerShell parsing for structured renderer logs, or install the summarize CLI when semantic file summaries are needed.

### Metadata
- Reproducible: yes
- Related Files: C:/Users/caosh/Desktop/preflog.txt

### Resolution
- **Resolved**: 2026-07-11T18:50:00+08:00
- **Notes**: Continued with local keyword and numeric aggregation.

---

## [ERR-20260711-002] powershell-helper-alias-collision

**Logged**: 2026-07-11T18:52:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### Summary
A one-letter PowerShell helper named `H` collided with the built-in `Get-History` alias while parsing renderer logs.

### Error
```
Get-History: Cannot bind parameter 'Count'. Cannot convert value "stage" to type "System.Int32".
```

### Context
- Operation: parse structured key-value fields from the D3D12 performance log.

### Suggested Fix
Use descriptive helper names that cannot collide with PowerShell aliases, and parenthesize helper calls inside hashtable literals.

### Metadata
- Reproducible: yes
- Related Files: C:/Users/caosh/Desktop/preflog.txt

### Resolution
- **Resolved**: 2026-07-11T18:52:00+08:00
- **Notes**: Replaced one-letter helpers with `GetNumField`, `GetTextField`, and `GetQuotedField`.

---

## [ERR-20260711-005] nested-powershell-stdin-produced-no-output

**Logged**: 2026-07-11T20:46:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### Summary
A multiline aggregation script piped through a nested `powershell -Command -` process exited successfully but returned no statistics.

### Error
```
Exit code: 0
Output: empty
```

### Context
- Operation: stream and aggregate selected D3D12 performance-log events without loading the complete log into model context.
- The active shell was already PowerShell, so the nested stdin wrapper added unnecessary quoting and encoding risk.

### Suggested Fix
Execute the aggregation directly in the active PowerShell process, or save a reusable parser script with `apply_patch` when the command becomes large.

### Metadata
- Reproducible: unknown
- Related Files: C:/Users/caosh/Desktop/preflog.txt

### Resolution
- **Resolved**: 2026-07-11T20:47:00+08:00
- **Notes**: Re-ran the parser directly in the active shell.

---
## [ERR-20260713-001] repeated-rg-windows-glob-literal-path

**Logged**: 2026-07-13T22:40:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### Summary
Repeated a known Windows `rg` wildcard-path mistake while inspecting D3D12 descriptor files.

### Error
```
rg: native/cocos/renderer/gfx-d3d12/D3D12DescriptorSet.*: 文件名、目录名或卷标语法不正确。 (os error 123)
```

### Context
- PowerShell did not expand the wildcard path before passing it to `rg`.
- The preceding source inspection completed; only the secondary search failed.

### Suggested Fix
Use explicit paths or `rg -g 'D3D12DescriptorSet.*' native/cocos/renderer/gfx-d3d12` on Windows.

### Metadata
- Reproducible: yes
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12DescriptorSet.cpp
- See Also: ERR-20260712-002

### Resolution
- **Resolved**: 2026-07-13T22:40:00+08:00
- **Notes**: Switched to explicit paths.

---
