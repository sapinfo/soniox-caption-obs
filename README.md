# Soniox Captions for OBS

[English](README.en.md) | **한국어**

Soniox API를 사용한 OBS Studio 실시간 자막 플러그인입니다.

---

## 주요 기능

- 실시간 음성→텍스트 (Soniox stt-rt-v4 모델)
- 내장 번역 (한국어, 영어, 일본어, 중국어, 스페인어, 프랑스어, 독일어)
- 단축키 지원 (Properties 열지 않고 시작/중지)
- 네트워크 끊김 시 자동 재연결
- 한중일(CJK) 폰트 지원
- 폰트 크기 조절 가능

## 빠른 시작

### 1. Soniox API 키 발급

[soniox.com](https://soniox.com) 에서 가입 후 API 키를 발급받으세요 (무료 플랜 있음).

### 2. 다운로드

[**최신 Release 다운로드**](../../releases/latest)

| 플랫폼 | 파일 |
|--------|------|
| macOS (Apple Silicon) | `soniox-caption-obs-x.x.x-macos-arm64.zip` |
| Windows | `soniox-caption-obs-x.x.x-windows-x64.zip` |
| Linux (Ubuntu) | `soniox-caption-obs-x.x.x-x86_64-linux-gnu.deb` |

### 3. 설치

<details>
<summary><b>macOS</b></summary>

1. `soniox-caption-obs-x.x.x-macos-arm64.zip` 다운로드 후 압축 해제
2. `soniox-caption-obs.plugin` 을 아래 경로로 복사:
   ```
   ~/Library/Application Support/obs-studio/plugins/
   ```
3. OBS Studio 재시작

**제거:**
```bash
rm -rf ~/Library/Application\ Support/obs-studio/plugins/soniox-caption-obs.plugin
```
</details>

<details>
<summary><b>Windows</b></summary>

1. `soniox-caption-obs-x.x.x-windows-x64.zip` 다운로드 후 압축 해제
2. 내용물을 아래 경로로 복사:
   ```
   %APPDATA%\obs-studio\plugins\soniox-caption-obs\
   ```
3. OBS Studio 재시작
</details>

<details>
<summary><b>Linux (Ubuntu)</b></summary>

```bash
sudo dpkg -i soniox-caption-obs-x.x.x-x86_64-linux-gnu.deb
```

또는 수동으로 `~/.config/obs-studio/plugins/soniox-caption-obs/` 에 복사
</details>

### 4. 사용법

1. OBS에서 소스 **+** 클릭 → **Soniox Captions** 선택
2. 소스 우클릭 → **속성**:
   - **Soniox API Key** 입력
   - **Audio Source** 에서 마이크 선택 (예: Mic/Aux)
   - **Language** 선택
   - (선택) **Enable Translation** 체크 후 번역 대상 언어 선택
3. **Start Caption** 클릭
4. 마이크에 말하면 실시간 자막이 화면에 표시됩니다!

### 단축키

**OBS 설정 → 단축키 → Toggle Soniox Captions** 에서 단축키를 지정하면 Properties를 열지 않고도 시작/중지할 수 있습니다.

---

## 소스 빌드

<details>
<summary>펼치기</summary>

### 사전 요구사항

- CMake 3.28+
- Xcode 16+ (macOS) / Visual Studio 2022 (Windows) / GCC 12+ (Linux)
- OpenSSL (macOS: `brew install openssl`)

### macOS

```bash
cmake --preset macos
cmake --build --preset macos
# 결과물: build_macos/RelWithDebInfo/soniox-caption-obs.plugin
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

모든 의존성(IXWebSocket, nlohmann/json, OBS SDK)은 CMake FetchContent를 통해 자동 다운로드됩니다.

</details>

## 라이선스

GPL-2.0 - [LICENSE](LICENSE) 참조
