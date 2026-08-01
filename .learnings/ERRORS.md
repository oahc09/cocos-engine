## [ERR-20260729-002] stale-native-cmake-cache-mixed-module-roots

**Logged**: 2026-07-29
**Priority**: medium
**Status**: unresolved
**Area**: build

### Summary
`cmake --build native/build --config Debug --target cocos_engine` could not reconfigure because the existing cache points at CMake 3.30 modules while the active CMake is 4.3.

### Error
```
CMake Error: File D:/Program Files/CMake/share/cmake-3.30/Modules/CMakeSystem.cmake.in does not exist.
include could not find requested file:
  D:/Program Files/CMake/share/cmake-3.30/Modules/CMakeDetermineCompiler.cmake
```

### Context
- The build tree was regenerated automatically because its stamp was out of date.
- Reconfiguring or deleting the shared build directory was intentionally avoided during a read-only code review.
- Use the authoritative test-project solution or a fresh isolated build directory for later integration verification.

### Suggested Fix
Reconfigure an isolated build directory with one CMake installation, or build the known integration solution without forcing this stale cache to regenerate.

### Metadata
- Reproducible: yes
- Related Files: native/build/CMakeCache.txt

---

## [ERR-20260717-001] powershell_rg_glob

**Logged**: 2026-07-17T22:51:00+08:00
**Priority**: low
**Status**: resolved
**Area**: tests

### Summary
PowerShell did not expand `D3D12*.cpp` globs passed directly to `rg`.

### Error
```
rg: native/cocos/renderer/gfx-d3d12/D3D12*.cpp: 文件名、目录名或卷标语法不正确。 (os error 123)
```

### Context
The failed read-only search attempted to find D3D12 timestamp instrumentation.

### Resolution
Search the `gfx-d3d12` directory directly (or pass explicit files) instead of
using a PowerShell glob as an `rg` path argument.

### Metadata
- Reproducible: yes
- Related Files: native/cocos/renderer/gfx-d3d12
- Recurrence-Count: 6
- Last-Seen: 2026-07-29
- See Also: ERR-20260718-008, ERR-20260719-020

---

## [ERR-20260717-004] powershell_multiple_match

**Logged**: 2026-07-17T23:13:30+08:00
**Priority**: low
**Status**: resolved
**Area**: config

### Summary
PowerShell `Select-String` returned multiple CMake match line numbers.

### Error
```
System.Object[] does not contain a method named 'op_Subtraction'
```

### Resolution
Select the first matching line number explicitly before arithmetic.

### Metadata
- Reproducible: yes
- Related Files: native/CMakeLists.txt

---

## [ERR-20260718-005] wpr_cpu_policy_denied

**Logged**: 2026-07-18T00:41:00+08:00
**Priority**: low
**Status**: resolved
**Area**: tests

### Summary
Windows Performance Recorder CPU sampling is disabled by local system policy.

### Error
```
Failed to enable the policy to profile system performance.
Profile Id: CPU.Light.File
Error code: 0xc5585011
```

### Context
Attempted a read-only CPU sampling trace for `test-cases.exe` after the
renderer's own detailed timers proved intrusive.

### Resolution
Do not retry WPR CPU sampling in this environment. Use the existing
default-off renderer counters or obtain an explicitly authorized machine
policy change before relying on ETW CPU stacks.

### Metadata
- Reproducible: yes
- Related Files: d3d12_perf_records/round-71

---

## [ERR-20260717-003] apply_patch_context_drift

**Logged**: 2026-07-17T23:13:00+08:00
**Priority**: low
**Status**: resolved
**Area**: config

### Summary
The CMake insertion patch used an incomplete end-of-block context.

### Error
```
apply_patch verification failed: Failed to find expected lines
```

### Context
Adding a source-local Debug O2 list for the measured SceneCulling candidate.

### Resolution
Read the exact `RENDER_QUEUE_DEBUG_OPTIMIZED_SOURCES` neighborhood and insert
the independent block before its real enclosing `endif()`.

### Metadata
- Reproducible: no
- Related Files: native/CMakeLists.txt

---

## [ERR-20260717-002] pipeline_stage_path_assumption

**Logged**: 2026-07-17T23:02:00+08:00
**Priority**: low
**Status**: resolved
**Area**: tests

### Summary
Assumed forward-stage source paths that do not exist in this engine layout.

### Error
```
native/cocos/renderer/pipeline/ForwardStage.cpp: 系统找不到指定的文件。
```

### Context
Read-only source exploration before considering a new top-level frame timer.

