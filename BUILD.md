# Building EB Recorder on Windows

The repository contains a self-contained Windows build helper for the currently tested OBS SDK baseline.

## Tested build environment

- Windows 10/11 x64
- Visual Studio 2026 (`18.x`) with **Desktop development with C++**
- Visual Studio bundled CMake `4.2+`
- Windows SDK `10.0.26100`
- OBS Studio source `32.2.2`
- obs-deps `2026-07-15` x64
- Qt dependency archive from obs-deps `2026-07-15`

The build helper verifies downloaded archives by SHA-256 before use.

## Normal build

From the repository root:

```text
BUILD.cmd
```

The script:

1. discovers Visual Studio through `vswhere.exe`,
2. downloads and verifies the pinned OBS / obs-deps archives when they are not already cached,
3. builds a minimal local x64 OBS development SDK,
4. builds EB Recorder itself in the CMake `Release` configuration,
5. stages the plugin and rejects accidental PDB files,
6. creates the production release archive and SHA-256 sidecar.

Output:

```text
EBRecorder-vX.Y.Z-Windows-x64.zip
EBRecorder-vX.Y.Z-Windows-x64.zip.sha256
```

The plugin binary is built as a production `Release` target. Debug symbols are not shipped in the user archive. The locally cached OBS development SDK may still use `RelWithDebInfo`; it is a build-only dependency and is never included in the release package.

## Clean build

```text
BUILD-CLEAN.cmd
```

This removes generated build trees while keeping downloaded/extracted dependency caches where possible.

## Full clean

```text
BUILD-FULL-CLEAN.cmd
```

This removes the complete project build cache and forces dependencies to be downloaded/extracted again.

## Dependency cache

The build helper stores project-local build data under `.cache/` and can reuse validated archives from neighboring `EBRecorder-*` development folders.

All cache/build/output folders are excluded from Git.

## Notes

The helper intentionally builds only the x64 OBS development targets needed by EB Recorder and patches the local extracted OBS source tree to skip the nested Win32 helper build. The patch is applied only inside `.cache/`; it does not modify a system OBS installation or an external OBS source checkout.
