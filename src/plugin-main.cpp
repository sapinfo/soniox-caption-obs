/*
 * Soniox Captions for OBS
 * Real-time speech-to-text captions using Soniox API
 *
 * 오디오 캡처 → Soniox WebSocket 전송 → 실시간 자막/번역 표시
 *
 * 스레드 모델:
 *  - UI/핫키 스레드: start/stop/test/update/destroy — control_mutex로 직렬화
 *  - WebSocket 스레드: 메시지 콜백 — session 스냅샷만 읽음 (설정 문자열 직접 접근 금지)
 *  - 오디오 스레드: audio_capture_callback — websocket은 콜백 해제 후에만 파괴
 *  - tick 스레드: keepalive 전송 + 자막 유지시간(hold) 만료 처리
 *  - 렌더 스레드: get_width/height/video_render — display_mode는 atomic
 */

#include <obs-module.h>
#include <media-io/audio-resampler.h>
#include <plugin-support.h>

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <nlohmann/json.hpp>

#ifdef __APPLE__
#include <Security/Security.h>
#endif

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <chrono>
#include <sstream>
#include <thread>

using json = nlohmann::json;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static const char *SONIOX_URL = "wss://stt-rt.soniox.com/transcribe-websocket";

// 오디오가 이 시간 이상 전송되지 않으면 keepalive 전송 (Soniox는 무음/무전송 시 연결을 닫음)
static constexpr int64_t KEEPALIVE_INTERVAL_MS = 5000;
// 발화 도중 오디오가 이 시간 이상 끊기면 (음소거/소스 정지) 수동 finalize — Soniox는 오디오가 없으면
// 끝점을 감지하지 못해 미완성 자막이 화면에 남는다
static constexpr int64_t AUDIO_GAP_FINALIZE_MS = 1000;
static constexpr int TICK_INTERVAL_MS = 250;

enum display_mode_t { DISPLAY_ORIGINAL = 0, DISPLAY_TRANSLATION = 1, DISPLAY_BOTH = 2 };

static display_mode_t parse_display_mode(const char *s)
{
	if (s && strcmp(s, "translation") == 0)
		return DISPLAY_TRANSLATION;
	if (s && strcmp(s, "both") == 0)
		return DISPLAY_BOTH;
	return DISPLAY_ORIGINAL;
}

// Soniox 문서상 재시도 가능한 에러 (408 timeout, 413 세션 최대 길이, 429 rate limit, 5xx)
// — 나머지(400/401/402/403 등)는 재연결해도 같은 결과
static bool is_retryable_soniox_error(int code)
{
	return code == 408 || code == 413 || code == 429 || code == 500 || code == 503;
}

// 세션 시작 시점의 설정 스냅샷 — WebSocket 스레드는 이 복사본만 읽는다
struct session_config {
	std::string api_key;
	std::string language{"ko"};
	std::string secondary_lang{"none"};
	std::string target_lang{"en"};
	std::string context_text;
	std::string context_general_raw; // "key=value" lines, parsed to JSON array at send time
	display_mode_t display_mode{DISPLAY_ORIGINAL};
	int max_endpoint_delay_ms{500}; // Soniox: 500~3000 (default 500 = faster finalize)
	int caption_hold_ms{4000};      // 발화 종료 후 자막 유지 시간 (0 = 다음 발화까지 유지)
	bool enable_diarization{false};
};

// 텍스트 스타일 — UI 스레드가 쓰고 WebSocket/tick 스레드가 읽으므로 style_mutex로 보호
struct text_style {
	int font_size{48};
	std::string font_face{"Apple SD Gothic Neo"};
	std::string font_style{"Regular"};
	int font_flags{0};
	uint32_t color1{0xFFFFFFFF}; // ABGR (OBS 내부 포맷)
	uint32_t color2{0xFFFFFFFF};
	bool outline{false};
	bool drop_shadow{false};
	int custom_width{0};
	bool word_wrap{false};
};

// ─── 소스 데이터 구조체 ───
struct soniox_caption_data {
	obs_source_t *source;
	obs_source_t *text_source;       // 원문용
	obs_source_t *text_source_trans; // 번역용

	// 핫키
	obs_hotkey_id hotkey_id{OBS_INVALID_HOTKEY_ID};

	// start/stop/test/update/destroy 직렬화 (UI 스레드 vs 핫키 스레드)
	std::mutex control_mutex;

	// 설정 (control_mutex 하에서만 접근)
	session_config settings;
	std::string audio_source_name;

	// 현재 세션 스냅샷 (start 시 복사, WebSocket 스레드 시작 전에 기록됨)
	session_config session;

	// 렌더 스레드용
	std::atomic<int> display_mode{DISPLAY_ORIGINAL};

	// 텍스트 스타일
	std::mutex style_mutex;
	text_style style;

	// WebSocket
	std::unique_ptr<ix::WebSocket> websocket;      // 캡셔닝용
	std::unique_ptr<ix::WebSocket> test_websocket; // 연결 테스트용 (캡셔닝과 분리)
	std::atomic<bool> test_running{false};          // 테스트 결과 문구를 update()가 덮어쓰지 않도록
	std::atomic<bool> connected{false};
	std::atomic<bool> captioning{false};
	std::atomic<bool> stopping{false};    // stop 중 Close 콜백 무시용
	std::atomic<bool> fatal_error{false}; // Soniox error_message 수신 (재연결 중단)

	// 오디오 캡처
	obs_source_t *audio_source{nullptr};

	// Audio resampler (OBS project SR/layout -> 16kHz mono s16le with proper anti-aliasing)
	audio_resampler_t *resampler{nullptr};

	// tick 스레드 (keepalive + hold 만료)
	std::thread tick_thread;
	std::mutex tick_mutex;
	std::condition_variable tick_cv;
	bool tick_stop{false};
	std::atomic<int64_t> last_send_ms{0};  // 오디오 + keepalive
	std::atomic<int64_t> last_audio_ms{0}; // 오디오만
	std::atomic<bool> finalize_sent{false}; // 오디오 끊김당 finalize 1회

