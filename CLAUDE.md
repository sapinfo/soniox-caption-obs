# CLAUDE.md - Soniox Captions OBS Plugin

## Project Overview
OBS Studio plugin providing real-time speech-to-text captions using Soniox API.
Single-file C++ plugin (`src/plugin-main.cpp`) with WebSocket streaming.

## Tech Stack
- **Language**: C++17
- **Build**: CMake 3.28+ / Xcode (macOS) / VS 2022 (Windows) / Ninja (Linux)
- **OBS SDK**: 31.1.1 (via buildspec.json auto-download)
- **Dependencies**: IXWebSocket v11.4.5 (WebSocket), nlohmann/json v3.11.3, OpenSSL 3.x
- **Platforms**: macOS (arm64 + x86_64), Windows (x64), Ubuntu (x86_64)

## Build Commands
```bash
# macOS local build
cmake --preset macos
cmake --build --preset macos

# Install to OBS
cp -R build_macos/RelWithDebInfo/soniox-caption-obs.plugin \
  ~/Library/Application\ Support/obs-studio/plugins/

# Clean rebuild (after adding/removing source files)
rm -rf build_macos && cmake --preset macos

# Release (tag triggers CI build for all platforms)
git tag x.x.x && git push origin x.x.x
```

## CI/CD
- **Trigger**: Tag push only (`on.push.tags`). Main branch push does NOT trigger Actions.
- **Builds**: macOS arm64 + x86_64 (cross-compile), Windows x64, Ubuntu x86_64
- **Release**: Automatic draft release on valid semver tag (e.g., `0.1.2`)
- **Codesigning**: macOS arm64 only (requires Apple Developer secrets)

## Key Architecture Decisions
- Single source file (`plugin-main.cpp`) - all plugin logic in one file
- Uses OBS built-in text source (`text_ft2_source_v2` / `text_gdiplus`) for rendering
- Button toggle: use `obs_property_set_description()` in callback (RefreshProperties doesn't re-call get_properties)
- Audio: OBS float32 48kHz -> int16 16kHz downsampled -> WebSocket binary to Soniox
- x86_64 macOS CI: Intel Homebrew OpenSSL at `/usr/local/opt/openssl@3`
- `CMakeLists.txt` guards `OPENSSL_ROOT_DIR` with `NOT DEFINED CACHE{}` to prevent preset override
- Font: `obs_properties_add_font()` for system font dialog (face/style/size in one `obs_data_t` object)
- Text style properties passed through to internal text source (`color1/2`, `outline`, `drop_shadow`, `custom_width`, `word_wrap` for ft2; `color`, `extents`, `outline` for gdiplus)

## Important Conventions
- Version in `buildspec.json` (single source of truth)
- Commit messages: imperative mood, explain "why" not "what"
- GitHub repo: `sapinfo/soniox-caption-obs`
