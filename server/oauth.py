"""oauth.py — "Connect" buttons on the companion page.

All three providers are authorization-code flows; X requires PKCE, TikTok
accepts it, Instagram does not use it. The state parameter carries the holder
id and a nonce, signed, so the callback knows which holder to attach the token to.

Register one app per provider and put the credentials in the environment:
  X         developer.x.com     -> X_CLIENT_ID, X_CLIENT_SECRET
  TikTok    developers.tiktok.com -> TIKTOK_CLIENT_KEY, TIKTOK_CLIENT_SECRET
  Instagram developers.facebook.com (Instagram API with Instagram Login)
                                -> IG_APP_ID, IG_APP_SECRET
Each needs the redirect URI  <PUBLIC_URL>/auth/<provider>/callback  whitelisted.
"""
import base64
import hashlib
import os
import secrets
import time
from urllib.parse import urlencode

import httpx
from itsdangerous import BadSignature, URLSafeTimedSerializer

import db

PUBLIC_URL = os.environ.get("PUBLIC_URL", "http://localhost:8000").rstrip("/")
_signer = URLSafeTimedSerializer(os.environ.get("SECRET_KEY", "dev-secret-change-me"), salt="oauth-state")

PROVIDERS = {
    "x": {
        "label": "X",
        "auth_url": "https://x.com/i/oauth2/authorize",
        "token_url": "https://api.x.com/2/oauth2/token",
        "client_id": os.environ.get("X_CLIENT_ID", ""),
        "client_secret": os.environ.get("X_CLIENT_SECRET", ""),
        "scope": "tweet.read users.read offline.access",
        "pkce": True,
        "id_param": "client_id",
        "basic_auth": True,
    },
    "tiktok": {
        "label": "TikTok",
        "auth_url": "https://www.tiktok.com/v2/auth/authorize/",
        "token_url": "https://open.tiktokapis.com/v2/oauth/token/",
        "client_id": os.environ.get("TIKTOK_CLIENT_KEY", ""),
        "client_secret": os.environ.get("TIKTOK_CLIENT_SECRET", ""),
        "scope": "user.info.basic,video.list",
        "pkce": True,
        "id_param": "client_key",
        "basic_auth": False,
    },
    "reels": {
        "label": "Instagram Reels",
        "auth_url": "https://www.instagram.com/oauth/authorize",
        "token_url": "https://api.instagram.com/oauth/access_token",
        "client_id": os.environ.get("IG_APP_ID", ""),
        "client_secret": os.environ.get("IG_APP_SECRET", ""),
        "scope": "instagram_business_basic",
        "pkce": False,
        "id_param": "client_id",
        "basic_auth": False,
    },
}


def configured(provider):
    p = PROVIDERS[provider]
    return bool(p["client_id"] and p["client_secret"])


def redirect_uri(provider):
    return f"{PUBLIC_URL}/auth/{provider}/callback"


def start(provider, holder_id):
    """Return the URL to send the phone to."""
    p = PROVIDERS[provider]
    verifier = secrets.token_urlsafe(48)
    state = _signer.dumps({"h": holder_id, "p": provider, "v": verifier, "n": secrets.token_hex(8)})
    params = {
        p["id_param"]: p["client_id"],
        "redirect_uri": redirect_uri(provider),
        "response_type": "code",
        "scope": p["scope"],
        "state": state,
    }
    if p["pkce"]:
        challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
        params["code_challenge"] = challenge
        params["code_challenge_method"] = "S256"
    return f"{p['auth_url']}?{urlencode(params)}"


def finish(provider, code, state):
    """Exchange the code; returns the holder id the token was attached to."""
    try:
        s = _signer.loads(state, max_age=900)
    except BadSignature:
        raise ValueError("bad state")
    if s["p"] != provider:
        raise ValueError("provider mismatch")
    p = PROVIDERS[provider]
    data = {"grant_type": "authorization_code", "code": code, "redirect_uri": redirect_uri(provider)}
    headers = {}
    if p["pkce"]:
        data["code_verifier"] = s["v"]
    if p["basic_auth"]:
        basic = base64.b64encode(f"{p['client_id']}:{p['client_secret']}".encode()).decode()
        headers["Authorization"] = f"Basic {basic}"
        data["client_id"] = p["client_id"]
    else:
        data[p["id_param"]] = p["client_id"]
        data["client_secret"] = p["client_secret"]
    with httpx.Client(timeout=20) as c:
        r = c.post(p["token_url"], data=data, headers=headers)
        r.raise_for_status()
        tok = r.json()
        account, extra = None, {}
        if provider == "reels":
            # Short-lived (1 h) -> long-lived (60 days).
            r2 = c.get("https://graph.instagram.com/access_token",
                       params={"grant_type": "ig_exchange_token", "client_secret": p["client_secret"],
                               "access_token": tok["access_token"]})
            if r2.status_code == 200:
                tok = {**tok, **r2.json()}
            me = c.get("https://graph.instagram.com/me", params={"fields": "username", "access_token": tok["access_token"]})
            if me.status_code == 200:
                account = me.json().get("username")
        elif provider == "x":
            me = c.get("https://api.x.com/2/users/me", headers={"Authorization": f"Bearer {tok['access_token']}"})
            if me.status_code == 200:
                d = me.json()["data"]
                account, extra = "@" + d["username"], {"user_id": d["id"]}
        elif provider == "tiktok":
            extra = {"open_id": tok.get("open_id")}
            me = c.get("https://open.tiktokapis.com/v2/user/info/", params={"fields": "display_name"},
                       headers={"Authorization": f"Bearer {tok['access_token']}"})
            if me.status_code == 200:
                account = me.json().get("data", {}).get("user", {}).get("display_name")
    db.save_token(s["h"], provider, tok["access_token"], tok.get("refresh_token"), tok.get("expires_in"), account, extra)
    return s["h"]


def ensure_fresh(holder_id, provider, tok):
    """Refresh an expiring token where the provider supports it."""
    if not tok.get("expires_at") or tok["expires_at"] - time.time() > 120 or not tok.get("refresh_token"):
        return tok
    p = PROVIDERS[provider]
    data = {"grant_type": "refresh_token", "refresh_token": tok["refresh_token"]}
    headers = {}
    if p["basic_auth"]:
        basic = base64.b64encode(f"{p['client_id']}:{p['client_secret']}".encode()).decode()
        headers["Authorization"] = f"Basic {basic}"
        data["client_id"] = p["client_id"]
    else:
        data[p["id_param"]] = p["client_id"]
        data["client_secret"] = p["client_secret"]
    with httpx.Client(timeout=20) as c:
        r = c.post(p["token_url"], data=data, headers=headers)
        r.raise_for_status()
        new = r.json()
    db.save_token(holder_id, provider, new["access_token"], new.get("refresh_token", tok["refresh_token"]),
                  new.get("expires_in"), tok.get("account"), tok.get("extra"))
    return db.get_token(holder_id, provider)
