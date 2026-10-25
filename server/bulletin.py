"""bulletin.py — build what the holder downloads.

A bulletin is: a source, a version, up to 24 items (each with a cached MP3 and
its duration), and the five system clips. Building one means fetching the
feed, excluding anything this holder has spoken in the last twelve hours, and
synthesising whatever is not already in the media cache. Twenty fresh items
take ~15–40 s depending on the TTS provider, so builds run in a background
thread and the holder keeps playing the previous bulletin until the new one is
ready. The heartbeat reply only advertises the new version once it is.
"""
import hashlib
import threading
import time
import traceback

import db
import sources
import tts

SYSTEM_TEXT = {
    "low_paper": "Twelve sheets left.",
    "new_roll": "New roll.",
    "offline": "I can't reach the feed. Reading what I have.",
    "empty": "Nothing new. Pull again later.",
}
SWITCH_TEXT = {s: f"Now reading {label}." for s, label in sources.LABELS.items()}

REBUILD_AFTER_S = 30 * 60      # refresh the same source every half hour
_locks = {}
_building = set()
_guard = threading.Lock()


def _system_clips(source):
    out = {}
    for key, text in SYSTEM_TEXT.items():
        out[key] = tts.synthesize(text)[0]
    out["switch"] = tts.synthesize(SWITCH_TEXT[source])[0]
    return out


def build(holder_id, source):
    """Synchronous build. Returns the bulletin dict or raises."""
    recent = db.recently_played(holder_id)
    items = [it for it in sources.fetch(source, holder_id) if it["id"] not in recent]
    out_items = []
    for it in items:
        try:
            fname, dur = tts.synthesize(it["text"])
        except Exception as e:  # one bad clip must not sink the bulletin
            print(f"[tts] {it['id']}: {e}")
            continue
        out_items.append({"id": it["id"], "text": it["text"], "file": fname, "duration_ms": dur})
    sys_clips = _system_clips(source)
    digest = hashlib.sha1(("|".join(i["id"] for i in out_items) + source).encode()).hexdigest()[:8]
    version = f"{source}-{int(time.time())}-{digest}"
    payload = {"version": version, "source": source, "source_label": sources.LABELS[source],
               "built": time.time(), "items": out_items, "system": sys_clips}
    db.save_bulletin(holder_id, source, version, payload, "ready")
    db.set_source(holder_id, source)
    return payload


def _worker(holder_id, source):
    try:
        db.set_bulletin_status(holder_id, "building")
        build(holder_id, source)
    except Exception as e:
        traceback.print_exc()
        db.set_bulletin_status(holder_id, "error", f"{type(e).__name__}: {e}")
    finally:
        with _guard:
            _building.discard(holder_id)


def request(holder_id, source):
    """Kick off a background build if one is not already running."""
    with _guard:
        if holder_id in _building:
            return False
        _building.add(holder_id)
    threading.Thread(target=_worker, args=(holder_id, source), daemon=True).start()
    return True


def is_building(holder_id):
    with _guard:
        return holder_id in _building


def ensure_current(holder_id):
    """Called on every heartbeat: start a build if the desired source differs
    from the built one, or the built one is stale."""
    h = db.get_holder(holder_id)
    b = db.get_bulletin(holder_id)
    want = h["desired_source"]
    if b is None or b["payload"] is None or b["source"] != want or time.time() - b["ts"] > REBUILD_AFTER_S:
        request(holder_id, want)