	// 자막 상태 (text_mutex 보호)
	std::mutex text_mutex;
	std::string final_buffer;
	std::string final_trans_buffer; // 번역 확정 텍스트
	int turn_count{0};
	int last_speaker{-1}; // 화자 전환 감지용 (발화 경계에서 -1로 리셋)
	bool utterance_open{false}; // 원문 토큰 수신 후 <end>/<fin> 전까지 true

	// 번역 committed protection (확정 텍스트 1.5초 유지)
	std::chrono::steady_clock::time_point committed_protect_until;
	// protection 중 억제된 번역 — 만료 시 tick 스레드가 표시 (짧은 발화의 번역 유실 방지)
	std::string pending_trans;
	bool pending_trans_committed{false}; // 확정 번역이면 표시 후 다시 protection
	// 발화 종료 후 자막 지우기 예정 시각 (max = 예정 없음)
	std::chrono::steady_clock::time_point clear_deadline{std::chrono::steady_clock::time_point::max()};
};

static int64_t steady_now_ms()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

static std::string trim(std::string s)
{
	const char *ws = " \t\r\n";
	size_t b = s.find_first_not_of(ws);
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(ws);
	return s.substr(b, e - b + 1);
}

#ifdef __APPLE__
// IXWebSocket의 mbedTLS 백엔드는 macOS 시스템 인증서를 읽지 못함 ("SYSTEM" = Windows 전용)
// → 키체인의 시스템 루트 인증서를 PEM 번들로 만들어 메모리 CA로 전달 (최초 1회 생성)
static const std::string &macos_root_ca_pem()
{
	static const std::string pem = [] {
		std::string out;
		CFArrayRef anchors = nullptr;
		if (SecTrustCopyAnchorCertificates(&anchors) != errSecSuccess || !anchors)
			return out;
		CFDataRef data = nullptr;
		if (SecItemExport(anchors, kSecFormatPEMSequence, 0, nullptr, &data) == errSecSuccess && data) {
			out.assign(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
				   (size_t)CFDataGetLength(data));
			CFRelease(data);
		}
		obs_log(LOG_INFO, "Loaded %ld root certificates from keychain", (long)CFArrayGetCount(anchors));
		CFRelease(anchors);
		return out;
	}();
	return pem;
}
#endif

static void configure_tls(ix::WebSocket *ws)
{
#ifdef __APPLE__
	const std::string &pem = macos_root_ca_pem();
	if (pem.empty()) {
		obs_log(LOG_WARNING, "Could not load keychain root certificates — TLS will fail");
		return;
	}
	ix::SocketTLSOptions tls;
	tls.caFile = pem; // "-----BEGIN CERTIFICATE-----" 포함 → IXWebSocket이 메모리 CA로 인식
	ws->setTLSOptions(tls);
#else
	(void)ws; // Windows: mbedTLS가 시스템 저장소 사용, Linux: OpenSSL 기본 경로
#endif
}

// ─── Context "general" 파싱: "key=value" 라인 → JSON 배열 ───
static json parse_general(const std::string &raw)
{
	json arr = json::array();
	std::istringstream iss(raw);
	std::string line;
	while (std::getline(iss, line)) {
		line = trim(line);
		auto eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string k = trim(line.substr(0, eq));
		std::string v = trim(line.substr(eq + 1));
		if (k.empty())
			continue;
		arr.push_back({{"key", k}, {"value", v}});
	}
	return arr;
}

// ─── 텍스트 표시 업데이트 ───
static void update_source_text(soniox_caption_data *data, obs_source_t *src, const char *text)
{
	if (!src)
		return;

	text_style st;
	{
		std::lock_guard<std::mutex> lock(data->style_mutex);
		st = data->style;
	}

	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", st.font_face.c_str());
	obs_data_set_int(font, "size", st.font_size);
	obs_data_set_string(font, "style", st.font_style.c_str());
	obs_data_set_int(font, "flags", st.font_flags);

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "text", text);
	obs_data_set_obj(s, "font", font);

#ifdef _WIN32
	// text_gdiplus 속성
	obs_data_set_int(s, "color", st.color1);
	obs_data_set_int(s, "opacity", 100);
	obs_data_set_bool(s, "outline", st.outline);
	obs_data_set_int(s, "outline_size", 4);
	obs_data_set_int(s, "outline_color", 0x000000);
	obs_data_set_int(s, "outline_opacity", 100);
	if (st.custom_width > 0) {
		// gdiplus는 extents 크기를 그대로 소스 크기로 사용 (자동 높이 없음) — cy=0이면 아무것도
		// 렌더링되지 않으므로 최대 3줄 높이의 고정 박스를 지정
		obs_data_set_bool(s, "extents", true);
		obs_data_set_int(s, "extents_cx", st.custom_width);
		obs_data_set_int(s, "extents_cy", (int)(st.font_size * 1.4 * 3));
		obs_data_set_bool(s, "extents_wrap", st.word_wrap);
	} else {
		obs_data_set_bool(s, "extents", false);
	}
#else
	// text_ft2_source_v2 속성
	obs_data_set_int(s, "color1", st.color1);
	obs_data_set_int(s, "color2", st.color2);
	obs_data_set_bool(s, "outline", st.outline);
	obs_data_set_bool(s, "drop_shadow", st.drop_shadow);
	obs_data_set_int(s, "custom_width", st.custom_width);
	obs_data_set_bool(s, "word_wrap", st.word_wrap);
#endif

	obs_source_update(src, s);

	obs_data_release(font);
	obs_data_release(s);
}

static void update_text_display(soniox_caption_data *data, const char *text)
{
	update_source_text(data, data->text_source, text);
}

static void update_trans_display(soniox_caption_data *data, const char *text)
{
	update_source_text(data, data->text_source_trans, text);
}

