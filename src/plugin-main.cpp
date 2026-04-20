/*
 * Soniox Captions for OBS
 * Real-time speech-to-text captions using Soniox API
 *
 * Step 5+6 v2: 오디오 캡처 → Soniox 전송 → 실시간 자막 표시
 */

#include <obs-module.h>
#include <plugin-support.h>

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>

using json = nlohmann::json;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// ─── 소스 데이터 구조체 ───
struct soniox_caption_data {
	obs_source_t *source;
	obs_source_t *text_source;       // 원문용
	obs_source_t *text_source_trans; // 번역용

	// 핫키
	obs_hotkey_id hotkey_id{OBS_INVALID_HOTKEY_ID};

	// WebSocket
	std::unique_ptr<ix::WebSocket> websocket;
	std::atomic<bool> connected{false};
	std::atomic<bool> captioning{false};
	std::atomic<bool> stopping{false}; // stop 중 Close 콜백 무시용

	// 오디오 캡처
	obs_source_t *audio_source{nullptr};
	std::string audio_source_name;

	// 자막 상태
	std::mutex text_mutex;
	std::string final_buffer;
	std::string final_trans_buffer; // 번역 확정 텍스트
	std::string partial_text;
	int turn_count{0};

	// 설정
	int font_size{48};
	std::string font_face{"Apple SD Gothic Neo"};
	std::string font_style{"Regular"};
	int font_flags{0};
	std::string api_key;
	std::string language{"ko"};
	std::string display_mode{"original"}; // "original", "translation", or "both"
	std::string target_lang{"en"};
	int max_endpoint_delay_ms{500}; // Soniox: 500~3000 (default 500 = faster finalize)

	// 번역 committed protection (확정 텍스트 1.5초 유지)
	std::chrono::steady_clock::time_point committed_protect_until;

	// 텍스트 스타일
	uint32_t color1{0xFFFFFFFF}; // ABGR (OBS 내부 포맷)
	uint32_t color2{0xFFFFFFFF};
	bool outline{false};
	bool drop_shadow{false};
	int custom_width{0};
	bool word_wrap{false};
};

// ─── 텍스트 표시 업데이트 ───
static void update_source_text(soniox_caption_data *data, obs_source_t *src, const char *text)
{
	if (!src)
		return;

	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", data->font_face.c_str());
	obs_data_set_int(font, "size", data->font_size);
	obs_data_set_string(font, "style", data->font_style.c_str());
	obs_data_set_int(font, "flags", data->font_flags);

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "text", text);
	obs_data_set_obj(s, "font", font);

#ifdef _WIN32
	// text_gdiplus 속성
	obs_data_set_int(s, "color", data->color1);
	obs_data_set_int(s, "opacity", 100);
	obs_data_set_bool(s, "outline", data->outline);
	obs_data_set_int(s, "outline_size", 4);
	obs_data_set_int(s, "outline_color", 0x000000);
	obs_data_set_int(s, "outline_opacity", 100);
	if (data->custom_width > 0) {
		obs_data_set_bool(s, "extents", true);
		obs_data_set_int(s, "extents_cx", data->custom_width);
		obs_data_set_int(s, "extents_cy", 0);
		obs_data_set_bool(s, "extents_wrap", data->word_wrap);
	} else {
		obs_data_set_bool(s, "extents", false);
	}
#else
	// text_ft2_source_v2 속성
	obs_data_set_int(s, "color1", data->color1);
	obs_data_set_int(s, "color2", data->color2);
	obs_data_set_bool(s, "outline", data->outline);
	obs_data_set_bool(s, "drop_shadow", data->drop_shadow);
	obs_data_set_int(s, "custom_width", data->custom_width);
	obs_data_set_bool(s, "word_wrap", data->word_wrap);
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
static void audio_capture_callback(void *param, obs_source_t *, const struct audio_data *audio,
				   bool muted)
{
	auto *data = static_cast<soniox_caption_data *>(param);

	if (!data->captioning || !data->connected || !data->websocket || muted)
		return;
	if (!audio->data[0] || audio->frames == 0)
		return;

	// OBS: float32, 48000Hz → Soniox: pcm_s16le, 16000Hz
	const float *src = reinterpret_cast<const float *>(audio->data[0]);
	uint32_t src_frames = audio->frames;
	uint32_t dst_frames = src_frames / 3;
	if (dst_frames == 0)
		return;

	std::vector<int16_t> pcm16(dst_frames);
	for (uint32_t i = 0; i < dst_frames; i++) {
		float sample = src[i * 3];
		if (sample > 1.0f)
			sample = 1.0f;
		if (sample < -1.0f)
			sample = -1.0f;
		pcm16[i] = static_cast<int16_t>(sample * 32767.0f);
	}

	data->websocket->sendBinary(
		std::string(reinterpret_cast<const char *>(pcm16.data()), pcm16.size() * sizeof(int16_t)));
}

