# Soniox Captions for OBS

Real-time speech-to-text captions for OBS Studio using the [Soniox](https://soniox.com) API.

## Features

- Real-time speech-to-text transcription (Soniox stt-rt-v4 model)
- Built-in translation (Soniox one-way translation)
- Multiple language support (Korean, English, Japanese, Chinese, Spanish, French, German)
- CJK font support
- Configurable font size
- OBS audio source selection

## Requirements

- OBS Studio 30.0+
- [Soniox API Key](https://soniox.com) (free tier available)
- macOS (Apple Silicon) / Windows / Linux

## Installation

### macOS

1. Download the latest release from [Releases](../../releases)
2. Copy `soniox-caption-obs.plugin` to `~/Library/Application Support/obs-studio/plugins/`
3. Restart OBS Studio

### Uninstall

```bash
rm -rf ~/Library/Application\ Support/obs-studio/plugins/soniox-caption-obs.plugin
```

## Usage

1. In OBS, add a new Source: **Soniox Captions**
2. Open Properties:
   - Enter your **Soniox API Key**
   - Select **Audio Source** (e.g., Mic/Aux)
   - Choose **Language**
   - Optionally enable **Translation**
3. Click **Start Caption**
4. Speak into your microphone - captions appear in real-time!

## Build from Source

### Prerequisites

- CMake 3.28+
- Xcode 16+ (macOS) / Visual Studio 2022 (Windows) / GCC 12+ (Linux)
- OpenSSL (`brew install openssl` on macOS)

### macOS

```bash
cmake --preset macos
cmake --build --preset macos
```

The built plugin is at `build_macos/RelWithDebInfo/soniox-caption-obs.plugin`.

### Windows

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64
```

### Linux

```bash
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

## How It Works

1. Captures audio from the selected OBS audio source
2. Converts audio (float32 48kHz -> int16 16kHz PCM)
3. Streams audio to Soniox via WebSocket (wss://stt-rt.soniox.com)
4. Parses real-time token responses (partial + final text)
5. Displays captions on OBS screen via text source

## Dependencies

- [IXWebSocket](https://github.com/machinezone/IXWebSocket) - WebSocket client
- [nlohmann/json](https://github.com/nlohmann/json) - JSON parsing
- [OpenSSL](https://www.openssl.org/) - TLS
- [OBS Plugin Template](https://github.com/obsproject/obs-plugintemplate) - Build system

All dependencies are automatically downloaded during build via CMake FetchContent.

## License

GPL-2.0 - see [LICENSE](LICENSE)