// ─── 오디오 캡처 콜백 ───
// Uses OBS's audio_resampler to convert the project's actual SR/layout to
// 16kHz mono int16 with proper anti-aliasing, instead of naive 3:1 decimation
// (which would alias any content above ~5.3kHz into the audible band).
static void audio_capture_callback(void *param, obs_source_t *, const struct audio_data *audio,
				   bool muted)
{
	auto *data = static_cast<soniox_caption_data *>(param);

	// muted이면 전송하지 않음 — tick 스레드가 keepalive로 연결 유지
	if (!data->captioning || !data->connected || !data->websocket || muted)
		return;
	if (!data->resampler || !audio->data[0] || audio->frames == 0)
		return;

	uint8_t *output[MAX_AV_PLANES] = {0};
	uint32_t out_frames = 0;
	uint64_t ts_offset = 0;
	bool ok = audio_resampler_resample(data->resampler, output, &out_frames, &ts_offset,
					   reinterpret_cast<const uint8_t *const *>(audio->data),
					   audio->frames);
	if (!ok || out_frames == 0 || !output[0])
		return;

	data->websocket->sendBinary(std::string(reinterpret_cast<const char *>(output[0]),
						out_frames * sizeof(int16_t)));
	int64_t now = steady_now_ms();
	data->last_send_ms = now;
	data->last_audio_ms = now;
	data->finalize_sent = false;
}

// ─── Soniox 토큰 파싱 (WebSocket 스레드) ───
static void handle_soniox_message(soniox_caption_data *data, ix::WebSocket *ws, const std::string &msg_str)
{
	const session_config &cfg = data->session;

	try {
		json resp = json::parse(msg_str);

		if (resp.contains("error_message")) {
			std::string err = resp["error_message"];
			int code = resp.value("error_code", 0);
			obs_log(LOG_ERROR, "Soniox error (code=%d): %s", code, err.c_str());
			if (is_retryable_soniox_error(code)) {
				// 서버가 연결을 닫으면 자동 재연결이 새 세션을 연다
				update_text_display(data, ("Reconnecting: " + err).c_str());
				return;
			}
			// API 키 오류 등은 재시도해도 같은 결과 → 재연결 중단, 에러 문구 유지
			data->fatal_error = true;
			ws->disableAutomaticReconnection();
			update_text_display(data, ("Error: " + err).c_str());
			update_trans_display(data, "");
			return;
		}

		auto tokens = resp.value("tokens", json::array());
		if (tokens.empty())
			return;

		std::lock_guard<std::mutex> lock(data->text_mutex);
		auto now = std::chrono::steady_clock::now();

		std::string new_final;
		std::string non_final;
		std::string new_final_trans;
		std::string non_final_trans;
		bool got_end = false;

		for (const auto &token : tokens) {
			std::string text = token.value("text", "");

			// <end> = 발화 완료, <fin> = 수동 finalize 완료 (둘 다 발화 경계로 처리)
			if (text == "<end>" || text == "<fin>") {
				data->final_buffer += new_final;
				data->final_trans_buffer += new_final_trans;
				new_final.clear();
				new_final_trans.clear();

				std::string line = trim(data->final_buffer);
				if (!line.empty()) {
					data->turn_count++;
					std::string trans = trim(data->final_trans_buffer);
					if (!trans.empty())
						obs_log(LOG_INFO, "[Turn %d] %s → %s", data->turn_count,
							line.c_str(), trans.c_str());
					else
						obs_log(LOG_INFO, "[Turn %d] %s", data->turn_count, line.c_str());
				}

				if (cfg.display_mode != DISPLAY_ORIGINAL) {
					// protection 중에 끝난 발화: 번역이 화면에 못 나갔으면 만료 후 표시되도록 보관
					std::string trans = trim(data->final_trans_buffer);
					if (now < data->committed_protect_until && !trans.empty()) {
						data->pending_trans = trans;
						data->pending_trans_committed = true;
					} else
						data->committed_protect_until = now + std::chrono::milliseconds(1500);
				}
				// 확정된 줄은 화면에 남겨두고 hold 시간 후 지움 (다음 발화가 오면 즉시 교체)
				if (cfg.caption_hold_ms > 0)
					data->clear_deadline = now + std::chrono::milliseconds(cfg.caption_hold_ms);

				data->final_buffer.clear();
				data->final_trans_buffer.clear();
				data->last_speaker = -1; // 발화 경계에서 화자 리셋
				data->utterance_open = false;
				got_end = true;
				continue;
			}

			// 번역 토큰과 원문 토큰 분리
			bool is_trans = token.value("translation_status", "") == "translation";
			bool is_final = token.value("is_final", false);

			// 원문 토큰에 한해 화자 전환 시 "S{N}: " prefix 추가
			std::string prefix;
			if (cfg.enable_diarization && !is_trans && token.contains("speaker") &&
			    !token["speaker"].is_null()) {
				int spk = token.value("speaker", -1);
				if (spk >= 0 && spk != data->last_speaker) {
					prefix = "S" + std::to_string(spk) + ": ";
					data->last_speaker = spk;
				}
			}

			if (is_trans) {
				if (is_final)
					new_final_trans += text;
				else
					non_final_trans += text;
			} else {
				if (is_final)
					new_final += prefix + text;
				else
					non_final += prefix + text;
				data->utterance_open = true;
			}
		}

		data->final_buffer += new_final;
		data->final_trans_buffer += new_final_trans;

		std::string orig = trim(data->final_buffer + non_final);
		std::string trans = trim(data->final_trans_buffer + non_final_trans);
		bool trans_protected = now < data->committed_protect_until;

		// 새 텍스트가 들어오면 hold 타이머 연장 (늦게 도착하는 번역 토큰 포함)
		if (!got_end && (!orig.empty() || !trans.empty()) &&
		    data->clear_deadline != std::chrono::steady_clock::time_point::max()) {
			data->clear_deadline = cfg.caption_hold_ms > 0
						       ? now + std::chrono::milliseconds(cfg.caption_hold_ms)
						       : std::chrono::steady_clock::time_point::max();
		}

		if (cfg.display_mode != DISPLAY_ORIGINAL && !trans.empty()) {
			if (trans_protected) {
				data->pending_trans = trans;
				data->pending_trans_committed = false; // 진행 중 발화의 부분 번역
			} else {
				data->pending_trans.clear();
			}
		}

		if (cfg.display_mode == DISPLAY_BOTH) {
			// Both 모드: 원문과 번역을 각각 독립 업데이트
			if (!orig.empty())
				update_text_display(data, orig.c_str());
			if (!trans_protected && !trans.empty())
				update_trans_display(data, trans.c_str());
		} else if (cfg.display_mode == DISPLAY_TRANSLATION) {
			if (!trans_protected && !trans.empty())
				update_text_display(data, trans.c_str());
		} else {
			if (!orig.empty())
				update_text_display(data, orig.c_str());
		}

	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "JSON parse error: %s", e.what());
	}
}