// ─── Soniox 토큰 파싱 ───
static void handle_soniox_message(soniox_caption_data *data, const std::string &msg_str)
{
	try {
		json resp = json::parse(msg_str);

		if (resp.contains("error_message")) {
			std::string err = resp["error_message"];
			obs_log(LOG_ERROR, "Soniox error: %s", err.c_str());
			update_text_display(data, ("Error: " + err).c_str());
			return;
		}

		auto tokens = resp.value("tokens", json::array());
		if (tokens.empty())
			return;

		std::lock_guard<std::mutex> lock(data->text_mutex);

		std::string new_final;
		std::string non_final;
		std::string new_final_trans;
		std::string non_final_trans;

		for (const auto &token : tokens) {
			std::string text = token.value("text", "");

			// <end> = 발화 완료
			if (text == "<end>") {
				std::string line = data->final_buffer;
				while (!line.empty() && line.front() == ' ')
					line.erase(line.begin());
				while (!line.empty() && line.back() == ' ')
					line.pop_back();

				if (!line.empty()) {
					data->turn_count++;
					std::string trans = data->final_trans_buffer;
					while (!trans.empty() && trans.front() == ' ')
						trans.erase(trans.begin());
					if (!trans.empty())
						obs_log(LOG_INFO, "[Turn %d] %s → %s",
							data->turn_count, line.c_str(),
							trans.c_str());
					else
						obs_log(LOG_INFO, "[Turn %d] %s",
							data->turn_count, line.c_str());
				}

				bool has_trans = data->display_mode == "translation" || data->display_mode == "both";
				if (has_trans) {
					// 번역 committed protection: 1.5초 유지
					data->committed_protect_until =
						std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
				}
				if (data->display_mode != "translation") {
					// 원문 표시 클리어 (translation 전용 모드가 아닐 때)
					update_text_display(data, "");
				}
				data->final_buffer.clear();
				data->final_trans_buffer.clear();
				data->partial_text.clear();
				continue;
			}

			// 번역 토큰과 원문 토큰 분리
			bool is_trans =
				token.value("translation_status", "") == "translation";
			bool is_final = token.value("is_final", false);

			if (is_trans) {
				if (is_final)
					new_final_trans += text;
				else
					non_final_trans += text;
			} else {
				if (is_final)
					new_final += text;
				else
					non_final += text;
			}
		}

		if (!new_final.empty())
			data->final_buffer += new_final;
		if (!new_final_trans.empty())
			data->final_trans_buffer += new_final_trans;

		bool trans_protected = std::chrono::steady_clock::now() < data->committed_protect_until;

		if (data->display_mode == "both") {
			// Both 모드: 원문과 번역을 각각 독립 업데이트
			std::string orig = data->final_buffer + non_final;
			while (!orig.empty() && orig.front() == ' ')
				orig.erase(orig.begin());
			if (!orig.empty()) {
				data->partial_text = orig;
				update_text_display(data, orig.c_str());
			}

			if (!trans_protected) {
				std::string trans = data->final_trans_buffer + non_final_trans;
				while (!trans.empty() && trans.front() == ' ')
					trans.erase(trans.begin());
				update_trans_display(data, trans.empty() ? "" : trans.c_str());
			}
		} else if (data->display_mode == "translation") {
			if (!trans_protected) {
				std::string display = data->final_trans_buffer + non_final_trans;
				while (!display.empty() && display.front() == ' ')
					display.erase(display.begin());
				if (!display.empty()) {
					data->partial_text = display;
					update_text_display(data, display.c_str());
				}
			}
		} else {
			// Original 모드
			std::string display = data->final_buffer + non_final;
			while (!display.empty() && display.front() == ' ')
				display.erase(display.begin());
			if (!display.empty()) {
				data->partial_text = display;
				update_text_display(data, display.c_str());
			}
		}

	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "JSON parse error: %s", e.what());
	}
}

