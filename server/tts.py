"""tts.py — text in, a small cached MP3 out.

Four providers, chosen with TTS_PROVIDER:
  openai   OPENAI_API_KEY, OPENAI_TTS_MODEL (default gpt-4o-mini-tts), OPENAI_TTS_VOICE (default "alloy")
  edge     free Microsoft neural voices via the edge-tts package; EDGE_VOICE (default en-GB-SoniaNeural)
  espeak   offline, robotic — only for development without network
  silent   no speech at all: a quiet tone whose length matches the text
           (~15 characters per second). Lets the whole pipeline run on a
           laptop with no network and no TTS installed.

Whatever produces the speech, ffmpeg normalises it to 64 kbps mono 22.05 kHz MP3,
which is what the ESP32 decoder is happiest with (~34 KB per headline).
Files are content-addressed: sha1(provider|voice|text)[:12].mp3, so the holder
can treat "file exists" as "file is correct" and never re-download.
"""
import asyncio
import hashlib
import os
import subprocess
import tempfile

from mutagen.mp3 import MP3

MEDIA_DIR = os.environ.get("MEDIA_DIR", os.path.join(os.path.dirname(__file__), "data", "media"))
PROVIDER = os.environ.get("TTS_PROVIDER", "edge")


def _voice():
    return {
        "openai": os.environ.get("OPENAI_TTS_VOICE", "alloy"),
        "edge": os.environ.get("EDGE_VOICE", "en-GB-SoniaNeural"),
        "espeak": "en-gb",
        "silent": "tone",
    }[PROVIDER]


def clip_name(text):
    h = hashlib.sha1(f"{PROVIDER}|{_voice()}|{text}".encode()).hexdigest()[:12]
    return f"{h}.mp3"


def _normalise(src, dst):
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", src, "-ac", "1", "-ar", "22050",
                    "-b:a", "64k", "-af", "loudnorm=I=-18:TP=-2,apad=pad_dur=0.25", "-f", "mp3", dst], check=True)


def _openai(text, out_path):
    import httpx
    r = httpx.post("https://api.openai.com/v1/audio/speech",
                   headers={"Authorization": f"Bearer {os.environ['OPENAI_API_KEY']}"},
                   json={"model": os.environ.get("OPENAI_TTS_MODEL", "gpt-4o-mini-tts"),
                         "voice": _voice(), "input": text, "response_format": "mp3"}, timeout=60)
    r.raise_for_status()
    with open(out_path, "wb") as f:
        f.write(r.content)


def _edge(text, out_path):
    import edge_tts

    async def run():
        await edge_tts.Communicate(text, _voice()).save(out_path)
    asyncio.run(run())


def _espeak(text, out_path):
    subprocess.run(["espeak-ng", "-v", "en-gb", "-s", "165", "-w", out_path, text], check=True)


def _silent(text, out_path):
    secs = max(0.8, len(text) / 15.0)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi", "-i", f"sine=frequency=440:duration={secs:.2f}",
                    "-af", "volume=0.05", out_path], check=True)


def synthesize(text):
    """Return (filename, duration_ms). Cached."""
    os.makedirs(MEDIA_DIR, exist_ok=True)
    name = clip_name(text)
    final = os.path.join(MEDIA_DIR, name)
    if not os.path.exists(final):
        with tempfile.TemporaryDirectory() as td:
            raw = os.path.join(td, "raw." + ("wav" if PROVIDER in ("espeak", "silent") else "mp3"))
            {"openai": _openai, "edge": _edge, "espeak": _espeak, "silent": _silent}[PROVIDER](text, raw)
            tmp = final + ".part"
            _normalise(raw, tmp)
            os.replace(tmp, final)
    return name, int(MP3(final).info.length * 1000)