// ─── tick 스레드: keepalive + hold 만료 ───
static void tick_loop(soniox_caption_data *data)
{
	std::unique_lock<std::mutex> lock(data->tick_mutex);
	while (!data->tick_stop) {
		data->tick_cv.wait_for(lock, std::chrono::milliseconds(TICK_INTERVAL_MS));
		if (data->tick_stop)
			break;

		if (data->connected && steady_now_ms() - data->last_send_ms >= KEEPALIVE_INTERVAL_MS) {
			data->websocket->sendText(R"({"type":"keepalive"})");
			data->last_send_ms = steady_now_ms();
		}

		std::lock_guard<std::mutex> tl(data->text_mutex);
		auto now = std::chrono::steady_clock::now();

		// 발화 도중 오디오 끊김 → 수동 finalize (응답의 <fin>이 발화 경계 처리)
		if (data->connected && data->utterance_open && !data->finalize_sent &&
		    steady_now_ms() - data->last_audio_ms >= AUDIO_GAP_FINALIZE_MS) {
			data->websocket->sendText(R"({"type":"finalize"})");
			data->last_send_ms = steady_now_ms();
			data->finalize_sent = true;
			obs_log(LOG_INFO, "Audio gap during speech — sent finalize");
		}

		// protection 만료 → 억제됐던 번역 표시
		if (!data->pending_trans.empty() && now >= data->committed_protect_until) {
			if (data->session.display_mode == DISPLAY_BOTH)
				update_trans_display(data, data->pending_trans.c_str());
			else
				update_text_display(data, data->pending_trans.c_str());
			data->pending_trans.clear();
			// 부분 번역은 다시 잠그지 않음 — 진행 중 발화의 번역이 1.5초 단위로 끊겨 갱신되지 않도록
			if (data->pending_trans_committed)
				data->committed_protect_until = now + std::chrono::milliseconds(1500);
			if (data->clear_deadline != std::chrono::steady_clock::time_point::max())
				data->clear_deadline = now + std::chrono::milliseconds(data->session.caption_hold_ms);
		}

		if (now >= data->clear_deadline) {
			data->clear_deadline = std::chrono::steady_clock::time_point::max();
			update_text_display(data, "");
			update_trans_display(data, "");
		}
	}
}

static void stop_tick_thread(soniox_caption_data *data)
{
	if (!data->tick_thread.joinable())
		return;
	{
		std::lock_guard<std::mutex> lock(data->tick_mutex);
		data->tick_stop = true;
	}
	data->tick_cv.notify_all();
	data->tick_thread.join();
}

// ─── 연결 테스트 정리 ───
static void stop_test_connection(soniox_caption_data *data)
{
	// 소멸자가 stop()으로 run 스레드를 join — 콜백 실행 중이면 끝날 때까지 대기
	data->test_websocket.reset();
	data->test_running = false;
}

// ─── 캡셔닝 중지 (control_mutex 보유 상태에서 호출) ───
static void stop_captioning(soniox_caption_data *data)
{
	if (!data->captioning)
		return;

	data->stopping = true;
	data->captioning = false;
	data->connected = false;

	// 1. 오디오 콜백 해제 — 이후 오디오 스레드는 websocket/resampler에 접근하지 않음
	if (data->audio_source) {
		obs_source_remove_audio_capture_callback(data->audio_source, audio_capture_callback, data);
		obs_source_release(data->audio_source);
		data->audio_source = nullptr;
	}

	// 2. tick 스레드 종료 — 이후 websocket에 접근하지 않음
	stop_tick_thread(data);

	// 3. WebSocket 종료 (run 스레드 join)
	if (data->websocket) {
		data->websocket->stop();
		data->websocket.reset();
	}

	if (data->resampler) {
		audio_resampler_destroy(data->resampler);
		data->resampler = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(data->text_mutex);
		data->clear_deadline = std::chrono::steady_clock::time_point::max();
	}

	data->stopping = false;
	update_text_display(data, "Soniox Captions Ready!");
	update_trans_display(data, "");
	obs_log(LOG_INFO, "Captioning stopped");
}

// ─── Soniox 세션 설정 JSON ───
static std::string build_config_json(const session_config &cfg)
{
	auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			      std::chrono::system_clock::now().time_since_epoch())
			      .count();

	json config;
	config["api_key"] = cfg.api_key;
	config["model"] = "stt-rt-v5";
	config["audio_format"] = "pcm_s16le";
	config["sample_rate"] = 16000;
	config["num_channels"] = 1;
	if (cfg.secondary_lang != "none" && cfg.secondary_lang != cfg.language)
		config["language_hints"] = {cfg.language, cfg.secondary_lang};
	else
		config["language_hints"] = {cfg.language};
	config["enable_language_identification"] = true;
	config["enable_endpoint_detection"] = true;
	config["max_endpoint_delay_ms"] = cfg.max_endpoint_delay_ms;
	config["client_reference_id"] = "obs_" + std::to_string(now_ms);

	if (cfg.enable_diarization)
		config["enable_speaker_diarization"] = true;

	// Context: 배경 서술(text) + 세션 메타데이터(general key=value pairs)
	json ctx;
	if (!cfg.context_text.empty())
		ctx["text"] = cfg.context_text;
	if (!cfg.context_general_raw.empty()) {
		json general = parse_general(cfg.context_general_raw);
		if (!general.empty())
			ctx["general"] = general;
	}
	if (!ctx.empty())
		config["context"] = ctx;

	// 번역 모드 또는 both 모드일 때 Soniox 내장 번역 추가
	if (cfg.display_mode != DISPLAY_ORIGINAL && !cfg.target_lang.empty()) {
		config["translation"] = {{"type", "one_way"}, {"target_language", cfg.target_lang}};
	}

	return config.dump();
}

