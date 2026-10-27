# E2E 테스트 도구

실제 Soniox API와 실제 OBS를 사용해 플러그인을 자동 검증하는 스크립트입니다.

| 스크립트 | 용도 |
|---|---|
| `soniox_probe.py` | OBS 없이 Soniox에 직접 WAV를 실시간 스트리밍 → 토큰 순서/번역/keepalive/에러 확인 |
| `obs_e2e.py` | obs-websocket으로 OBS를 조작 → 소스 생성, 버튼 클릭, 스크린샷 캡처, 크래시 시나리오 |

API 키는 프로젝트 루트의 `.env` (`SONIOX_API_KEY=...`, gitignore됨)에서 읽으며 출력하지 않습니다.

## 1. 준비

```bash
# Python venv (프로젝트 밖 아무 곳)
python3 -m venv ~/soniox-venv
~/soniox-venv/bin/pip install websockets obsws-python      # Windows: ...\Scripts\pip.exe
```

### 테스트 음성 WAV 만들기 (16kHz mono 16-bit)

**macOS** (Yuna = 한국어 음성, 없으면 시스템 설정 → 손쉬운 사용 → 콘텐츠 읽기 → 음성에서 다운로드):
```bash
say -v Yuna -o /tmp/test_ko.aiff "안녕하세요. 오늘은 실시간 자막 플러그인을 테스트하고 있습니다. [[slnc 2000]] 날씨가 정말 좋네요. 점심은 김치찌개를 먹을 예정입니다. [[slnc 2000]] 이 문장이 마지막 문장입니다. 감사합니다. [[slnc 1000]]"
afconvert -f WAVE -d LEI16@16000 -c 1 /tmp/test_ko.aiff /tmp/test_ko.wav
```

**Windows** (PowerShell, Heami = 한국어 음성). `PromptBuilder`에 ko-KR 컬처를 꼭 지정 — 안 하면 한국어가 무음으로 렌더링됨:
```powershell
Add-Type -AssemblyName System.Speech
$syn = New-Object System.Speech.Synthesis.SpeechSynthesizer
$syn.SelectVoice("Microsoft Heami Desktop")
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$syn.SetOutputToWaveFile("$env:TEMP\test_ko.wav", $fmt)
$pb = New-Object System.Speech.Synthesis.PromptBuilder([Globalization.CultureInfo]::GetCultureInfo("ko-KR"))
$pb.AppendText("안녕하세요. 오늘은 실시간 자막 플러그인을 테스트하고 있습니다."); $pb.AppendBreak([TimeSpan]::FromSeconds(2))
$pb.AppendText("날씨가 정말 좋네요. 점심은 김치찌개를 먹을 예정입니다."); $pb.AppendBreak([TimeSpan]::FromSeconds(2))
$pb.AppendText("이 문장이 마지막 문장입니다. 감사합니다."); $pb.AppendBreak([TimeSpan]::FromSeconds(1))
$syn.Speak($pb); $syn.Dispose()
```

결과가 약 21초여야 정상입니다 (5초면 무음 — 위 컬처 문제).

## 2. Soniox 프로토콜 프로브 (OBS 불필요)

```bash
P=~/soniox-venv/bin/python
$P tools/e2e/soniox_probe.py .env /tmp/test_ko.wav --translate en   # 전사 + 번역 토큰 순서
$P tools/e2e/soniox_probe.py .env /tmp/test_ko.wav --idle 35        # keepalive로 35초 무음 유지
$P tools/e2e/soniox_probe.py .env /tmp/test_ko.wav --bad-key        # 401 에러 응답 형식
```

출력 형식: `O`=원문 / `T`=번역, `F`=final / `p`=partial.

## 3. OBS E2E

1. 플러그인 빌드 후 설치, OBS 실행
2. OBS → **도구 → WebSocket 서버 설정** → 서버 활성화 (포트 4455, 인증 사용 그대로)
3. 실행:
   ```bash
   $P tools/e2e/obs_e2e.py .env /tmp/test_ko.wav /tmp/shots --mode both            # 전체 시나리오 (~100초)
   $P tools/e2e/obs_e2e.py .env /tmp/test_ko.wav /tmp/shots --mode translation --quick
   $P tools/e2e/obs_e2e.py .env /tmp/test_ko.wav /tmp/shots --mode original --quick
   ```
4. 확인:
   - 스크립트 마지막에 `E2E DONE` / `QUICK DONE`, OBS 프로세스 생존
   - OBS 로그 (macOS: `~/Library/Application Support/obs-studio/logs/`, Windows: `%APPDATA%\obs-studio\logs\`)에서 `[soniox-caption-obs]` 라인
   - `/tmp/shots/*.png` — 자막이 실제로 렌더링되는지 (배경은 남색 color source)

시작 시 나오는 `RemoveInput ... code 600` 에러는 이전 테스트 잔여물 정리 시도라 무시해도 됩니다.

### 전체 시나리오가 검증하는 것

| 단계 | 내용 | 기대 결과 |
|---|---|---|
| 1 | 캡션 시작 → 음성 재생, 8초 시점 Test 버튼 3회 | 자막/번역 표시, 로그 `Test connection ignored` |
| 2 | 음성 종료 후 30초 무음 → 재생 | `WS closed` 없음, 재생 후 다시 전사 |
| 3 | 정지 후 Test 5회 연타, Test 직후 소스 삭제 | 크래시 없음 |
| 4 | 잘못된 키로 시작, 12초 대기 | 401 에러 1회, 재연결 없음 |
| 5 | Start/Stop 6회 연타 후 소스 삭제 | 크래시 없음 |
