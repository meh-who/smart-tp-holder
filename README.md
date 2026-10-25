# smart-tp-holder

*(S)wipable, Scrollable, Flushable: An Interaction Model for Toilet Paper* — a toilet-roll
holder that reads a feed aloud, one item per sheet pulled. A visitor taps the NFC tag on the
holder with a phone; a companion page opens showing whether the holder is online, what it is
reading, and how much paper is left, and lets them choose the source: News, X, TikTok or Reels.

```
 phone ──NFC tap──▶ companion page ──┐
                                     ▼
                      server (FastAPI) ── builds bulletin.json + MP3 clips (server-side TTS)
                                     ▲
 holder (ESP32-S3) ── heartbeat every 20 s, downloads new bulletin, plays MP3s per sheet
```

The holder only ever connects *out* over HTTPS, so it works on any wi-fi without port
forwarding. Speech is synthesised on the server and cached, so the device plays files and
never needs a network round-trip on the pull path.

    firmware/holder/     Arduino sketch for ESP32-S3 (open holder.ino in Arduino IDE 2.x)
    firmware/test/       gesture engine test, runs on a laptop
    server/              FastAPI server + companion page + Dockerfile (see server/.env.example)
    docs/                assembly-guide.html (start here), sensor-mounting.html (arm geometry)

## Run the server locally, no hardware, no network

    cd server && pip install -r requirements.txt
    export TTS_PROVIDER=silent SECRET_KEY=dev PUBLIC_URL=http://localhost:8000 CURATED_DIR=data/curated
    python manage.py add-holder demo-01 demokey --name Demo
    uvicorn app:app --reload &
    python simulate_holder.py http://localhost:8000 demo-01 demokey   # pretends to be the ESP32
    open http://localhost:8000/r/demo-01                              # what the phone sees

`TTS_PROVIDER=silent` makes quiet tones of the right length so the whole flow can be
rehearsed. Switch to `edge` (free neural voices) or `openai` for real speech.

## Hosting

The page must be reachable from visitors' phones and OAuth needs HTTPS. Railway, Fly.io or
any VPS with Docker: deploy `server/`, mount a volume at `/data`, set the variables from
`server/.env.example`, and put `PUBLIC_URL` on HTTPS. Then:

    python manage.py add-holder lisbon-01 <key> --name "TEI Lisbon"   # same id/key as firmware config.h
    python manage.py build lisbon-01 news                              # warm the clip cache
    python manage.py tag-url lisbon-01                                 # → write this URL to the NFC tag

Register `<PUBLIC_URL>/auth/<service>/callback` with each service you enable.

## Services

| Source | Account needed | What it reads | Setup |
|---|---|---|---|
| News | No | Headlines from `NEWS_FEEDS` | Any RSS URLs |
| X | Yes | The visitor's home timeline | X developer app, OAuth 2.0, `tweet.read users.read offline.access`. Timeline access depends on the (paid) API tier. |
| TikTok | Yes | Captions of the visitor's **own** videos | TikTok Login Kit + Display API, `user.info.basic`, `video.list`; app review needed for public use. |
| Reels | Yes | Captions of the visitor's **own** Reels | Meta app, Instagram API with Instagram Login, `instagram_business_basic`; professional accounts only. |

Neither TikTok nor Instagram exposes the feed a person scrolls, only their own posts. For a
venue, `server/data/curated/<source>.json` serves a curated public set instead and the page
shows that service as "Connected · curated set".

## The NFC tag

One NDEF URL record, `https://<PUBLIC_URL>/r/<holder-id>`, on an NTAG215/216 sticker (on-metal
type if the cover is metal). Write and lock it with the NFC Tools app. The firmware does not
know the tag exists; the phone is the reader.