// ─── 캡셔닝 시작 (control_mutex 보유 상태에서 호출) ───
static void start_captioning(soniox_caption_data *data)
{
	// 기존 연결 정리 (stop 플래그로 Close 콜백 무시)
	if (data->captioning)
		stop_captioning(data);
	stop_test_connection(data);

	if (data->settings.api_key.empty()) {
		update_text_display(data, "[Enter API Key first!]");
		return;
	}

	obs_source_t *audio_src = obs_get_source_by_name(data->audio_source_name.c_str());
	if (!audio_src) {
		update_text_display(data, "[Select Audio Source!]");
		return;
	}

	update_text_display(data, "Connecting...");
	update_trans_display(data, "");

	// Create resampler matching OBS project audio -> 16kHz mono s16le with
	// proper anti-aliasing. Honors the actual project SR (not hardcoded 48k).
	struct obs_audio_info oai = {};
	if (!obs_get_audio_info(&oai)) {
		oai.samples_per_sec = 48000;
		oai.speakers = SPEAKERS_STEREO;
	}
	struct resample_info src_info = {};
	src_info.samples_per_sec = oai.samples_per_sec;
	src_info.format = AUDIO_FORMAT_FLOAT_PLANAR;
	src_info.speakers = oai.speakers;
	struct resample_info dst_info = {};
	dst_info.samples_per_sec = 16000;
	dst_info.format = AUDIO_FORMAT_16BIT;
	dst_info.speakers = SPEAKERS_MONO;

	data->resampler = audio_resampler_create(&dst_info, &src_info);
	if (!data->resampler) {
		obs_log(LOG_ERROR, "Failed to create audio resampler (%u Hz -> 16 kHz)", oai.samples_per_sec);
		update_text_display(data, "Error: audio resampler init failed");
		obs_source_release(audio_src);
		return;
	}
	obs_log(LOG_INFO, "Audio resampler: %u Hz (%d ch) -> 16000 Hz mono", oai.samples_per_sec,
		(int)oai.speakers);

	// 세션 스냅샷 — 이후 WebSocket 스레드는 이 복사본만 읽음
	data->session = data->settings;
	data->fatal_error = false;

	{
		std::lock_guard<std::mutex> lock(data->text_mutex);
		data->final_buffer.clear();
		data->final_trans_buffer.clear();
		data->turn_count = 0;
		data->last_speaker = -1;
		data->utterance_open = false;
		data->clear_deadline = std::chrono::steady_clock::time_point::max();
		data->committed_protect_until = {};
		data->pending_trans.clear();
	}
	data->last_audio_ms = steady_now_ms();
	data->finalize_sent = false;

	// 1. WebSocket 먼저 설정
	data->websocket = std::make_unique<ix::WebSocket>();
	ix::WebSocket *ws = data->websocket.get();
	ws->setUrl(SONIOX_URL);
	configure_tls(ws);

	// 자동 재연결: 끊어지면 3초 후 재시도 (Soniox error_message 수신 시 중단)
	ws->enableAutomaticReconnection();
	ws->setMinWaitBetweenReconnectionRetries(3000);
	ws->setMaxWaitBetweenReconnectionRetries(30000);

	ws->setOnMessageCallback([data, ws](const ix::WebSocketMessagePtr &msg) {
		switch (msg->type) {
		case ix::WebSocketMessageType::Open: {
			obs_log(LOG_INFO, "Soniox WebSocket connected");
			{
				// 재연결 시 이전 세션의 미완성 버퍼 폐기
				std::lock_guard<std::mutex> lock(data->text_mutex);
				data->final_buffer.clear();
				data->final_trans_buffer.clear();
				data->last_speaker = -1;
				data->utterance_open = false;
			}

			std::string cfg = build_config_json(data->session);
			ws->send(cfg);
			data->last_send_ms = steady_now_ms();
			data->connected = true;
			obs_log(LOG_INFO, "Config sent (%zu bytes)", cfg.size());
			if (data->session.display_mode != DISPLAY_ORIGINAL)
				obs_log(LOG_INFO, "Translation enabled: %s → %s",
					data->session.language.c_str(), data->session.target_lang.c_str());

			update_text_display(data, "Listening...");
			break;
		}

		case ix::WebSocketMessageType::Message:
			handle_soniox_message(data, ws, msg->str);
			break;

		case ix::WebSocketMessageType::Error:
			obs_log(LOG_ERROR, "WS error: %s (status=%d)", msg->errorInfo.reason.c_str(),
				msg->errorInfo.http_status);
			data->connected = false;
			if (!data->fatal_error)
				update_text_display(data, ("Error: " + msg->errorInfo.reason).c_str());
			break;

		case ix::WebSocketMessageType::Close:
			obs_log(LOG_INFO, "WS closed (code=%d, reason=%s)", msg->closeInfo.code,
				msg->closeInfo.reason.c_str());
			data->connected = false;
			// stop 중이거나 Soniox 에러 문구를 표시 중이면 덮어쓰지 않음
			if (!data->stopping && data->captioning && !data->fatal_error) {
				std::string reason = msg->closeInfo.reason.empty() ? "Connection closed"
										   : msg->closeInfo.reason;
				update_text_display(data, reason.c_str());
			}
			break;

		default:
			break;
		}
	});

	// 2. 오디오 캡처 등록 (connected=false이므로 아직 전송 안 함)
	data->audio_source = audio_src;
	obs_source_add_audio_capture_callback(audio_src, audio_capture_callback, data);

	// 3. tick 스레드 시작
	data->tick_stop = false;
	data->tick_thread = std::thread(tick_loop, data);

	// 4. 캡셔닝 활성화 및 WebSocket 시작
	data->captioning = true;
	ws->start();
	obs_log(LOG_INFO, "Caption started, waiting for connection...");
}