// ─── 캡셔닝 중지 ───
static void stop_captioning(soniox_caption_data *data)
{
	if (!data->captioning)
		return;

	data->stopping = true;
	data->captioning = false;
	data->connected = false;

	if (data->audio_source) {
		obs_source_remove_audio_capture_callback(data->audio_source, audio_capture_callback,
							 data);
		obs_source_release(data->audio_source);
		data->audio_source = nullptr;
	}

	if (data->websocket) {
		data->websocket->stop();
		data->websocket.reset();
	}

	data->stopping = false;
	update_text_display(data, "Soniox Captions Ready!");
	update_trans_display(data, "");
	obs_log(LOG_INFO, "Captioning stopped");
}

// ─── 캡셔닝 시작 ───
static void start_captioning(soniox_caption_data *data)
{
	// 기존 연결 정리 (stop 플래그로 Close 콜백 무시)
	if (data->captioning)
		stop_captioning(data);

	if (data->api_key.empty()) {
		update_text_display(data, "[Enter API Key first!]");
		return;
	}

	obs_source_t *audio_src = obs_get_source_by_name(data->audio_source_name.c_str());
	if (!audio_src) {
		update_text_display(data, "[Select Audio Source!]");
		return;
	}

	update_text_display(data, "Connecting...");

	{
		std::lock_guard<std::mutex> lock(data->text_mutex);
		data->final_buffer.clear();
		data->final_trans_buffer.clear();
		data->partial_text.clear();
		data->turn_count = 0;
	}

	// 1. WebSocket 먼저 설정
	data->websocket = std::make_unique<ix::WebSocket>();
	data->websocket->setUrl("wss://stt-rt.soniox.com/transcribe-websocket");

	// 자동 재연결: 끊어지면 3초 후 재시도
	data->websocket->enableAutomaticReconnection();
	data->websocket->setMinWaitBetweenReconnectionRetries(3000);
	data->websocket->setMaxWaitBetweenReconnectionRetries(30000);

	std::string key = data->api_key;
	std::string lang = data->language;

	data->websocket->setOnMessageCallback([data, key, lang](const ix::WebSocketMessagePtr &msg) {
		switch (msg->type) {
		case ix::WebSocketMessageType::Open: {
			obs_log(LOG_INFO, "Soniox WebSocket connected");
			data->connected = true;

			json config;
			config["api_key"] = key;
			config["model"] = "stt-rt-v4";
			config["audio_format"] = "pcm_s16le";
			config["sample_rate"] = 16000;
			config["num_channels"] = 1;
			config["language_hints"] = {lang};
			config["enable_language_identification"] = true;
			config["enable_endpoint_detection"] = true;
			config["max_endpoint_delay_ms"] = data->max_endpoint_delay_ms;

			// 번역 모드 또는 both 모드일 때 Soniox 내장 번역 추가
			if (data->display_mode != "original" && !data->target_lang.empty()) {
				config["translation"] = {
					{"type", "one_way"},
					{"target_language", data->target_lang}};
				obs_log(LOG_INFO, "Translation enabled: %s → %s",
					lang.c_str(), data->target_lang.c_str());
			}

			std::string cfg = config.dump();
			data->websocket->send(cfg);
			obs_log(LOG_INFO, "Config sent (%zu bytes)", cfg.size());

			update_text_display(data, "Listening...");
			break;
		}

		case ix::WebSocketMessageType::Message:
			handle_soniox_message(data, msg->str);
			break;

		case ix::WebSocketMessageType::Error:
			obs_log(LOG_ERROR, "WS error: %s (status=%d)",
				msg->errorInfo.reason.c_str(),
				msg->errorInfo.http_status);
			data->connected = false;
			update_text_display(data,
					    ("Error: " + msg->errorInfo.reason).c_str());
			break;

		case ix::WebSocketMessageType::Close:
			obs_log(LOG_INFO, "WS closed (code=%d, reason=%s)",
				msg->closeInfo.code,
				msg->closeInfo.reason.c_str());
			data->connected = false;
			// stop 중이면 무시 (stop_captioning이 텍스트를 설정함)
			if (!data->stopping && data->captioning) {
				std::string reason = msg->closeInfo.reason.empty()
							    ? "Connection closed"
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

	// 3. 캡셔닝 활성화 및 WebSocket 시작
	data->captioning = true;
	data->websocket->start();
	obs_log(LOG_INFO, "Caption started, waiting for connection...");
}

// ─── 연결 테스트 (오디오 없이) ───
static void test_connection(soniox_caption_data *data)
{
	if (data->websocket) {
		data->websocket->stop();
		data->websocket.reset();
		data->connected = false;
	}

	if (data->api_key.empty()) {
		update_text_display(data, "[Enter API Key first!]");
		return;
	}

	update_text_display(data, "Testing connection...");

	data->websocket = std::make_unique<ix::WebSocket>();
	data->websocket->setUrl("wss://stt-rt.soniox.com/transcribe-websocket");
	data->websocket->disableAutomaticReconnection();

	std::string key = data->api_key;
	std::string lang = data->language;

	data->websocket->setOnMessageCallback([data, key, lang](const ix::WebSocketMessagePtr &msg) {
		switch (msg->type) {
		case ix::WebSocketMessageType::Open: {
			data->connected = true;
			update_text_display(data, "Connected OK!");
			obs_log(LOG_INFO, "Test connection: OK");
			data->websocket->stop();
			break;
		}
		case ix::WebSocketMessageType::Message: {
			try {
				json resp = json::parse(msg->str);
				if (resp.contains("error_message"))
					update_text_display(
						data,
						("Error: " + resp["error_message"].get<std::string>())
							.c_str());
				else
					update_text_display(data, "Connected! Ready.");
			} catch (...) {
			}
			break;
		}
		case ix::WebSocketMessageType::Error:
			update_text_display(data, ("Error: " + msg->errorInfo.reason).c_str());
			break;
		case ix::WebSocketMessageType::Close:
			if (!data->stopping)
				update_text_display(data, "Test: Disconnected");
			data->connected = false;
			break;
		default:
			break;
		}
	});

	data->websocket->start();
}

// ─── 콜백 함수들 ───

// ─── 핫키: Start/Stop Caption 토글 ───
static void hotkey_toggle_caption(void *private_data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *data = static_cast<soniox_caption_data *>(private_data);

	obs_data_t *settings = obs_source_get_settings(data->source);
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->display_mode = obs_data_get_string(settings, "display_mode");
	data->target_lang = obs_data_get_string(settings, "target_lang");
	data->max_endpoint_delay_ms = (int)obs_data_get_int(settings, "max_endpoint_delay_ms");
	obs_data_release(settings);

	if (data->captioning)
		stop_captioning(data);
	else
		start_captioning(data);
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

	// 폰트 설정 읽기
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		data->font_face = obs_data_get_string(font_obj, "face");
		data->font_style = obs_data_get_string(font_obj, "style");
		data->font_size = (int)obs_data_get_int(font_obj, "size");
		data->font_flags = (int)obs_data_get_int(font_obj, "flags");
		obs_data_release(font_obj);
	}

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
	data->hotkey_id = obs_hotkey_register_source(source, "soniox_toggle_caption",
						     "Toggle Soniox Captions",
						     hotkey_toggle_caption, data);

	obs_log(LOG_INFO, "caption source created");
	return data;
}

static void soniox_caption_destroy(void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	stop_captioning(data);
	if (data->hotkey_id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(data->hotkey_id);
	if (data->text_source)
		obs_source_release(data->text_source);
	if (data->text_source_trans)
		obs_source_release(data->text_source_trans);
	delete data;
}

static void soniox_caption_update(void *private_data, obs_data_t *settings)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);

	// 폰트 (obs_data_t 오브젝트)
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		data->font_face = obs_data_get_string(font_obj, "face");
		data->font_style = obs_data_get_string(font_obj, "style");
		data->font_size = (int)obs_data_get_int(font_obj, "size");
		data->font_flags = (int)obs_data_get_int(font_obj, "flags");
		obs_data_release(font_obj);
	}

	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->display_mode = obs_data_get_string(settings, "display_mode");
	data->target_lang = obs_data_get_string(settings, "target_lang");
	data->max_endpoint_delay_ms = (int)obs_data_get_int(settings, "max_endpoint_delay_ms");

	// 텍스트 스타일
	data->color1 = (uint32_t)obs_data_get_int(settings, "color1");
	data->color2 = (uint32_t)obs_data_get_int(settings, "color2");
	data->outline = obs_data_get_bool(settings, "outline");
	data->drop_shadow = obs_data_get_bool(settings, "drop_shadow");
	data->custom_width = (int)obs_data_get_int(settings, "custom_width");
	data->word_wrap = obs_data_get_bool(settings, "word_wrap");

	if (!data->captioning && !data->connected) {
		if (!data->api_key.empty())
			update_text_display(data, "Soniox Captions Ready!");
		else
			update_text_display(data, "[Set API Key in Properties]");
	}
}

// ─── 버튼 콜백들 ───
static bool on_test_clicked(obs_properties_t *, obs_property_t *, void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	obs_data_t *settings = obs_source_get_settings(data->source);
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	obs_data_release(settings);

	if (data->connected) {
		data->stopping = true;
		data->websocket->stop();
		data->websocket.reset();
		data->connected = false;
		data->stopping = false;
		update_text_display(data, "Soniox Captions Ready!");
	} else {
		test_connection(data);
	}
	return false;
}

static bool on_start_stop_clicked(obs_properties_t *, obs_property_t *property, void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);

	obs_data_t *settings = obs_source_get_settings(data->source);
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->display_mode = obs_data_get_string(settings, "display_mode");
	data->target_lang = obs_data_get_string(settings, "target_lang");
	data->max_endpoint_delay_ms = (int)obs_data_get_int(settings, "max_endpoint_delay_ms");
	obs_data_release(settings);

	if (data->captioning) {
		stop_captioning(data);
		obs_property_set_description(property, "Start Caption");
	} else {
		start_captioning(data);
		obs_property_set_description(property, "Stop Caption");
	}
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

// Properties UI
static obs_properties_t *soniox_caption_get_properties(void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	obs_properties_t *props = obs_properties_create();

	// API Key
	obs_properties_add_text(props, "api_key", "Soniox API Key", OBS_TEXT_PASSWORD);

	// 오디오 소스 선택
	obs_property_t *audio_list =
		obs_properties_add_list(props, "audio_source", "Audio Source", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(audio_list, "(Select audio source)", "");
	obs_enum_sources(enum_audio_sources, audio_list);

	// 언어 선택
	obs_property_t *lang =
		obs_properties_add_list(props, "language", "Language", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(lang, "Korean", "ko");
	obs_property_list_add_string(lang, "English", "en");
	obs_property_list_add_string(lang, "Japanese", "ja");
	obs_property_list_add_string(lang, "Chinese", "zh");
	obs_property_list_add_string(lang, "Spanish", "es");
	obs_property_list_add_string(lang, "French", "fr");
	obs_property_list_add_string(lang, "German", "de");

	// 표시 모드 (원문/번역 토글)
	obs_property_t *mode = obs_properties_add_list(props, "display_mode", "Display Mode",
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, "Original", "original");
	obs_property_list_add_string(mode, "Translation", "translation");
	obs_property_list_add_string(mode, "Both (Original + Translation)", "both");
	obs_property_set_modified_callback(mode, [](obs_properties_t *ps, obs_property_t *,
						    obs_data_t *s) -> bool {
		const char *dm = obs_data_get_string(s, "display_mode");
		bool needs_trans = dm && strcmp(dm, "original") != 0;
		obs_property_set_visible(obs_properties_get(ps, "target_lang"), needs_trans);
		return true;
	});

	obs_property_t *target =
		obs_properties_add_list(props, "target_lang", "Translate To", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(target, "English", "en");
	obs_property_list_add_string(target, "Korean", "ko");
	obs_property_list_add_string(target, "Japanese", "ja");
	obs_property_list_add_string(target, "Chinese", "zh");
	obs_property_list_add_string(target, "Spanish", "es");
	obs_property_list_add_string(target, "French", "fr");
	obs_property_list_add_string(target, "German", "de");

	// 초기 가시성: display_mode가 "original"이 아닐 때 target_lang 표시
	if (data) {
		obs_property_set_visible(target, data->display_mode != "original");
	}

	// 엔드포인트(사일런스) 감도: 낮을수록 더 빨리 캡션 확정
	obs_properties_add_int_slider(props, "max_endpoint_delay_ms",
				      "Endpoint Delay (ms)", 500, 3000, 50);

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
	obs_data_set_default_string(settings, "audio_source", "");
	obs_data_set_default_string(settings, "display_mode", "original");
	obs_data_set_default_string(settings, "target_lang", "en");
	obs_data_set_default_int(settings, "max_endpoint_delay_ms", 500);

	// 폰트 기본값 (obs_data_t 오브젝트)
	obs_data_t *font_obj = obs_data_create();
#ifdef _WIN32
	obs_data_set_default_string(font_obj, "face", "Malgun Gothic");
#else
	obs_data_set_default_string(font_obj, "face", "Apple SD Gothic Neo");
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
	if (data->display_mode == "both" && data->text_source_trans) {
		uint32_t w2 = obs_source_get_width(data->text_source_trans);
		return w1 > w2 ? w1 : w2;
	}
	return w1;
}

static uint32_t soniox_caption_get_height(void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	uint32_t h1 = data->text_source ? obs_source_get_height(data->text_source) : 0;
	if (data->display_mode == "both" && data->text_source_trans) {
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
	if (data->display_mode == "both" && data->text_source_trans) {
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
