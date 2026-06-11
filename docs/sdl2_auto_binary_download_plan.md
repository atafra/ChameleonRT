# Implementation Plan - Auto-Download SDL2 Binaries for ChameleonRT

Status: IN PROGRESS - implementation mostly complete (2026-06-10)
Goal: Make configure/build smoother by auto-downloading SDL2 binary packages when SDL2 is not already discoverable, while keeping an escape hatch for local/custom SDL2 installs.

## 0. Implementation status update (2026-06-10)

Implemented in-tree:
- New resolver module added: `cmake/sdl2.cmake`.
- Root CMake now includes SDL2 resolver before subdirectories.
- Resolution order implemented: `SDL2_PREBUILT_DIR` -> normal `find_package(SDL2 CONFIG QUIET)` -> Windows auto-fetch fallback.
- Windows auto-fetch implemented for pinned SDL2 `2.32.10` from official release archive.
- Extracted-folder detection implemented for observed archive layout (`SDL2-<version>`), with fallback checks.
- README updated with Windows auto-fetch behavior and override knobs.

Validated locally:
- Configure with explicit `SDL2_DIR`: PASS.
- Configure with explicit `SDL2_PREBUILT_DIR`: PASS.
- Configure with no SDL2 hints (Windows auto-fetch path): PASS.

Still pending for full completion:
- Archive SHA256 value is not yet hard-populated in repo defaults (hash variable exists, currently optional).
- Full build/runtime packaging verification (copy/install + app launch) has not yet been run in this pass.

---

## 1. Current state

Today top-level CMake calls `find_package(SDL2 CONFIG REQUIRED)`.
If SDL2 is not installed or not discoverable via `SDL2_DIR`/`CMAKE_PREFIX_PATH`, configure fails early.

Result: first-time setup friction is high, especially on clean Windows machines.

---

## 2. Target behavior

Desired UX:
1. Programmer runs CMake with no SDL2 setup on machine.
2. CMake auto-downloads a pinned SDL2 binary package.
3. CMake wires `SDL2::SDL2` and `SDL2::SDL2main` targets automatically.
4. Runtime SDL2 library is packaged next to executable and installed to `bin/`.

Still supported:
- User-provided SDL2 path (existing behavior) via `SDL2_DIR`.
- Explicit override path via new `SDL2_PREBUILT_DIR` cache variable.
- Manual opt-out of downloading.

---

## 3. Scope and strategy

Primary implementation target: Windows binary auto-download only (for now).

Why:
- Official SDL2 VC binary package includes CMake config and import libs.
- Matches most common local dev pain point in this repo.
- Lowest-risk first rollout.

Current non-Windows policy:
- Linux and macOS do not auto-fetch SDL2 in this implementation.
- Linux/macOS continue to use normal system/local discovery unless explicitly configured.

---

## 4. Proposed CMake changes

## 4.1 Top-level options

Implemented knobs (current code):

```cmake
option(CHAMELEONRT_AUTO_FETCH_SDL2 "Auto-download SDL2 binaries when not found" ON)
set(SDL2_PREBUILT_DIR "" CACHE PATH "Path to existing SDL2 package root (contains cmake/sdl2-config.cmake)")
set(CHAMELEONRT_SDL2_ARCHIVE_SHA256 "" CACHE STRING "Optional SHA256 for the pinned SDL2 archive")

# Hard-pinned in cmake/sdl2.cmake (not user-configurable cache):
set(CHAMELEONRT_SDL2_VERSION "2.32.10")
```

Decision lock:
- SDL2 version is hard-pinned to `2.32.10` in module logic.
- Auto-fetch behavior is enabled only on Windows in module logic.

Behavior rules:
- If `SDL2_PREBUILT_DIR` is set: use it directly.
- Else try normal `find_package(SDL2 CONFIG QUIET)`.
- If not found and auto-fetch enabled on Windows: fetch SDL2 package and retry `find_package` with explicit path.
- If still not found: fail with actionable error.

## 4.2 New helper module

Create new file: `cmake/sdl2.cmake`

Responsibilities:
1. Resolve SDL2 source path strategy (prebuilt dir, system, or auto-fetch).
2. Download/extract SDL2 binary package with pinned URL + hash.
3. Set `SDL2_DIR` to extracted package CMake folder.
4. Call `find_package(SDL2 CONFIG REQUIRED NO_DEFAULT_PATH PATHS <resolved path>)`.
5. Expose resolved runtime library paths for packaging.

Windows package URL pattern:
- `https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip`