// ─── 연결 테스트 (오디오 없이, control_mutex 보유 상태에서 호출) ───
static void test_connection(soniox_caption_data *data)
{
	stop_test_connection(data);

	if (data->settings.api_key.empty()) {
		update_text_display(data, "[Enter API Key first!]");
		return;
	}

	update_text_display(data, "Testing connection...");

	data->test_running = true;
	data->test_websocket = std::make_unique<ix::WebSocket>();
	ix::WebSocket *ws = data->test_websocket.get();
	ws->setUrl(SONIOX_URL);
	configure_tls(ws);
	ws->disableAutomaticReconnection();

	ws->setOnMessageCallback([data, ws](const ix::WebSocketMessagePtr &msg) {
		switch (msg->type) {
		case ix::WebSocketMessageType::Open:
			update_text_display(data, "Connected OK!");
			obs_log(LOG_INFO, "Test connection: OK");
			// close()는 비동기 — run 스레드 안에서 호출해도 self-join 없음.
			// 재연결이 꺼져 있으므로 close 완료 후 run 루프가 스스로 종료된다.
			ws->close();
			break;
		case ix::WebSocketMessageType::Error:
			update_text_display(data, ("Error: " + msg->errorInfo.reason).c_str());
			data->test_running = false;
			break;
		case ix::WebSocketMessageType::Close:
			update_text_display(data, "Test: Disconnected");
			data->test_running = false;
			break;
		default:
			break;
		}
	});

	ws->start();
}

// Centralized settings -> data loader. Used by create(), update(), button callbacks, and hotkey.
// Any new settings field belongs here (and only here) so all entry points stay in sync.
// Caller must hold control_mutex (or be in create()).
static void load_settings_into_data(soniox_caption_data *data, obs_data_t *settings)
{
	session_config &cfg = data->settings;
	cfg.api_key = obs_data_get_string(settings, "api_key");
	cfg.language = obs_data_get_string(settings, "language");
	cfg.secondary_lang = obs_data_get_string(settings, "secondary_lang");
	cfg.display_mode = parse_display_mode(obs_data_get_string(settings, "display_mode"));
	cfg.target_lang = obs_data_get_string(settings, "target_lang");
	cfg.max_endpoint_delay_ms = (int)obs_data_get_int(settings, "max_endpoint_delay_ms");
	cfg.caption_hold_ms = (int)obs_data_get_int(settings, "caption_hold_ms");
	cfg.enable_diarization = obs_data_get_bool(settings, "enable_diarization");
	cfg.context_text = obs_data_get_string(settings, "context_text");
	cfg.context_general_raw = obs_data_get_string(settings, "context_general_raw");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");

	// 렌더 레이아웃은 진행 중인 세션의 모드를 따름 (번역 config는 세션 시작 시 고정)
	if (!data->captioning)
		data->display_mode = cfg.display_mode;

	std::lock_guard<std::mutex> lock(data->style_mutex);
	text_style &st = data->style;
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		st.font_face = obs_data_get_string(font_obj, "face");
		st.font_style = obs_data_get_string(font_obj, "style");
		st.font_size = (int)obs_data_get_int(font_obj, "size");
		st.font_flags = (int)obs_data_get_int(font_obj, "flags");
		obs_data_release(font_obj);
	}
	st.color1 = (uint32_t)obs_data_get_int(settings, "color1");
	st.color2 = (uint32_t)obs_data_get_int(settings, "color2");
	st.outline = obs_data_get_bool(settings, "outline");
	st.drop_shadow = obs_data_get_bool(settings, "drop_shadow");
	st.custom_width = (int)obs_data_get_int(settings, "custom_width");
	st.word_wrap = obs_data_get_bool(settings, "word_wrap");
}

static void reload_settings(soniox_caption_data *data)
{
	obs_data_t *settings = obs_source_get_settings(data->source);
	load_settings_into_data(data, settings);
	obs_data_release(settings);
}

static void toggle_captioning(soniox_caption_data *data)
{
	reload_settings(data);
	if (data->captioning) {
		stop_captioning(data);
		// 캡셔닝 중 바뀐 표시 모드를 레이아웃에 반영 (세션 중에는 세션 모드를 유지했음)
		data->display_mode = data->settings.display_mode;
	} else {
		start_captioning(data);
		if (data->captioning)
			data->display_mode = data->session.display_mode;
	}
}

// ─── 핫키: Start/Stop Caption 토글 ───
static void hotkey_toggle_caption(void *private_data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *data = static_cast<soniox_caption_data *>(private_data);
	std::lock_guard<std::mutex> lock(data->control_mutex);
	toggle_captioning(data);
}

// ─── 콜백 함수들 ───

static const char *soniox_caption_get_name(void *)
{
	return "Soniox Captions";
}

static void *soniox_caption_create(obs_data_t *settings, obs_source_t *source)
{
	auto *data = new soniox_caption_data();
	data->source = source;

	load_settings_into_data(data, settings);

	obs_data_t *ts = obs_data_create();
	obs_data_set_string(ts, "text", "Soniox Captions Ready!");
#ifdef _WIN32
	data->text_source = obs_source_create_private("text_gdiplus", "soniox_text", ts);
#else
	data->text_source = obs_source_create_private("text_ft2_source_v2", "soniox_text", ts);
#endif
	obs_data_release(ts);

	// 번역용 텍스트 소스
	obs_data_t *ts2 = obs_data_create();
	obs_data_set_string(ts2, "text", "");
#ifdef _WIN32
	data->text_source_trans = obs_source_create_private("text_gdiplus", "soniox_text_trans", ts2);
#else
	data->text_source_trans = obs_source_create_private("text_ft2_source_v2", "soniox_text_trans", ts2);
#endif
	obs_data_release(ts2);

	// 핫키 등록 — OBS Settings > Hotkeys에서 키 할당 가능
	data->hotkey_id = obs_hotkey_register_source(source, "soniox_toggle_caption", "Toggle Soniox Captions",
						     hotkey_toggle_caption, data);

	obs_log(LOG_INFO, "caption source created");
	return data;
}

