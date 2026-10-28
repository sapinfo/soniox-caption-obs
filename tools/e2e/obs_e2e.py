"""End-to-end test of the Soniox caption plugin through obs-websocket.

Usage: python obs_e2e.py <env_file> <wav> <shots_dir> [--mode both|original|translation] [--quick]
API key is read from env_file and never printed. See tools/e2e/README.md.
Requires OBS running with obs-websocket enabled (Tools > WebSocket Server Settings).
Creates and uses a separate "SonioxTest" scene collection, then switches back.
"""
import argparse
import base64
import json
import os
import sys
import time

import obsws_python as obs

sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)

COLLECTION = "SonioxTest"
AUDIO = "TestAudio"
CAP = "Captions"

t0 = time.monotonic()


def log(msg):
    print(f"[{time.monotonic() - t0:6.1f}s] {msg}")


def load_key(env_path):
    for line in open(env_path, encoding="utf-8"):
        line = line.strip()
        if line.startswith("SONIOX_API_KEY="):
            return line.split("=", 1)[1].strip().strip('"').strip("'")
    raise SystemExit("SONIOX_API_KEY not found")


def obs_config_dir():
    if sys.platform == "win32":
        return os.path.join(os.environ["APPDATA"], "obs-studio")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/obs-studio")
    return os.path.expanduser("~/.config/obs-studio")


def ws_password():
    p = os.path.join(obs_config_dir(), "plugin_config", "obs-websocket", "config.json")
    return json.load(open(p, encoding="utf-8"))["server_password"]


def connect():
    for _ in range(60):
        try:
            return obs.ReqClient(host="localhost", port=4455, password=ws_password(), timeout=15)
        except Exception:
            time.sleep(1)
    raise SystemExit("could not connect to obs-websocket")


class Shooter:
    def __init__(self, cl, out_dir, tag):
        self.cl, self.out, self.tag, self.n = cl, out_dir, tag, 0
        os.makedirs(out_dir, exist_ok=True)

    def shot(self, label):
        scene = self.cl.get_current_program_scene().current_program_scene_name
        r = self.cl.get_source_screenshot(scene, "png", 960, 540, -1)
        data = r.image_data.split(",", 1)[1]
        self.n += 1
        path = os.path.join(self.out, f"{self.tag}_{self.n:02d}_{label}.png")
        open(path, "wb").write(base64.b64decode(data))
        return path


def press(cl, prop):
    cl.press_input_properties_button(CAP, prop)
    log(f"pressed {prop}")