### Resolution
Use `rg --files native/cocos/renderer` to establish actual source locations
before requesting function bodies.

### Metadata
- Reproducible: yes
- Related Files: native/cocos/renderer

---

## [ERR-20260718-001] d3d12_definition_header_path_assumption

**Logged**: 2026-07-18T00:00:00+08:00
**Priority**: low
**Status**: resolved
**Area**: tests

### Summary
A combined D3D12 diagnostics search included a nonexistent header path.

### Error
```
rg: native/cocos/renderer/gfx-d3d12/D3D12Def.h: 系统找不到指定的文件。
```

### Context
Read-only exploration of the compile-time performance-counter definition and
descriptor submission implementation.

### Resolution
Search only confirmed source files, or first enumerate an unfamiliar D3D12
directory with `rg --files` before adding a header path to a multi-file query.

### Metadata
- Reproducible: yes
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12Device.h

---

## [ERR-20260718-002] pending_buffer_fast_path_patch_context

**Logged**: 2026-07-18T06:30:00+08:00
**Priority**: low
**Status**: resolved
**Area**: backend

### Summary
A multi-file fast-path patch used an incomplete command-buffer call-site context.

### Error
```
apply_patch verification failed: Failed to find expected lines in D3D12CommandBuffer.cpp
```

### Context
Adding a device-owned pending-buffer flag after a diagnostic confirmed repeated
empty queue-drain calls in the D3D12 draw loop.

### Resolution
No files were changed by the failed atomic patch. Re-read every exact call
site and apply the header, device, and command-buffer edits as separate small
patches.

### Metadata
- Reproducible: no
- Related Files: native/cocos/renderer/gfx-d3d12/D3D12CommandBuffer.cpp

---

## [ERR-20260718-003] github_raw_stream_timeout

**Logged**: 2026-07-18T07:00:00+08:00
**Priority**: low
**Status**: resolved
**Area**: docs

### Summary
Streaming several GitHub raw source files through the shell did not return.

### Error
```
curl.exe raw.githubusercontent.com request produced no output for 70 seconds
and was terminated.
```

### Context
Read-only comparison of DiligentCore's D3D12 descriptor and command-context
implementation with the local renderer.

### Resolution
Use the GitHub file pages and browser source retrieval for individual files;
do not batch raw-host requests through a single shell pipeline.

### Metadata
- Reproducible: unknown
- Related Files: external DiligentCore GraphicsEngineD3D12 sources

---

## [ERR-20260718-004] round78_measurement_path_assumption

**Logged**: 2026-07-18T07:10:00+08:00
**Priority**: low
**Status**: resolved
**Area**: tests

### Summary
An inspection assumed an old nested measurement-file path that does not exist.

### Error
```
Get-Content failed because round-78 stores measurement files directly in the
round directory with label-prefixed filenames.
```

### Context
Read-only comparison of the script FPS result with a RenderDoc-observed rate.

### Resolution
List the round directory and select the label-prefixed measurement file before
reading historical performance samples.

### Metadata
- Reproducible: yes
- Related Files: d3d12_perf_records/round-78

---

## [ERR-20260718-005] static_test_not_registered

**Logged**: 2026-07-18T07:20:00+08:00
**Priority**: low
**Status**: resolved
**Area**: tests

### Summary
A new static test was defined but not registered in the script's explicit test list.

### Error
```
python native\\tests\\unit-test\\d3d12_perf_static_test.py passed without
running test_local_descriptor_diagnostic_separates_dynamic_cbvs_from_static_signatures.
```

### Context
Test-first implementation of the local descriptor composition diagnostic.

### Resolution
Whenever this file receives a test function, add it to the `tests` list before
using the script as RED/GREEN evidence.

### Metadata
- Reproducible: yes
- Related Files: native/tests/unit-test/d3d12_perf_static_test.py

---

## [ERR-20260718-006] msbuild_wrapper_timeout_kept_children_alive

**Logged**: 2026-07-18T07:24:00+08:00
**Priority**: medium
**Status**: resolved
**Area**: tests

### Summary
The shell wrapper timed out while the launched MSBuild child process tree kept compiling.

### Error
```
command timed out after 64056 milliseconds
```

### Context
Building the required Debug|x64 test-cases.sln after adding a D3D12 diagnostic.

### Resolution
After a timed-out build command, inspect MSBuild and cl.exe before retrying;
monitor the existing build instead of starting another build.

### Metadata
- Reproducible: yes
- Related Files: D:/Work/CocosProjects/cocos-test-projects/build/windows/proj/test-cases.sln

