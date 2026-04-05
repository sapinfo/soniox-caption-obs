# Soniox Captions for OBS

**English** | [한국어](README.md)

**Real-time speech-to-text captions + translation** for OBS Studio using the [Soniox](https://soniox.com) API.
Speak in any language and see both the original captions and translated subtitles displayed simultaneously.

---

## What is Soniox?

[Soniox](https://soniox.com) is a best-in-class real-time speech-to-text (STT) API.

- **Speed**: 249ms median latency — top of the industry
- **Accuracy**: 1.29% Word Error Rate (WER) — best-in-class precision
- **60+ languages**: Same accuracy and speed for Korean, Japanese, Chinese, and all non-English languages. Most competitors are "English-first" with accuracy dropping dramatically for other languages
- **Code-switching**: Automatically handles mid-sentence language switching (e.g., Korean → English)
- **Real-time translation**: Streaming translation for 3,600+ language pairs
- **Advanced endpointing**: Uses tone and meaning — not just silence — to detect when a speaker is done

![Daily.co STT Benchmark - Soniox leads in both speed and accuracy](docs/images/daily-co-benchmark.png)
*Source: [Daily.co (Pipecat) STT Benchmark](https://daily.co/blog/benchmarking-stt-for-voice-agents)*

> *"Most 'industry leaders' are English-first. Their accuracy drops off a cliff the moment you move into other languages. Soniox is the only speech API provider you can use today to build voice agents for all 8 billion people on Earth."*
> — [Soniox Blog](https://soniox.com/blog/soniox-named-best-in-class-for-voice-agents)

## Features

- **Real-time speech-to-text** (Soniox stt-rt-v4 model)
- **Real-time translation** — translated subtitles appear alongside original captions instantly (KO↔EN, KO↔JA, KO↔ZH, and more — 7 languages)
- Hotkey support (start/stop without opening Properties)
- Auto-reconnect on disconnect
- CJK font support
- Configurable font size

## Quick Start

### 1. Get Soniox API Key

Sign up at [soniox.com](https://soniox.com), register a payment method, add credits, and get your API key.

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
2. In OBS, go to **File** → **Show Settings Folder**
3. Open the **plugins** folder
4. Copy `soniox-caption-obs.plugin` into the **plugins** folder
5. Restart OBS Studio

**Uninstall:** In OBS, go to **File** → **Show Settings Folder** → **plugins** folder → delete `soniox-caption-obs.plugin`
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
