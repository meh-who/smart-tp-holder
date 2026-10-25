#!/usr/bin/env python3
"""simulate_holder.py — pretend to be the ESP32 so you can test the page without hardware.

  python simulate_holder.py http://localhost:8000 lisbon-01 <key>

Heartbeats every 5 s, fetches the bulletin when the server advertises a new one,
and 'pulls a sheet' every 20 s (marks one item played). Ctrl-C to stop.
"""
import json, sys, time, httpx

base, hid, key = sys.argv[1].rstrip("/"), sys.argv[2], sys.argv[3]
H = {"X-Holder-Key": key}
have_version, items, cursor, played_since, sheets = "", [], 0, [], 0
t0 = time.time(); last_pull = None
with httpx.Client(timeout=20) as c:
    while True:
        hb = {"fw": "sim-1", "uptime_s": int(time.time() - t0), "rssi": -58, "sheets_total": sheets,
              "sheets_this_roll": sheets, "roll_sheets": 200, "last_pull_s_ago": int(time.time() - last_pull) if last_pull else -1,
              "queued": 0, "playing": False, "bulletin": have_version,
              "source": have_version.split("-")[0] if have_version else "", "mains": True, "played": played_since}
        r = c.post(f"{base}/api/holder/{hid}/heartbeat", json=hb, headers=H); r.raise_for_status(); rep = r.json()
        played_since = []
        if rep["bulletin"] and rep["bulletin"] != have_version:
            b = c.get(f"{base}/api/holder/{hid}/bulletin", headers=H).json()
            items, cursor, have_version = b["items"], 0, b["version"]
            print(f"[sim] new bulletin {have_version}: {len(items)} items from {b['source']}")
            for it in items[:3]: print("      ", it["duration_ms"], "ms", it["text"])
        if items and (last_pull is None or time.time() - last_pull > 20):
            it = items[cursor % len(items)]; cursor += 1; sheets += 1; last_pull = time.time()
            played_since.append(it["id"]); print(f"[sim] pull -> speaks: {it['text']}")
        time.sleep(5)