---

## [ERR-20260718-007] static_test_wrong_workdir

**Logged**: 2026-07-18T10:54:00+08:00
**Priority**: low
**Status**: resolved
**Area**: tests

### Summary
The static test was invoked from the build directory with a repository-relative path.

### Error
```
python.exe: can't open file '...build\\windows\\proj\\native\\tests\\unit-test\\d3d12_perf_static_test.py'
```

### Resolution
Run repository-relative tests from the engine root, and run MSBuild separately
from the project build directory.

---
### ERR-20260718-008: PowerShell command-side wildcard is not expanded for rg input

- Symptom: `rg` reported invalid filenames for `D3D12DescriptorSet.*` and `D3D12PipelineLayout.*`.
- Cause: PowerShell passed the wildcard literally to `rg`; this was a diagnostic command failure, not source failure.
- Prevention: pass explicit file paths or let `rg` discover files from a directory.
### ERR-20260718-009: Parallel Debug build contended on cocos_engine.pdb

- Symptom: solution build with `/m` failed with C1041 for `Debug\\cocos_engine.pdb` across many translation units.
- Cause: parallel MSVC workers attempted concurrent PDB writes; no source diagnostic was emitted.
- Prevention: rerun the Debug verification build with `/m:1` before attributing a failure to the code change.
### ERR-20260718-010: Debug solution link was blocked by a missing unrelated object

- Symptom: serial solution build reached link and failed with LNK1104 for `cocos_engine.dir\\Debug\\ForwardStage.obj`.
- Cause: the target's incremental object set is incomplete after the preceding interrupted build; changed D3D12 sources had no compile diagnostic.
- Prevention: verify the changed source set with the `ClCompile` target before deciding whether a controlled full rebuild is needed.
### ERR-20260718-011: Project tracking logs are held by another build process

- Symptom: serial `cocos_engine` rebuild failed in `TrackedVCToolTask` because `cl.read.1.tlog` is held by another process.
- Cause: a concurrent IDE or build task owns the same Debug project state; no C++ compile diagnostic was emitted.
- Prevention: do not retry destructive rebuilds until the owner exits; use read-only process inspection and preserve the user's running session.
### ERR-20260718-012: Temporary compile command had invalid PowerShell quote interpolation

- Symptom: the isolated compiler verification script stopped with a PowerShell parser error before invoking `cl.exe`.
- Cause: escaped quotes were used inside an interpolated replacement expression.
- Prevention: construct quoted compiler paths with PowerShell format strings, then invoke the controlled command through `cmd.exe /d /c`.
### ERR-20260718-013: Isolated cl.exe invocation lacked the Visual C++ environment

- Symptom: temporary source compilation stopped at standard header `functional` not found.
- Cause: the raw compiler process did not inherit INCLUDE/LIB from the Visual Studio developer environment.
- Prevention: call `VsDevCmd.bat -arch=x64 -host_arch=x64` in the same `cmd.exe` process before isolated compilation.
### ERR-20260718-014: Required Debug solution build exceeded the 120-second command window

- Symptom: the non-clean serial `test-cases.sln` build ran without diagnostics but exceeded the shell command timeout.
- Cause: the earlier failed rebuild had invalidated a large Debug object set; the build now needs a longer monitored invocation.
- Prevention: launch the user-authorized build as a hidden child process with redirected logs, then poll completion instead of truncating it at the interactive command timeout.
### ERR-20260718-015: Static runner test used the native subtree as the repository root

- Symptom: the new foreground-runner contract errored because it looked for `native/d3d12_perf_records/run_round.ps1`.
- Cause: the static test's `ROOT` intentionally points at `native`, not the repository root.
- Prevention: reference repository-level artifacts through `ROOT.parent` in this test file.

### ERR-20260718-016: WPR CPU sampling is blocked by the local performance policy

- Symptom: `wpr -start CPU.verbose -filemode` failed before launching `test-cases.exe` with `0xc5585011` ("Failed to enable the policy to profile system performance").
- Cause: the Windows performance-recording policy on this machine denies CPU profiling for the current session; this is unrelated to the D3D12 executable or source changes.
- Prevention: check `wpr -status` first, attempt the profiler once, then use RenderDoc timing and opt-in engine diagnostics if the policy blocks ETW. Do not retry or infer a code failure from this HRESULT.

### ERR-20260719-017: PowerShell did not expand ProgramFiles(x86) inside a single-quoted path

