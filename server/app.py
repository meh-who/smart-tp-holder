"""app.py — the companion web app and the holder API.

    uvicorn app:app --host 0.0.0.0 --port 8000

Phone side (opened by tapping the NFC tag, URL = PUBLIC_URL/r/<holder_id>):
  GET  /r/{holder_id}                  the page
  GET  /api/holder/{id}/status         connection status + services (polled every 5 s)
  POST /api/holder/{id}/source         {"source": "news"|"x"|"tiktok"|"reels"}
  POST /api/holder/{id}/disconnect     {"provider": ...}
  GET  /auth/{provider}/start?holder=  OAuth redirect
  GET  /auth/{provider}/callback

Holder side (header X-Holder-Key):
  POST /api/holder/{id}/heartbeat      telemetry in, {source, bulletin, poll_s} out
  GET  /api/holder/{id}/bulletin       the current bulletin JSON
  GET  /media/{file}                   cached MP3s (public, content-addressed)
"""
import os
import time

from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from itsdangerous import BadSignature, URLSafeSerializer

import bulletin
import db
import oauth
import sources
import tts

HERE = os.path.dirname(os.path.abspath(__file__))
POLL_S = int(os.environ.get("HOLDER_POLL_S", "20"))
ONLINE_WINDOW_S = POLL_S * 2 + 10

app = FastAPI(title="toilet roll holder — companion", docs_url=None, redoc_url=None)
db.init()
os.makedirs(tts.MEDIA_DIR, exist_ok=True)
app.mount("/media", StaticFiles(directory=tts.MEDIA_DIR), name="media")
_session = URLSafeSerializer(os.environ.get("SECRET_KEY", "dev-secret-change-me"), salt="page-session")


# ---- helpers ------------------------------------------------------------------------
def _holder_or_404(hid):
    h = db.get_holder(hid)
    if not h:
        raise HTTPException(404, "unknown holder")
    return h


def _check_key(request: Request, h):
    if request.headers.get("X-Holder-Key") != h["key"]:
        raise HTTPException(401, "bad holder key")


def _check_session(request: Request, hid):
    """The page sets a cookie when opened from the tag URL; API calls need it.
    Anyone who taps the tag can control the holder — that is the point in a
    gallery — but a random site cannot POST to it from a visitor's phone."""
    try:
        s = _session.loads(request.cookies.get("holder_session", ""))
    except BadSignature:
        raise HTTPException(403, "open the page from the holder's tag first")
    if s.get("h") != hid:
        raise HTTPException(403, "session is for another holder")


def _services(hid):
    connected = db.connected_providers(hid)
    out = [{"id": "news", "label": "News", "configured": True, "connected": True, "account": None,
            "note": "Headlines from public RSS feeds. No login."}]
    notes = {
        "x": "Your home timeline. Needs the venue's X API key.",
        "tiktok": "Your own videos' captions — the API does not expose the For You feed.",
        "reels": "Your own Reels' captions — the API does not expose the Reels feed. Professional account required.",
    }
    for p in ("x", "tiktok", "reels"):
        curated = os.path.exists(os.path.join(sources.CURATED_DIR, f"{p}.json"))
        out.append({"id": p, "label": oauth.PROVIDERS[p]["label"], "configured": oauth.configured(p) or curated,
                    "connected": p in connected or curated,
                    "account": connected.get(p) or ("curated set" if curated else None), "note": notes[p]})
    return out


# ---- page -----------------------------------------------------------------------------
@app.get("/r/{hid}", response_class=HTMLResponse)
def page(hid: str, response: Response):
    _holder_or_404(hid)
    with open(os.path.join(HERE, "static", "companion.html"), encoding="utf-8") as f:
        html = f.read().replace("__HOLDER_ID__", hid)
    resp = HTMLResponse(html)
    resp.set_cookie("holder_session", _session.dumps({"h": hid, "t": int(time.time())}),
                    max_age=12 * 3600, httponly=True, samesite="lax")
    return resp