static void soniox_caption_destroy(void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	// 핫키를 먼저 해제해 destroy 중 핫키 콜백이 진입하지 않도록 함
	if (data->hotkey_id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(data->hotkey_id);
	{
		std::lock_guard<std::mutex> lock(data->control_mutex);
		stop_captioning(data);
		stop_test_connection(data);
	}
	if (data->text_source)
		obs_source_release(data->text_source);
	if (data->text_source_trans)
		obs_source_release(data->text_source_trans);
	delete data;
}

static void soniox_caption_update(void *private_data, obs_data_t *settings)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	std::lock_guard<std::mutex> lock(data->control_mutex);

	load_settings_into_data(data, settings);

	if (!data->captioning && !data->test_running) {
		if (!data->settings.api_key.empty())
			update_text_display(data, "Soniox Captions Ready!");
		else
			update_text_display(data, "[Set API Key in Properties]");
	}
}

// ─── 버튼 콜백들 ───
static bool on_test_clicked(obs_properties_t *, obs_property_t *, void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	std::lock_guard<std::mutex> lock(data->control_mutex);

	// 캡셔닝 중 테스트는 진행 중인 세션을 건드리지 않고 무시
	if (data->captioning) {
		obs_log(LOG_INFO, "Test connection ignored: captioning is running");
		return false;
	}

	reload_settings(data);
	test_connection(data);
	return false;
}

static bool on_start_stop_clicked(obs_properties_t *, obs_property_t *property, void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	std::lock_guard<std::mutex> lock(data->control_mutex);

	toggle_captioning(data);
	obs_property_set_description(property, data->captioning ? "Stop Caption" : "Start Caption");
	return true;
}

// ─── 오디오 소스 열거 ───
static bool enum_audio_sources(void *param, obs_source_t *source)
{
	auto *list = static_cast<obs_property_t *>(param);
	uint32_t flags = obs_source_get_output_flags(source);
	if (flags & OBS_SOURCE_AUDIO) {
		const char *name = obs_source_get_name(source);
		if (name && strlen(name) > 0)
			obs_property_list_add_string(list, name, name);
	}
	return true;
}

// ─── Soniox 지원 언어 (원문/보조/번역 목록 공통) ───
struct language_entry {
	const char *code;
	const char *name;
};

static const language_entry LANGUAGES[] = {
	{"ko", "Korean"},     {"en", "English"},    {"ja", "Japanese"},   {"zh", "Chinese"},
	{"es", "Spanish"},    {"fr", "French"},     {"de", "German"},     {"af", "Afrikaans"},
	{"sq", "Albanian"},   {"ar", "Arabic"},     {"az", "Azerbaijani"}, {"eu", "Basque"},
	{"be", "Belarusian"}, {"bn", "Bengali"},    {"bs", "Bosnian"},    {"bg", "Bulgarian"},
	{"ca", "Catalan"},    {"hr", "Croatian"},   {"cs", "Czech"},      {"da", "Danish"},
	{"nl", "Dutch"},      {"et", "Estonian"},   {"fi", "Finnish"},    {"gl", "Galician"},
	{"el", "Greek"},      {"gu", "Gujarati"},   {"he", "Hebrew"},     {"hi", "Hindi"},
	{"hu", "Hungarian"},  {"id", "Indonesian"}, {"it", "Italian"},    {"kn", "Kannada"},
	{"kk", "Kazakh"},     {"lv", "Latvian"},    {"lt", "Lithuanian"}, {"mk", "Macedonian"},
	{"ms", "Malay"},      {"ml", "Malayalam"},  {"mr", "Marathi"},    {"no", "Norwegian"},
	{"fa", "Persian"},    {"pl", "Polish"},     {"pt", "Portuguese"}, {"pa", "Punjabi"},
	{"ro", "Romanian"},   {"ru", "Russian"},    {"sr", "Serbian"},    {"sk", "Slovak"},
	{"sl", "Slovenian"},  {"sw", "Swahili"},    {"sv", "Swedish"},    {"tl", "Tagalog"},
	{"ta", "Tamil"},      {"te", "Telugu"},     {"th", "Thai"},       {"tr", "Turkish"},
	{"uk", "Ukrainian"},  {"ur", "Urdu"},       {"vi", "Vietnamese"}, {"cy", "Welsh"},
};

static void add_language_items(obs_property_t *list)
{
	for (const auto &l : LANGUAGES)
		obs_property_list_add_string(list, l.name, l.code);
}

