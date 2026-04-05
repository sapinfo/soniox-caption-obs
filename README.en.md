# Soniox Captions for OBS

**English** | [한국어](README.md)

Real-time speech-to-text captions for OBS Studio using the [Soniox](https://soniox.com) API.

---

## Features

- Real-time speech-to-text (Soniox stt-rt-v4 model)
- Built-in translation (Korean, English, Japanese, Chinese, Spanish, French, German)
- Hotkey support (start/stop without opening Properties)
- Auto-reconnect on disconnect
- CJK font support
- Configurable font size

## Quick Start

### 1. Get Soniox API Key

Sign up at [soniox.com](https://soniox.com) and get your API key (free tier available).

### 2. Download

[**Download Latest Release**](../../releases/latest)

| Platform | File |
|----------|------|
| macOS (Apple Silicon) | `soniox-caption-obs-x.x.x-macos-arm64.zip` |
| Windows | `soniox-caption-obs-x.x.x-windows-x64.zip` |
| Linux (Ubuntu) | `soniox-caption-obs-x.x.x-x86_64-linux-gnu.deb` |

### 3. Install

<details>
<summary><b>macOS</b></summary>

1. Download and unzip `soniox-caption-obs-x.x.x-macos-arm64.zip`
2. Copy `soniox-caption-obs.plugin` to:
   ```
   ~/Library/Application Support/obs-studio/plugins/
   ```
3. Restart OBS Studio

**Uninstall:**
```bash
rm -rf ~/Library/Application\ Support/obs-studio/plugins/soniox-caption-obs.plugin
```
</details>

<details>
<summary><b>Windows</b></summary>

1. Download and unzip `soniox-caption-obs-x.x.x-windows-x64.zip`
2. Copy contents to:
   ```
   %APPDATA%\obs-studio\plugins\soniox-caption-obs\
   ```
3. Restart OBS Studio
</details>

<details>
<summary><b>Linux (Ubuntu)</b></summary>

```bash
sudo dpkg -i soniox-caption-obs-x.x.x-x86_64-linux-gnu.deb
```

Or manually copy to `~/.config/obs-studio/plugins/soniox-caption-obs/`
</details>

### 4. Usage

1. In OBS, click **+** in Sources → select **Soniox Captions**
2. Right-click the source → **Properties**:
   - Enter your **Soniox API Key**
   - Select **Audio Source** (e.g., Mic/Aux)
   - Choose **Language**
   - (Optional) Check **Enable Translation** and select target language
3. Click **Start Caption**
4. Speak into your microphone — captions appear in real-time!

### Hotkey

Assign a hotkey in **OBS Settings → Hotkeys → Toggle Soniox Captions** to start/stop without opening Properties.

---

## Build from Source

<details>
<summary>Expand</summary>

### Prerequisites

- CMake 3.28+
- Xcode 16+ (macOS) / Visual Studio 2022 (Windows) / GCC 12+ (Linux)
- OpenSSL (macOS: `brew install openssl`)

### macOS

```bash
cmake --preset macos
cmake --build --preset macos
# Output: build_macos/RelWithDebInfo/soniox-caption-obs.plugin
```

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

All dependencies (IXWebSocket, nlohmann/json, OBS SDK) are automatically downloaded via CMake FetchContent.

</details>

## License

GPL-2.0 - see [LICENSE](LICENSE)