@app.get("/api/holder/{hid}/status")
def status(hid: str, request: Request):
    h = _holder_or_404(hid)
    _check_session(request, hid)
    ts, hb = db.get_heartbeat(hid)
    b = db.get_bulletin(hid)
    online = bool(ts and time.time() - ts < ONLINE_WINDOW_S)
    hb = hb or {}
    built_source = b["source"] if b and b["payload"] else None
    return {
        "holder": {"id": hid, "name": h["name"]},
        "online": online,
        "last_seen_s": int(time.time() - ts) if ts else None,
        "last_pull_s": hb.get("last_pull_s_ago") if hb.get("last_pull_s_ago", -1) >= 0 else None,
        "playing": hb.get("playing", False),
        "queued": hb.get("queued", 0),
        "sheets_this_roll": hb.get("sheets_this_roll"),
        "roll_sheets": hb.get("roll_sheets", 200),
        "sheets_total": hb.get("sheets_total"),
        "rssi": hb.get("rssi"),
        "fw": hb.get("fw"),
        "holder_source": hb.get("source") or None,          # what the holder is actually reading
        "holder_bulletin": hb.get("bulletin"),
        "desired_source": h["desired_source"],              # what the visitor asked for
        "built_source": built_source,
        "bulletin": {
            "status": "building" if bulletin.is_building(hid) else (b["status"] if b else "none"),
            "version": b["version"] if b else None,
            "items": len(b["payload"]["items"]) if b and b["payload"] else 0,
            "error": b["error"] if b else None,
            "age_s": int(time.time() - b["ts"]) if b else None,
        },
        "services": _services(hid),
        "poll_s": POLL_S,
    }


@app.post("/api/holder/{hid}/source")
async def set_source(hid: str, request: Request):
    _holder_or_404(hid)
    _check_session(request, hid)
    body = await request.json()
    source = body.get("source")
    if source not in sources.LABELS:
        raise HTTPException(400, "unknown source")
    if source != "news":
        if not oauth.configured(source):
            raise HTTPException(409, f"{oauth.PROVIDERS[source]['label']} is not set up on this server")
        if not db.get_token(hid, source) and not os.path.exists(os.path.join(sources.CURATED_DIR, f"{source}.json")):
            raise HTTPException(409, f"connect {oauth.PROVIDERS[source]['label']} first")
    db.set_desired_source(hid, source)
    bulletin.request(hid, source)
    return {"ok": True, "desired_source": source}


@app.post("/api/holder/{hid}/disconnect")
async def disconnect(hid: str, request: Request):
    h = _holder_or_404(hid)
    _check_session(request, hid)
    body = await request.json()
    p = body.get("provider")
    if p not in oauth.PROVIDERS:
        raise HTTPException(400, "unknown provider")
    db.delete_token(hid, p)
    if h["desired_source"] == p:
        db.set_desired_source(hid, "news")
        bulletin.request(hid, "news")
    return {"ok": True}


# ---- OAuth ----------------------------------------------------------------------------
@app.get("/auth/{provider}/start")
def auth_start(provider: str, holder: str, request: Request):
    if provider not in oauth.PROVIDERS:
        raise HTTPException(404)
    _holder_or_404(holder)
    _check_session(request, holder)
    if not oauth.configured(provider):
        raise HTTPException(409, "provider not configured")
    return RedirectResponse(oauth.start(provider, holder))


@app.get("/auth/{provider}/callback")
def auth_callback(provider: str, request: Request, code: str = None, state: str = None, error: str = None):
    if provider not in oauth.PROVIDERS:
        raise HTTPException(404)
    if error or not code:
        return HTMLResponse(f"<p>Authorisation cancelled ({error or 'no code'}). You can close this tab.</p>")
    try:
        hid = oauth.finish(provider, code, state)
    except Exception as e:
        return HTMLResponse(f"<p>Could not connect {provider}: {e}</p>", status_code=400)
    return RedirectResponse(f"/r/{hid}?connected={provider}", status_code=303)


# ---- holder API ---------------------------------------------------------------------------
@app.post("/api/holder/{hid}/heartbeat")
async def heartbeat(hid: str, request: Request):
    h = _holder_or_404(hid)
    _check_key(request, h)
    payload = await request.json()
    db.save_heartbeat(hid, payload)
    played = payload.get("played") or []
    if played:
        db.mark_played(hid, played)
    bulletin.ensure_current(hid)
    b = db.get_bulletin(hid)
    ready = b and b["payload"]
    return {"source": b["source"] if ready else h["source"],
            "bulletin": b["version"] if ready else "",
            "poll_s": POLL_S}


@app.get("/api/holder/{hid}/bulletin")
def get_bulletin(hid: str, request: Request):
    h = _holder_or_404(hid)
    _check_key(request, h)
    b = db.get_bulletin(hid)
    if not b or not b["payload"]:
        raise HTTPException(404, "no bulletin yet")
    return JSONResponse(b["payload"])


@app.get("/healthz")
def healthz():
    return {"ok": True, "holders": len(db.list_holders()), "tts": tts.PROVIDER}


@app.get("/")
def root():
    return HTMLResponse("<p>Tap the holder with your phone.</p>")