- Symptom: invoking `vswhere.exe` failed because PowerShell treated `${env:ProgramFiles(x86)}` as literal text.
- Cause: the executable path was constructed in a single-quoted string, which disables variable expansion.
- Prevention: read `${env:ProgramFiles(x86)}` into a variable first or use `[Environment]::GetEnvironmentVariable('ProgramFiles(x86)')`, then join the remaining path.

### ERR-20260719-018: MSBuild is not registered in the non-developer PowerShell PATH

- Symptom: invoking `msbuild` returned "The term 'msbuild' is not recognized" before compilation.
- Cause: the Codex PowerShell process is not a Visual Studio Developer shell.
- Prevention: invoke `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe` explicitly for this workspace.

### ERR-20260719-019: apply_patch absolute-path write failed transiently

- Symptom: an otherwise valid patch reported `Failed to write file` for an absolute workspace path.
- Cause: unknown tool-side absolute-path handling failure; the file was not modified.
- Prevention: retry the identical patch using a repository-relative path before changing patch content.

### ERR-20260719-020: PowerShell did not expand a wildcard passed to rg

- Symptom: `rg` rejected `LinearAllocator*` with Windows error 123 while inspecting reference source.
- Cause: PowerShell passed the wildcard as a literal path argument; `rg` does not expand it itself on Windows.
- Prevention: pass the two explicit filenames or search the containing directory with a glob filter.

### ERR-20260720-021: Computer Use sky application-list API changed

- Symptom: the foreground sampling loop failed immediately with `sky.list_applications is not a function`.
- Cause: the current Computer Use SDK exposed a different application-discovery surface than the earlier persistent session summary described.
- Prevention: inspect the live `sky` object or current Computer Use documentation before reusing application-discovery method names; keep the launched test process alive so slow scene loading still contributes to warmup.

### ERR-20260720-022: Diagnostic rg command mixed a missing path and fragile regex quoting

- Symptom: one search returned exit 1 for a nonexistent `D3D12Std.h`; a later cleanup search produced an unclosed-group regex error.
- Cause: the diagnostic command included an assumed filename and used a PowerShell-quoted alternation containing escaped parentheses.
- Prevention: search only confirmed files/directories and use `rg -F -e pattern` for cleanup marker checks.

### ERR-20260720-023: nodeRepl.emitImage rejected a screenshot descriptor object

- Symptom: after a completed 50-second foreground wait, `nodeRepl.emitImage(state.screenshots[0])` reported an unsupported value.
- Cause: this SDK version requires the screenshot data URL, not the surrounding screenshot descriptor.
- Prevention: pass `state.screenshots[0].url` to `nodeRepl.emitImage`; the preceding wait remains valid when only image emission fails.
## ERR-20260720-024 — MSBuild launched with an execution timeout that was too short

- **Observed:** The incremental `test-cases.sln` build was terminated with exit code 124 after about five seconds.
- **Cause:** `shell_command` was given `timeout_ms=1000`; this tool kills the process instead of yielding a resumable cell.
- **Correction:** Launch builds with a normal 60-second window and use the yielded cell/wait path only when the orchestration layer explicitly returns a cell id.
## ERR-20260720-025 — WPR CPU profile requires unavailable system policy

- **Observed:** `wpr.exe -start CPU -filemode` failed with `0xc5585011`, “Failed to enable the policy to profile system performance.”
- **Cause:** The current non-elevated session lacks the Windows system-profile privilege.
- **Correction:** Do not change system policy or request elevation for this task; use low-frequency in-process phase closure or a user-mode profiler instead.
## [ERR-20260729-001] functions-exec-parallel-shell-wrapper

**Logged**: 2026-07-29T00:00:00+08:00
**Priority**: low
**Status**: resolved
**Area**: infra

### Summary
The JavaScript orchestration wrapper failed without exposing which parallel shell command caused the failure.

### Error
```
Script failed
Script error:
Exit code: 1
```

### Context
- Attempted to run four independent repository-read commands through `functions.exec` and `Promise.all`.
- The wrapper returned no child output, so the failing command could not be identified.
- Re-running the reads directly exposed useful output; one `rg` search simply had no matches.

### Suggested Fix
For review baselining where a no-match `rg` is acceptable, run direct shell reads or make each child return its exit code and captured output instead of allowing one rejection to discard all results.

### Metadata
- Reproducible: unknown
- Related Files: .learnings/ERRORS.md

### Resolution
- **Resolved**: 2026-07-29T00:00:00+08:00
- **Notes**: Split the wrapper into directly observable commands.

---
