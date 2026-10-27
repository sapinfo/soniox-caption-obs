# CLAUDE.md - Soniox Captions OBS Plugin

## Project Overview
OBS Studio plugin providing real-time speech-to-text captions using Soniox API.
Single-file C++ plugin (`src/plugin-main.cpp`) with WebSocket streaming.

## Tech Stack
- **Language**: C++17
- **Build**: CMake 3.28+ / Xcode (macOS) / VS 2022 (Windows) / Ninja (Linux)
- **OBS SDK**: 31.1.1 (via buildspec.json auto-download)
- **Dependencies**: IXWebSocket v11.4.5 (WebSocket), nlohmann/json v3.11.3, TLS: OpenSSL 3.x (macOS/Linux) / static mbedTLS from obs-deps (Windows)
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

# Windows local build (VS 2026): CMakeUserPresets.json (gitignored) inherits windows-x64
# with generator "Visual Studio 18 2026" + SDK 10.0.26100.0. Use VS-bundled cmake.exe.
cmake --preset windows-x64-local
cmake --build --preset windows-x64-local
# Install: copy build_x64/RelWithDebInfo/soniox-caption-obs.dll to
#   C:\ProgramData\obs-studio\plugins\soniox-caption-obs\bin\64bit\  (OBS must be closed)

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
- Endpoint delay: Soniox `max_endpoint_delay_ms` (500-3000ms, default **500** in this plugin). Lower = faster caption finalize after speech pause. Below 500ms requires manual `{"type":"finalize"}` message — Soniox does not expose silence threshold or VAD sensitivity (semantic endpointing only)
- Display mode: 3-mode toggle (`original` / `translation` / `both`). "Both" uses two independent private text sources (`text_source` for original, `text_source_trans` for translation), rendered stacked vertically via `gs_matrix_translate3f` in `video_render`
- Translation committed protection: on `<end>` token, translation text is held for 1.5s (`committed_protect_until` via `std::chrono::steady_clock`) while partial updates from next utterance are ignored — prevents flickering (pattern borrowed from ELSTTv2 overlay). Translation suppressed during protection is kept in `pending_trans` and flushed by the tick thread on expiry — otherwise short back-to-back utterances lose their translation entirely
- Soniox token order (verified): final translation tokens arrive ~10-40ms after the source finals and **before** `<end>`
- Threading: `control_mutex` serializes start/stop/test/update/destroy (UI vs hotkey thread). WS callbacks read only the `session` snapshot taken at start — never `settings` strings. `display_mode` is atomic for the render thread; text style behind `style_mutex`. Stop order: remove audio callback → join tick thread → stop websocket
- Tick thread (per session, 250ms): sends `{"type":"keepalive"}` when no audio sent for 5s (Soniox closes with 408 after 20s without audio/keepalive) and clears captions after `caption_hold_ms` (default 4000) following `<end>`
- Soniox `error_message` (e.g. 401 bad key) disables IXWebSocket auto-reconnect and keeps the error text on screen
- Never call `ix::WebSocket::stop()` from its own callback (self-join → abort). Use non-blocking `close()` instead (test connection does this)
- Windows `text_gdiplus` with extents uses `extents_cx/cy` as the exact source size (no auto height) — `extents_cy=0` renders nothing. Plugin uses a fixed 3-line box (`font_size * 1.4 * 3`)
- Windows TLS uses obs-deps static mbedTLS (+ `bcrypt`) so the DLL depends only on OBS/system DLLs — linking OpenSSL would require shipping libssl/libcrypto DLLs
- Built against OBS SDK 31.1.1 (upstream obs-plugintemplate's latest); verified loading and working in OBS 33.0.0
- Soniox translation tokens arrive in sentence-sized chunks (not word-by-word like STT). Client-side typing effects don't work well — committed protection is the better UX approach
- Test connection: verifies WebSocket Open only, then immediately closes — no config/auth payload sent (connection success = server reachable). Uses a separate `test_websocket`; ignored while captioning

## Important Conventions
- Version in `buildspec.json` (single source of truth)
- Commit messages: imperative mood, explain "why" not "what"
- GitHub repo: `sapinfo/soniox-caption-obs`
- E2E testing: `tools/e2e/` (Soniox protocol probe + obs-websocket driven OBS test). API key in root `.env` (`SONIOX_API_KEY`, gitignored) — scripts read it, never print it
