"""sources.py — turn a feed into a list of short spoken items.

Each fetcher returns [{"id": str, "text": str, "meta": {...}}] newest first.
Items are deduplicated, cleaned of URLs and hashtags, and trimmed to roughly
one breath so the holder's 20 s backlog cap behaves the same across sources.

What each platform's API actually hands over (read this before promising
anything to a curator):

  news    RSS. Free, no login. Headline + first sentence of the summary.
  x       OAuth 2.0 PKCE, scopes tweet.read users.read offline.access.
          GET /2/users/:id/timelines/reverse_chronological is the user's
          home timeline — the real feed — but only on paid API tiers.
  tiktok  Login Kit + Display API (scope video.list). Returns the user's OWN
          videos' titles/descriptions. There is no API for the For You feed.
  reels   Instagram API with Instagram Login (scope instagram_business_basic,
          needs a professional account). Returns the user's OWN media; we keep
          media_product_type == REELS. There is no API for the Reels feed.

So for TikTok and Reels the holder reads the visitor's own captions back to
them. That is a different work from "read my feed" — be explicit about it on
the wall text — or point those sources at a curated public cache (see
CURATED_DIR below) for the exhibition.
"""
import glob
import hashlib
import html
import json
import os
import re
import time

import feedparser
import httpx

import db

NEWS_FEEDS = [u.strip() for u in os.environ.get(
    "NEWS_FEEDS",
    "https://feeds.bbci.co.uk/news/world/rss.xml,"
    "https://www.theguardian.com/world/rss,"
    "https://rss.nytimes.com/services/xml/rss/nyt/World.xml").split(",") if u.strip()]

# Drop a folder of JSON files ({"items":[{"text":...},...]}) here to serve a
# curated public set for a source instead of the API (CURATED_DIR/<source>.json).
CURATED_DIR = os.environ.get("CURATED_DIR", os.path.join(os.path.dirname(__file__), "data", "curated"))

MAX_ITEMS = 24
MAX_CHARS = 180

LABELS = {"news": "the news", "x": "X", "tiktok": "TikTok", "reels": "Reels"}

_url_re = re.compile(r"https?://\S+|www\.\S+")
_tag_re = re.compile(r"<[^>]+>")
_hash_re = re.compile(r"(?<!\w)[#@]\w+")
_ws_re = re.compile(r"\s+")


def clean(text):
    t = html.unescape(_tag_re.sub(" ", text or ""))
    t = _url_re.sub("", t)
    t = _hash_re.sub("", t)
    t = _ws_re.sub(" ", t).strip(" -–—|•")
    if len(t) > MAX_CHARS:
        cut = t[:MAX_CHARS]
        t = cut[:max(cut.rfind(". "), cut.rfind(", "), cut.rfind(" "))].rstrip(",") + "…"
    return t


def item_id(source, key):
    return hashlib.sha1(f"{source}|{key}".encode()).hexdigest()[:12]


def _curated(source):
    p = os.path.join(CURATED_DIR, f"{source}.json")
    if not os.path.exists(p):
        return None
    with open(p) as f:
        data = json.load(f)
    return [{"id": item_id(source, it.get("id") or it["text"]), "text": clean(it["text"]), "meta": {"curated": True}}
            for it in data.get("items", []) if clean(it.get("text", ""))]


# ---- news ------------------------------------------------------------------------
def fetch_news(_hid):
    items = []
    for url in NEWS_FEEDS:
        try:
            feed = feedparser.parse(url)
        except Exception:
            continue
        for e in feed.entries[:12]:
            title = clean(e.get("title", ""))
            summary = clean(e.get("summary", ""))
            first = summary.split(". ")[0] if summary else ""
            text = title if not first or first.lower().startswith(title.lower()[:20]) else f"{title}. {first}."
            if title:
                items.append({"id": item_id("news", e.get("id") or e.get("link") or title),
                              "text": text, "meta": {"outlet": feed.feed.get("title", ""), "link": e.get("link")}})
    return items


# ---- X ----------------------------------------------------------------------------
def fetch_x(hid):
    tok = db.get_token(hid, "x")
    if not tok:
        raise PermissionError("X is not connected")
    import oauth
    tok = oauth.ensure_fresh(hid, "x", tok)
    h = {"Authorization": f"Bearer {tok['access_token']}"}
    with httpx.Client(timeout=20) as c:
        uid = tok["extra"].get("user_id")
        if not uid:
            me = c.get("https://api.x.com/2/users/me", headers=h).raise_for_status().json()["data"]
            uid = me["id"]
            db.save_token(hid, "x", tok["access_token"], tok.get("refresh_token"), None, me.get("username"),
                          {**tok["extra"], "user_id": uid})
        r = c.get(f"https://api.x.com/2/users/{uid}/timelines/reverse_chronological",
                  params={"max_results": 50, "tweet.fields": "created_at,author_id,text",
                          "expansions": "author_id", "user.fields": "name", "exclude": "retweets,replies"}, headers=h)
        r.raise_for_status()
        data = r.json()
    names = {u["id"]: u["name"] for u in data.get("includes", {}).get("users", [])}
    out = []
    for t in data.get("data", []):
        text = clean(t["text"])
        if len(text) < 12:
            continue
        who = names.get(t.get("author_id"))
        out.append({"id": item_id("x", t["id"]), "text": f"{who}: {text}" if who else text, "meta": {"author": who}})
    return out


# ---- TikTok (own videos) ------------------------------------------------------------
def fetch_tiktok(hid):
    tok = db.get_token(hid, "tiktok")
    if not tok:
        raise PermissionError("TikTok is not connected")
    import oauth
    tok = oauth.ensure_fresh(hid, "tiktok", tok)
    with httpx.Client(timeout=20) as c:
        r = c.post("https://open.tiktokapis.com/v2/video/list/",
                   params={"fields": "id,title,video_description,create_time"},
                   headers={"Authorization": f"Bearer {tok['access_token']}", "Content-Type": "application/json"},
                   json={"max_count": 20})
        r.raise_for_status()
        vids = r.json().get("data", {}).get("videos", [])
    out = []
    for v in vids:
        text = clean(v.get("video_description") or v.get("title") or "")
        if text:
            out.append({"id": item_id("tiktok", v["id"]), "text": text, "meta": {"created": v.get("create_time")}})
    return out


# ---- Reels (own media) ---------------------------------------------------------------
def fetch_reels(hid):
    tok = db.get_token(hid, "reels")
    if not tok:
        raise PermissionError("Instagram is not connected")
    with httpx.Client(timeout=20) as c:
        r = c.get("https://graph.instagram.com/me/media",
                  params={"fields": "id,caption,media_type,media_product_type,timestamp", "limit": 50,
                          "access_token": tok["access_token"]})
        r.raise_for_status()
        media = r.json().get("data", [])
    out = []
    for m in media:
        if m.get("media_product_type") not in (None, "REELS") and m.get("media_type") != "VIDEO":
            continue
        text = clean(m.get("caption") or "")
        if text:
            out.append({"id": item_id("reels", m["id"]), "text": text, "meta": {"ts": m.get("timestamp")}})
    return out


FETCHERS = {"news": fetch_news, "x": fetch_x, "tiktok": fetch_tiktok, "reels": fetch_reels}


def fetch(source, hid):
    curated = _curated(source)
    items = curated if curated is not None else FETCHERS[source](hid)
    seen, out = set(), []
    for it in items:
        k = it["text"].lower()[:60]
        if k in seen or not it["text"]:
            continue
        seen.add(k)
        out.append(it)
    return out[:MAX_ITEMS]