def alive(cl):
    cl.get_version()
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("env")
    ap.add_argument("wav")
    ap.add_argument("shots")
    ap.add_argument("--mode", default="both")
    ap.add_argument("--quick", action="store_true")
    a = ap.parse_args()
    key = load_key(a.env)

    cl = connect()
    v = cl.get_version()
    log(f"OBS {v.obs_version}, websocket {v.obs_web_socket_version}")
    kinds = cl.get_input_kind_list(False).input_kinds
    assert "soniox_caption" in kinds, "plugin source kind not registered"
    log("plugin source kind 'soniox_caption' registered")

    original_collection = cl.get_scene_collection_list().current_scene_collection_name
    if COLLECTION in cl.get_scene_collection_list().scene_collections:
        cl.set_current_scene_collection(COLLECTION)
    else:
        cl.create_scene_collection(COLLECTION)
    time.sleep(2)
    log(f"switched to scene collection {COLLECTION} (was {original_collection!r})")

    scene = cl.get_current_program_scene().current_program_scene_name
    for name in (CAP, AUDIO, "BG"):
        try:
            cl.remove_input(name)
            time.sleep(0.5)
        except Exception:
            pass

    cl.create_input(scene, "BG", "color_source_v3",
                    {"color": 0xFF402010, "width": 1920, "height": 1080}, True)
    cl.create_input(scene, AUDIO, "ffmpeg_source",
                    {"local_file": a.wav, "looping": False, "restart_on_activate": False,
                     "close_when_inactive": False}, True)
    cl.create_input(scene, CAP, "soniox_caption",
                    {"api_key": key, "audio_source": AUDIO, "language": "ko",
                     "display_mode": a.mode, "target_lang": "en", "caption_hold_ms": 4000,
                     "custom_width": 900, "word_wrap": True, "outline": True}, True)
    time.sleep(1)
    sh = Shooter(cl, a.shots, a.mode)
    log(f"created inputs; shot {sh.shot('ready')}")

    # ── Phase 1: captioning with real audio ──
    cl.trigger_media_input_action(AUDIO, "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_STOP")
    press(cl, "start_stop")
    time.sleep(2.5)
    log(f"shot {sh.shot('connected')}")
    cl.trigger_media_input_action(AUDIO, "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART")
    log("audio playback started")

    test_pressed = False
    for i in range(26):
        time.sleep(1)
        if i == 8 and not test_pressed:
            # Bug #1 regression: Test Connection while captioning must be ignored
            for _ in range(3):
                press(cl, "test_connection")
            test_pressed = True
        sh.shot(f"t{i:02d}")
    log("audio should be done; captions should clear after hold (4s)")

    if a.quick:
        press(cl, "start_stop")
        time.sleep(1)
        for name in (CAP, AUDIO, "BG"):
            cl.remove_input(name)
        cl.set_current_scene_collection(original_collection)
        log("QUICK DONE")
        return

    # ── Phase 2: keepalive — 30s of no audio must not drop the session ──
    for i in range(6):
        time.sleep(5)
        sh.shot(f"idle{i}")
    log("idle 30s done")
    alive(cl)

    # replay audio to confirm the session is still usable after idle
    cl.trigger_media_input_action(AUDIO, "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART")
    time.sleep(3)
    log(f"post-idle shot {sh.shot('postidle')}")

    # ── Phase 2b: mute mid-utterance — plugin must finalize instead of leaving a stuck partial ──
    cl.set_input_mute(AUDIO, True)
    log("muted audio mid-utterance (expect 'sent finalize' + a Turn line in OBS log)")
    time.sleep(2)
    log(f"muted shot {sh.shot('muted')}")
    time.sleep(5)
    log(f"after hold shot {sh.shot('muted_cleared')} (captions should be cleared)")
    cl.set_input_mute(AUDIO, False)
    press(cl, "start_stop")  # stop
    time.sleep(1)
    log(f"stopped; shot {sh.shot('stopped')}")

    # ── Phase 3: Test connection stress (bug #2) ──
    for _ in range(5):
        press(cl, "test_connection")
        time.sleep(0.15)
    time.sleep(3)
    log(f"after test spam; shot {sh.shot('test')}")
    press(cl, "test_connection")
    time.sleep(0.05)
    cl.remove_input(CAP)  # destroy while a test socket is connecting
    log("removed caption source during test connection")
    time.sleep(2)
    alive(cl)
    log("OBS still responsive after destroy-during-test")

    # ── Phase 4: invalid key → no reconnect loop (bug #5) ──
    cl.create_input(scene, CAP, "soniox_caption",
                    {"api_key": "invalid_key_for_test", "audio_source": AUDIO, "language": "ko",
                     "display_mode": "original"}, True)
    time.sleep(1)
    press(cl, "start_stop")
    time.sleep(12)
    log(f"bad key shot {sh.shot('badkey')}")
    press(cl, "start_stop")
    time.sleep(1)

    # ── Phase 5: start/stop toggle stress ──
    cl.set_input_settings(CAP, {"api_key": key}, True)
    for _ in range(6):
        press(cl, "start_stop")
        time.sleep(0.3)
    time.sleep(1)
    cl.remove_input(CAP)  # destroy while possibly captioning
    time.sleep(1)
    alive(cl)
    log("OBS still responsive after toggle stress + destroy")

    cl.remove_input(AUDIO)
    cl.remove_input("BG")
    cl.set_current_scene_collection(original_collection)
    log(f"restored scene collection {original_collection!r}")
    log("E2E DONE")


main()
