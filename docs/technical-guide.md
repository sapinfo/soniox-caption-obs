# Soniox Captions OBS Plugin - Technical Guide

## Project Overview

| Item | Detail |
|------|--------|
| Plugin Name | soniox-caption-obs |
| Version | 0.1.0 |
| Language | C++ (C++17) |
| Build System | CMake 3.28+ |
| OBS SDK | 31.1.1 |
| Platforms | macOS (arm64), Windows (x64), Linux (x86_64) |
| License | TBD (Open Source) |
| Dependencies | IXWebSocket (WebSocket), nlohmann/json (JSON), OpenSSL (TLS) |

## Features

- Real-time speech-to-text using Soniox API (stt-rt-v4 model)
- OBS Source type - displays captions directly on screen
- Built-in translation (Soniox one-way translation)
- Audio source selection (any OBS audio input)
- CJK font support (Korean, Japanese, Chinese)
- Configurable font size and language

## Project Structure

```
SonioxCaptionPlugIn/
├── CMakeLists.txt          # Build config (main) - links OBS, IXWebSocket, nlohmann/json
├── CMakePresets.json        # Platform build presets
├── buildspec.json           # Plugin metadata + OBS SDK version
├── src/
│   ├── plugin-main.cpp      # All plugin code (single file)
│   ├── plugin-support.c.in  # Logging helper (CMake auto-generated)
│   └── plugin-support.h     # Logging helper header
├── data/locale/en-US.ini    # Locale strings
├── cmake/                   # Build system (rarely modified)
├── docs/                    # Documentation
│   ├── technical-guide.md   # This file
│   └── dev-history.md       # Step-by-step development log
└── build_macos/             # Build output (gitignored)
```

## Build Commands

### Prerequisites
- CMake 3.28+
- Xcode 16+ (macOS) / Visual Studio 2022 (Windows) / GCC 12+ (Linux)
- OpenSSL (`brew install openssl` on macOS)

### macOS (Apple Silicon)

```bash
# Configure (downloads OBS SDK + dependencies, slow on first run)
cmake --preset macos

# Build
cmake --build --preset macos

# Install
cp -R build_macos/RelWithDebInfo/soniox-caption-obs.plugin \
  ~/Library/Application\ Support/obs-studio/plugins/

# Uninstall
rm -rf ~/Library/Application\ Support/obs-studio/plugins/soniox-caption-obs.plugin
```

### Windows (x64)

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64
# Install: copy to %APPDATA%/obs-studio/plugins/soniox-caption-obs/
```

### Linux (x86_64)

```bash
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
# Install: copy to ~/.config/obs-studio/plugins/soniox-caption-obs/
```

## Architecture

```
┌─────────────────────────────────────────────────┐
│ OBS Studio                                      │
│                                                 │
│  ┌──────────────────┐   ┌────────────────────┐  │
│  │ Audio Source      │   │ Soniox Captions     │  │
│  │ (Mic/Aux)        │   │ Source              │  │
│  │                  │   │                    │  │
│  │ float32 48kHz ───┼──►│ ┌────────────────┐ │  │
│  │                  │   │ │ Audio Callback  │ │  │
│  └──────────────────┘   │ │ float32→int16   │ │  │
│                         │ │ 48kHz→16kHz     │ │  │
│                         │ └───────┬────────┘ │  │
│                         │         │PCM binary │  │
│                         │         ▼          │  │
│                         │ ┌────────────────┐ │  │
│                         │ │ IXWebSocket    │ │  │
│                         │ │ WSS connection │ │  │
│                         │ └───────┬────────┘ │  │
│                         │         │          │  │
│                         │         ▼          │  │
│                         │ Soniox API Server  │  │
│                         │ stt-rt-v4 model    │  │
│                         │         │          │  │
│                         │         ▼          │  │
│                         │ ┌────────────────┐ │  │
│                         │ │ Token Parser   │ │  │
│                         │ │ final/partial  │ │  │
│                         │ │ + translation  │ │  │
│                         │ └───────┬────────┘ │  │
│                         │         │          │  │
│                         │         ▼          │  │
│                         │ ┌────────────────┐ │  │
│                         │ │ Text Source    │ │  │
│                         │ │ (ft2/gdiplus) │ │  │
│                         │ │ → OBS screen  │ │  │
│                         │ └────────────────┘ │  │
│                         └────────────────────┘  │
└─────────────────────────────────────────────────┘
```

## Soniox WebSocket Protocol

### Config Message (first message after connect)

```json
{
  "api_key": "...",
  "model": "stt-rt-v4",
  "audio_format": "pcm_s16le",
  "sample_rate": 16000,
  "num_channels": 1,
  "language_hints": ["ko"],
  "enable_endpoint_detection": true,
  "translation": {
    "type": "one_way",
    "target_language": "en"
  }
}
```

### Audio Streaming
- Format: PCM signed 16-bit little-endian, mono, 16kHz
- OBS audio (float32, 48kHz) → downsample 3:1 → convert to int16
- Send as WebSocket binary frames

### Token Response
```json
{
  "tokens": [
    {"text": "안녕", "is_final": true},
    {"text": "하세요", "is_final": false},
    {"text": "Hello", "is_final": true, "translation_status": "translation"},
    {"text": "<end>"}
  ]
}
```

- `is_final: true` → confirmed text (accumulated)
- `is_final: false` → provisional text (replaced each update)
- `translation_status: "translation"` → translated token
- `text: "<end>"` → utterance complete (endpoint)

### Stop Signal
- Send empty string `""` → Soniox stops processing
- Close WebSocket

## Key Dependencies

| Library | Version | Purpose | CMake Integration |
|---------|---------|---------|-------------------|
| OBS::libobs | 31.1.1 | OBS Plugin API | Auto-downloaded via buildspec.json |
| IXWebSocket | v11.4.5 | WebSocket client (WSS) | FetchContent from GitHub |
| nlohmann/json | v3.11.3 | JSON parsing | FetchContent from GitHub |
| OpenSSL | 3.x | TLS for WSS | Homebrew on macOS |

## Development Roadmap

| Step | Description | Status |
|------|-------------|--------|
| 1 | Skeleton plugin (load/unload) | Done |
| 2 | Register text source in OBS | Done |
| 3 | Properties UI (API key, language, font) | Done |
| 4 | WebSocket connection to Soniox | Done |
| 5 | Audio capture + streaming | Done |
| 6 | Token parsing + text display | Done |
| 7 | Translation support | Done |
| 8 | Cross-platform build + GitHub release | Next |
| 9 | Dock panel UI (Start/Stop without Properties) | Planned |
| 10 | Auto-reconnect on disconnect | Planned |
