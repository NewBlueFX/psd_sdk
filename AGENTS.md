# Repository Guidelines

## Project Structure & Modules
- Core library lives in `src/Psd` (headers, platform abstractions, parsing, export helpers).
- Sample CLI lives in `src/Samples` and demonstrates reading/writing PSDs; depends on the core library.
- `bin` holds sample PSD assets used by the sample app.
- `build` contains generated IDE projects and build outputs (Visual Studio/Xcode/CMake out-of-tree builds).

## Build, Test, and Development Commands
- Configure (out-of-tree recommended): `cmake -S . -B build/out -DCMAKE_BUILD_TYPE=Release`.
- Build all targets: `cmake --build build/out -- -j$(nproc)`.
- Build and run the sample (Linux/macOS): `cmake --build build/out --target PsdSamples && ./build/out/src/Samples/PsdSamples ../bin/your.psd`.
- On Windows/MSVC: open a generated solution from `build` or run `cmake --build build/out --config Release`.
- Keep the working directory pointed to the repo root (or copy `bin` next to the produced executable) so the sample can locate PSD assets.

## Coding Style & Naming
- C++11 codebase; follow existing patterns (tabs for indentation, CRLF line endings in legacy files).
- Types and classes use PascalCase (`Document`, `Layer`), functions CamelCase (`ExpandMaskToCanvas`), constants/macros upper-case with underscores.
- Prefer small, header-only abstractions; include only what you need (see `PsdPch.h` usage).
- Match surrounding brace placement and spacing when editing; avoid introducing trailing whitespace.

## Testing Guidelines
- No formal automated tests today; validate changes by rebuilding targets and exercising `PsdSamples` against files in `bin`.
- When adding features, include reproducible sample steps (input PSD path, expected behavior) or extend the sample app to cover new code paths.
- Run address/UB sanitizers or Valgrind when touching parsing/memory code paths where possible.

## Commit & Pull Request Guidelines
- Commit messages: short, imperative summaries (e.g., `Fix layer bounds read`) as in existing history.
- PRs should describe scope, risks, and platforms/toolchains validated (Linux/macOS/Windows). Link issues when available and note any new assets added to `bin`.
- Include build commands used and sample files exercised; add screenshots/log snippets if output behavior changes.

## Security & Configuration Tips
- The library reads untrusted PSDs; never trust metadata lengths—prefer bounds checks mirroring `PsdParse*` helpers.
- Keep platform-specific code confined to their adapters (e.g., `PsdNativeFile.cpp`); avoid leaking OS headers into public interfaces.
- Reference: Photoshop file format spec at https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/, layer/mask details at https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/#50577409_72092, and resource block details at https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/#50577413_61897 when clarifying field semantics.
