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

using json = nlohmann::json;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

// ─── 소스 데이터 구조체 ───
struct soniox_caption_data {
	obs_source_t *source;
	obs_source_t *text_source;

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
	std::string api_key;
	std::string language{"ko"};
	bool translate{false};
	std::string target_lang{"en"};
};

// ─── 텍스트 표시 업데이트 ───
static void update_text_display(soniox_caption_data *data, const char *text)
{
	if (!data->text_source)
		return;

	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", data->font_face.c_str());
	obs_data_set_int(font, "size", data->font_size);
	obs_data_set_string(font, "style", "Regular");
	obs_data_set_int(font, "flags", 0);

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "text", text);
	obs_data_set_obj(s, "font", font);
	obs_source_update(data->text_source, s);

	obs_data_release(font);
	obs_data_release(s);
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

				data->final_buffer.clear();
				data->final_trans_buffer.clear();
				data->partial_text.clear();
				update_text_display(data, "");
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

		// 원문 표시
		std::string display = data->final_buffer + non_final;
		while (!display.empty() && display.front() == ' ')
			display.erase(display.begin());

		// 번역 표시 (활성화 시 줄바꿈으로 아래에 추가)
		if (data->translate) {
			std::string trans_display =
				data->final_trans_buffer + non_final_trans;
			while (!trans_display.empty() && trans_display.front() == ' ')
				trans_display.erase(trans_display.begin());
			if (!trans_display.empty())
				display += "\n" + trans_display;
		}

		if (!display.empty()) {
			data->partial_text = display;
			update_text_display(data, display.c_str());
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
	data->websocket->disableAutomaticReconnection();

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
			config["enable_endpoint_detection"] = true;

			// 번역 활성화 시 Soniox 내장 번역 추가
			if (data->translate && !data->target_lang.empty()) {
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
			json config;
			config["api_key"] = key;
			config["model"] = "stt-rt-v4";
			config["audio_format"] = "auto";
			config["language_hints"] = {lang};
			config["enable_endpoint_detection"] = true;
			data->websocket->send(config.dump());
			update_text_display(data, "Connected OK!");
			obs_log(LOG_INFO, "Test connection: OK");
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

static const char *soniox_caption_get_name(void *)
{
	return "Soniox Captions";
}

static void *soniox_caption_create(obs_data_t *settings, obs_source_t *source)
{
	auto *data = new soniox_caption_data();
	data->source = source;
	data->font_size = (int)obs_data_get_int(settings, "font_size");

	obs_data_t *ts = obs_data_create();
	obs_data_set_string(ts, "text", "Soniox Captions Ready!");
	obs_data_set_int(ts, "font_size", data->font_size);
#ifdef _WIN32
	data->text_source = obs_source_create_private("text_gdiplus", "soniox_text", ts);
#else
	data->text_source = obs_source_create_private("text_ft2_source_v2", "soniox_text", ts);
#endif
	obs_data_release(ts);

	obs_log(LOG_INFO, "caption source created");
	return data;
}

static void soniox_caption_destroy(void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	stop_captioning(data);
	if (data->text_source)
		obs_source_release(data->text_source);
	delete data;
}

static void soniox_caption_update(void *private_data, obs_data_t *settings)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	data->font_size = (int)obs_data_get_int(settings, "font_size");
	data->font_face = obs_data_get_string(settings, "font_face");
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->translate = obs_data_get_bool(settings, "translate");
	data->target_lang = obs_data_get_string(settings, "target_lang");

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

static bool on_start_stop_clicked(obs_properties_t *, obs_property_t *, void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);

	obs_data_t *settings = obs_source_get_settings(data->source);
	data->api_key = obs_data_get_string(settings, "api_key");
	data->language = obs_data_get_string(settings, "language");
	data->audio_source_name = obs_data_get_string(settings, "audio_source");
	data->translate = obs_data_get_bool(settings, "translate");
	data->target_lang = obs_data_get_string(settings, "target_lang");
	obs_data_release(settings);

	if (data->captioning) {
		stop_captioning(data);
	} else {
		start_captioning(data);
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

	// 번역 옵션
	obs_properties_add_bool(props, "translate", "Enable Translation");

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

	// 폰트 선택
	obs_property_t *font_list =
		obs_properties_add_list(props, "font_face", "Font", OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
#ifdef _WIN32
	obs_property_list_add_string(font_list, "Malgun Gothic", "Malgun Gothic");
	obs_property_list_add_string(font_list, "Yu Gothic", "Yu Gothic");
#else
	obs_property_list_add_string(font_list, "Apple SD Gothic Neo", "Apple SD Gothic Neo");
	obs_property_list_add_string(font_list, "Hiragino Sans", "Hiragino Sans");
#endif
	obs_property_list_add_string(font_list, "Noto Sans CJK KR", "Noto Sans CJK KR");
	obs_property_list_add_string(font_list, "Noto Sans CJK JP", "Noto Sans CJK JP");
	obs_property_list_add_string(font_list, "Arial", "Arial");
	obs_property_list_add_string(font_list, "Helvetica", "Helvetica");

	// 폰트 크기
	obs_properties_add_int_slider(props, "font_size", "Font Size", 12, 120, 2);

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
	obs_data_set_default_bool(settings, "translate", false);
	obs_data_set_default_string(settings, "target_lang", "en");
#ifdef _WIN32
	obs_data_set_default_string(settings, "font_face", "Malgun Gothic");
#else
	obs_data_set_default_string(settings, "font_face", "Apple SD Gothic Neo");
#endif
	obs_data_set_default_int(settings, "font_size", 48);
}

static uint32_t soniox_caption_get_width(void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	return data->text_source ? obs_source_get_width(data->text_source) : 0;
}

static uint32_t soniox_caption_get_height(void *private_data)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	return data->text_source ? obs_source_get_height(data->text_source) : 0;
}

static void soniox_caption_video_render(void *private_data, gs_effect_t *)
{
	auto *data = static_cast<soniox_caption_data *>(private_data);
	if (data->text_source)
		obs_source_video_render(data->text_source);
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