// Properties UI
static obs_properties_t *soniox_caption_get_properties(void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	obs_properties_t *props = obs_properties_create();

	// API Key
	obs_properties_add_text(props, "api_key", "Soniox API Key", OBS_TEXT_PASSWORD);

	// 오디오 소스 선택
	obs_property_t *audio_list = obs_properties_add_list(props, "audio_source", "Audio Source",
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(audio_list, "(Select audio source)", "");
	obs_enum_sources(enum_audio_sources, audio_list);

	// 언어 선택
	obs_property_t *lang = obs_properties_add_list(props, "language", "Language", OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	add_language_items(lang);

	// 보조 언어 힌트 (code-switching 지원: 한+영, 한+일 등)
	obs_property_t *lang2 = obs_properties_add_list(props, "secondary_lang", "Secondary Language Hint",
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(lang2, "(None)", "none");
	add_language_items(lang2);

	// 표시 모드 (원문/번역 토글)
	obs_property_t *mode = obs_properties_add_list(props, "display_mode", "Display Mode", OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, "Original", "original");
	obs_property_list_add_string(mode, "Translation", "translation");
	obs_property_list_add_string(mode, "Both (Original + Translation)", "both");
	obs_property_set_modified_callback(mode, [](obs_properties_t *ps, obs_property_t *, obs_data_t *s) -> bool {
		const char *dm = obs_data_get_string(s, "display_mode");
		bool needs_trans = dm && strcmp(dm, "original") != 0;
		obs_property_set_visible(obs_properties_get(ps, "target_lang"), needs_trans);
		return true;
	});

	obs_property_t *target = obs_properties_add_list(props, "target_lang", "Translate To", OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_STRING);
	add_language_items(target);

	// 초기 가시성: display_mode가 "original"이 아닐 때 target_lang 표시
	if (data)
		obs_property_set_visible(target, data->display_mode != DISPLAY_ORIGINAL);

	// 엔드포인트(사일런스) 감도: 낮을수록 더 빨리 캡션 확정
	obs_properties_add_int_slider(props, "max_endpoint_delay_ms", "Endpoint Delay (ms)", 500, 3000, 50);

	// 발화 종료 후 자막 유지 시간 (0 = 다음 발화까지 유지)
	obs_properties_add_int_slider(props, "caption_hold_ms", "Caption Hold After Speech (ms, 0=keep)", 0,
				      15000, 500);

	// 화자 분리 (Beta): Soniox가 token.speaker 제공, 전환 시 "S{N}:" 접두
	obs_properties_add_bool(props, "enable_diarization", "Enable Speaker Diarization (Beta)");

	// Context: 도메인 정확도 향상 (Soniox에 배경 힌트 전달)
	obs_properties_add_text(props, "context_text", "Context Text (background narrative)", OBS_TEXT_MULTILINE);
	obs_properties_add_text(props, "context_general_raw", "Context Metadata (one 'key=value' per line)",
				OBS_TEXT_MULTILINE);

	// ─── 텍스트 스타일 ───

	// 폰트 선택 (시스템 폰트 다이얼로그)
	obs_properties_add_font(props, "font", "Font");

	// 텍스트 색상
	obs_properties_add_color(props, "color1", "Text Color");
	obs_properties_add_color(props, "color2", "Text Color 2 (Gradient)");

	// 텍스트 효과
	obs_properties_add_bool(props, "outline", "Outline");
	obs_properties_add_bool(props, "drop_shadow", "Drop Shadow");

	// 텍스트 레이아웃
	obs_properties_add_int(props, "custom_width", "Custom Text Width (0=auto)", 0, 4096, 1);
	obs_properties_add_bool(props, "word_wrap", "Word Wrap");

	// 버튼들
	obs_properties_add_button(props, "test_connection", "Test Connection", on_test_clicked);

	const char *btn_text = (data && data->captioning) ? "Stop Caption" : "Start Caption";
	obs_properties_add_button(props, "start_stop", btn_text, on_start_stop_clicked);

	return props;
}

static void soniox_caption_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "api_key", "");
	obs_data_set_default_string(settings, "language", "ko");
	obs_data_set_default_string(settings, "secondary_lang", "none");
	obs_data_set_default_string(settings, "audio_source", "");
	obs_data_set_default_string(settings, "display_mode", "original");
	obs_data_set_default_string(settings, "target_lang", "en");
	obs_data_set_default_int(settings, "max_endpoint_delay_ms", 500);
	obs_data_set_default_int(settings, "caption_hold_ms", 4000);
	obs_data_set_default_bool(settings, "enable_diarization", false);
	obs_data_set_default_string(settings, "context_text", "");
	obs_data_set_default_string(settings, "context_general_raw", "");

	// 폰트 기본값 (obs_data_t 오브젝트)
	obs_data_t *font_obj = obs_data_create();
#if defined(_WIN32)
	obs_data_set_default_string(font_obj, "face", "Malgun Gothic");
#elif defined(__APPLE__)
	obs_data_set_default_string(font_obj, "face", "Apple SD Gothic Neo");
#else
	obs_data_set_default_string(font_obj, "face", "Noto Sans CJK KR");
#endif
	obs_data_set_default_string(font_obj, "style", "Regular");
	obs_data_set_default_int(font_obj, "size", 48);
	obs_data_set_default_int(font_obj, "flags", 0);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);

	// 텍스트 스타일 기본값
	obs_data_set_default_int(settings, "color1", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "color2", 0xFFFFFFFF);
	obs_data_set_default_bool(settings, "outline", false);
	obs_data_set_default_bool(settings, "drop_shadow", false);
	obs_data_set_default_int(settings, "custom_width", 0);
	obs_data_set_default_bool(settings, "word_wrap", false);
}

static uint32_t soniox_caption_get_width(void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	uint32_t w1 = data->text_source ? obs_source_get_width(data->text_source) : 0;
	if (data->display_mode == DISPLAY_BOTH && data->text_source_trans) {
		uint32_t w2 = obs_source_get_width(data->text_source_trans);
		return w1 > w2 ? w1 : w2;
	}
	return w1;
}

static uint32_t soniox_caption_get_height(void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	uint32_t h1 = data->text_source ? obs_source_get_height(data->text_source) : 0;
	if (data->display_mode == DISPLAY_BOTH && data->text_source_trans) {
		uint32_t h2 = obs_source_get_height(data->text_source_trans);
		return h1 + h2;
	}
	return h1;
}

static void soniox_caption_video_render(void *private_data, gs_effect_t *)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	if (data->text_source)
		obs_source_video_render(data->text_source);
	if (data->display_mode == DISPLAY_BOTH && data->text_source_trans) {
		uint32_t h1 = data->text_source ? obs_source_get_height(data->text_source) : 0;
		gs_matrix_push();
		gs_matrix_translate3f(0.0f, (float)h1, 0.0f);
		obs_source_video_render(data->text_source_trans);
		gs_matrix_pop();
	}
}

// ─── 소스 등록 ───
static obs_source_info soniox_caption_source_info = {};

bool obs_module_load(void)
{
	soniox_caption_source_info.id = "soniox_caption";
	soniox_caption_source_info.type = OBS_SOURCE_TYPE_INPUT;
	soniox_caption_source_info.output_flags = OBS_SOURCE_VIDEO;
	soniox_caption_source_info.get_name = soniox_caption_get_name;
	soniox_caption_source_info.create = soniox_caption_create;
	soniox_caption_source_info.destroy = soniox_caption_destroy;
	soniox_caption_source_info.update = soniox_caption_update;
	soniox_caption_source_info.get_properties = soniox_caption_get_properties;
	soniox_caption_source_info.get_defaults = soniox_caption_get_defaults;
	soniox_caption_source_info.get_width = soniox_caption_get_width;
	soniox_caption_source_info.get_height = soniox_caption_get_height;
	soniox_caption_source_info.video_render = soniox_caption_video_render;

	obs_register_source(&soniox_caption_source_info);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