Expected extracted shape:
- `include/`
- `lib/x64/`
- `cmake/sdl2-config.cmake`

## 4.3 Dependency packaging integration

Use existing packaging helpers in `cmake/package.cmake`.

For imported SDL2 targets:
- Keep using `crt_add_packaged_dependency(SDL2::SDL2)`.
- Keep existing runtime artifact install for `SDL2::SDL2`.

If package layout requires direct file staging in edge cases:
- Use `crt_add_packaged_files(...)` as fallback path.

## 4.4 util linkage behavior

No major behavior change needed in util target linking.

Keep:
- `target_link_libraries(util PUBLIC SDL2::SDL2)`
- Windows: `target_link_libraries(util PUBLIC SDL2::SDL2main)`

But simplify fallback logic if SDL2 targets are always provided by `sdl2-config.cmake` in fetched mode.

---

## 5. Validation matrix

## 5.1 Configure cases

1. Fresh Windows machine, no SDL2 installed, auto-fetch ON:
- Expect download + successful configure.

2. Windows with `SDL2_DIR` set:
- Expect no download, use provided package.

3. Windows with `SDL2_PREBUILT_DIR` set:
- Expect no download, use explicit path.

4. Auto-fetch OFF and SDL2 missing:
- Expect clear fatal error with setup guidance.

## 5.2 Build/runtime cases

1. Build executable and at least one backend.
2. Confirm SDL2 runtime library lands beside executable.
3. Confirm install tree has SDL2 runtime under `bin/`.
4. Launch app to verify SDL2 window init works.

---

## 6. CI and caching

Cache extracted SDL2 package directory in CI to avoid repeated downloads.

Recommended cache path:
- `${CMAKE_BINARY_DIR}/sdl2`

Add log lines in configure to state:
- whether SDL2 came from system, prebuilt path, or auto-fetch.

---

## 7. Security and reproducibility

Pin both:
- exact SDL2 version
- archive hash (`URL_HASH SHA256=...`)

Do not use floating latest URLs.

Document update process:
1. bump hard-pinned SDL2 version in `cmake/sdl2.cmake` only by explicit maintainer decision
2. update URL hash
3. run validation matrix

---

## 8. Phased rollout checklist

Phase 0 - wiring
- [x] Add top-level SDL2 auto-fetch options.
- [x] Add `cmake/sdl2.cmake` include point in top-level CMake before util/backends.

Phase 1 - Windows binary auto-fetch
- [x] Implement Windows download/extract path in `cmake/sdl2.cmake`.
- [x] Resolve and enforce `SDL2_DIR` from extracted package.
- [~] Ensure imported target packaging works with existing package helpers.

Phase 2 - docs and diagnostics
- [x] Update README setup section with new default behavior and overrides.
- [x] Add clear configure messages for selected SDL2 source.
- [x] Add fatal-error guidance when resolution fails.

Phase 3 - platform expansion
- [ ] Evaluate Linux/macOS binary source viability.
- [ ] Add those paths only if package layout and runtime packaging are reliable.

Legend: `[x]` complete, `[~]` partially complete, `[ ]` not started.

---

## 9. Resolved decisions (from former open questions)

1. Platform scope decision:
- Auto-fetch is Windows-only for now.
- Linux/macOS stay on system/local SDL2 discovery.

2. Default policy decision:
- `CHAMELEONRT_AUTO_FETCH_SDL2` remains ON by default.
- The auto-fetch code path is gated to Windows only.

3. Package manager interaction decision:
- Existing discovery always wins first (`find_package(SDL2 CONFIG QUIET)`).
- Auto-fetch runs only as fallback when SDL2 is still unresolved.

4. Version policy decision:
- SDL2 is hard-pinned to `2.32.10` (current version in use).
- Any upgrade is an explicit version bump task with updated hash and validation run.

Remaining open questions:
- Should the repository set a non-empty default for `CHAMELEONRT_SDL2_ARCHIVE_SHA256` now (strict integrity), or keep it configurable until CI/network mirrors are finalized?
- Do we want to remove/trim the legacy SDL2 manual fallback block in `util/CMakeLists.txt` now that top-level SDL2 resolution is centralized?

---

## 10. Suggested first implementation slice

Implement Windows-only auto-fetch first, with strict hashing and clean fallback:
- If system/prebuilt SDL2 exists, use it.
- Else auto-fetch SDL2 VC package.
- Else fail with guidance.

This delivers immediate developer ergonomics gain with minimal cross-platform risk.
