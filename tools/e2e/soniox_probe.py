"""Soniox protocol probe: stream a WAV in real time and log token order.

Usage: python soniox_probe.py <env_file> <wav> [--translate LANG] [--bad-key] [--idle SECONDS]
The API key is read from env_file and never printed.
"""
import argparse
import asyncio
import json
import sys
import time
import wave

sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)

import websockets

URL = "wss://stt-rt.soniox.com/transcribe-websocket"


def load_key(env_path):
    for line in open(env_path, encoding="utf-8"):
        line = line.strip()
        if line.startswith("SONIOX_API_KEY="):
            return line.split("=", 1)[1].strip().strip('"').strip("'")
    raise SystemExit("SONIOX_API_KEY not found")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("env")
    ap.add_argument("wav")
    ap.add_argument("--translate")
    ap.add_argument("--bad-key", action="store_true")
    ap.add_argument("--idle", type=float, default=0, help="seconds of keepalive-only after audio")
    ap.add_argument("--no-keepalive", action="store_true")
    a = ap.parse_args()

    key = "invalid_key_for_test" if a.bad_key else load_key(a.env)
    w = wave.open(a.wav, "rb")
    assert w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2
    pcm = w.readframes(w.getnframes())
    print(f"audio: {len(pcm) / 32000:.1f}s")

    cfg = {
        "api_key": key,
        "model": "stt-rt-v5",
        "audio_format": "pcm_s16le",
        "sample_rate": 16000,
        "num_channels": 1,
        "language_hints": ["ko"],
        "enable_language_identification": True,
        "enable_endpoint_detection": True,
        "max_endpoint_delay_ms": 500,
    }
    if a.translate:
        cfg["translation"] = {"type": "one_way", "target_language": a.translate}

    t0 = time.monotonic()
    ts = lambda: f"{time.monotonic() - t0:6.2f}s"

    async with websockets.connect(URL) as ws:
        await ws.send(json.dumps(cfg))

        async def sender():
            chunk = 3200  # 100 ms
            for i in range(0, len(pcm), chunk):
                await ws.send(pcm[i:i + chunk])
                await asyncio.sleep(0.1)
            print(f"[{ts()}] audio done")
            end = time.monotonic() + a.idle
            while time.monotonic() < end:
                await asyncio.sleep(5)
                if not a.no_keepalive:
                    await ws.send(json.dumps({"type": "keepalive"}))
                    print(f"[{ts()}] keepalive sent")
            await ws.send(b"")  # end of stream
            print(f"[{ts()}] EOS sent")

        send_task = asyncio.create_task(sender())
        try:
            async for raw in ws:
                msg = json.loads(raw)
                if "error_message" in msg:
                    print(f"[{ts()}] ERROR code={msg.get('error_code')} msg={msg['error_message']}")
                    continue
                parts = []
                for t in msg.get("tokens", []):
                    kind = "T" if t.get("translation_status") == "translation" else "O"
                    fin = "F" if t.get("is_final") else "p"
                    parts.append(f"{kind}{fin}:{t.get('text')!r}")
                if parts:
                    print(f"[{ts()}] " + " ".join(parts))
                if msg.get("finished"):
                    print(f"[{ts()}] finished")
        except websockets.ConnectionClosed as e:
            print(f"[{ts()}] closed code={e.code} reason={e.reason!r}")
        send_task.cancel()
    print(f"[{ts()}] connection ended")


asyncio.run(main())
