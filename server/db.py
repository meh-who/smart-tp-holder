"""db.py — one SQLite file; the whole exhibition fits in it comfortably.

Tables
  holders     one row per physical holder (the id is what the NFC tag points at)
  tokens      OAuth tokens per (holder, provider)
  heartbeats  latest telemetry per holder (one row, overwritten)
  played      item ids a holder has spoken, with a timestamp (12-hour no-repeat)
  bulletins   the current bulletin JSON per holder
"""
import json
import os
import sqlite3
import threading
import time
from contextlib import contextmanager

DB_PATH = os.environ.get("HOLDER_DB", os.path.join(os.path.dirname(__file__), "data", "holder.sqlite"))
_lock = threading.RLock()

SCHEMA = """
CREATE TABLE IF NOT EXISTS holders (
  id TEXT PRIMARY KEY, key TEXT NOT NULL, name TEXT, source TEXT NOT NULL DEFAULT 'news',
  desired_source TEXT NOT NULL DEFAULT 'news', created REAL
);
CREATE TABLE IF NOT EXISTS tokens (
  holder_id TEXT, provider TEXT, access_token TEXT, refresh_token TEXT, expires_at REAL,
  account TEXT, extra TEXT, PRIMARY KEY (holder_id, provider)
);
CREATE TABLE IF NOT EXISTS heartbeats (holder_id TEXT PRIMARY KEY, ts REAL, payload TEXT);
CREATE TABLE IF NOT EXISTS played (holder_id TEXT, item_id TEXT, ts REAL, PRIMARY KEY (holder_id, item_id));
CREATE TABLE IF NOT EXISTS bulletins (holder_id TEXT PRIMARY KEY, source TEXT, version TEXT, ts REAL, payload TEXT, status TEXT, error TEXT);
"""


def init():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    with conn() as c:
        c.executescript(SCHEMA)


@contextmanager
def conn():
    with _lock:
        c = sqlite3.connect(DB_PATH, check_same_thread=False)
        c.row_factory = sqlite3.Row
        try:
            yield c
            c.commit()
        finally:
            c.close()


# ---- holders -----------------------------------------------------------------
def add_holder(hid, key, name=None):
    with conn() as c:
        c.execute("INSERT OR REPLACE INTO holders (id,key,name,source,desired_source,created) VALUES (?,?,?,?,?,?)",
                  (hid, key, name or hid, "news", "news", time.time()))


def get_holder(hid):
    with conn() as c:
        r = c.execute("SELECT * FROM holders WHERE id=?", (hid,)).fetchone()
        return dict(r) if r else None


def list_holders():
    with conn() as c:
        return [dict(r) for r in c.execute("SELECT * FROM holders ORDER BY id")]


def set_desired_source(hid, source):
    with conn() as c:
        c.execute("UPDATE holders SET desired_source=? WHERE id=?", (source, hid))


def set_source(hid, source):
    with conn() as c:
        c.execute("UPDATE holders SET source=? WHERE id=?", (source, hid))


# ---- tokens ------------------------------------------------------------------
def save_token(hid, provider, access_token, refresh_token=None, expires_in=None, account=None, extra=None):
    with conn() as c:
        c.execute("INSERT OR REPLACE INTO tokens VALUES (?,?,?,?,?,?,?)",
                  (hid, provider, access_token, refresh_token,
                   time.time() + expires_in if expires_in else None, account, json.dumps(extra or {})))


def get_token(hid, provider):
    with conn() as c:
        r = c.execute("SELECT * FROM tokens WHERE holder_id=? AND provider=?", (hid, provider)).fetchone()
        if not r:
            return None
        d = dict(r)
        d["extra"] = json.loads(d.get("extra") or "{}")
        return d


def delete_token(hid, provider):
    with conn() as c:
        c.execute("DELETE FROM tokens WHERE holder_id=? AND provider=?", (hid, provider))


def connected_providers(hid):
    with conn() as c:
        return {r["provider"]: r["account"] for r in c.execute("SELECT provider, account FROM tokens WHERE holder_id=?", (hid,))}


# ---- heartbeats --------------------------------------------------------------
def save_heartbeat(hid, payload):
    with conn() as c:
        c.execute("INSERT OR REPLACE INTO heartbeats VALUES (?,?,?)", (hid, time.time(), json.dumps(payload)))


def get_heartbeat(hid):
    with conn() as c:
        r = c.execute("SELECT * FROM heartbeats WHERE holder_id=?", (hid,)).fetchone()
        return (r["ts"], json.loads(r["payload"])) if r else (None, None)


# ---- played ------------------------------------------------------------------
def mark_played(hid, item_ids):
    now = time.time()
    with conn() as c:
        c.executemany("INSERT OR REPLACE INTO played VALUES (?,?,?)", [(hid, i, now) for i in item_ids])
        c.execute("DELETE FROM played WHERE ts < ?", (now - 7 * 86400,))


def recently_played(hid, hours=12):
    with conn() as c:
        return {r["item_id"] for r in c.execute("SELECT item_id FROM played WHERE holder_id=? AND ts>?",
                                                 (hid, time.time() - hours * 3600))}


# ---- bulletins ---------------------------------------------------------------
def save_bulletin(hid, source, version, payload, status="ready", error=None):
    with conn() as c:
        c.execute("INSERT OR REPLACE INTO bulletins VALUES (?,?,?,?,?,?,?)",
                  (hid, source, version, time.time(), json.dumps(payload) if payload is not None else None, status, error))


def set_bulletin_status(hid, status, error=None):
    with conn() as c:
        r = c.execute("SELECT 1 FROM bulletins WHERE holder_id=?", (hid,)).fetchone()
        if r:
            c.execute("UPDATE bulletins SET status=?, error=? WHERE holder_id=?", (status, error, hid))
        else:
            c.execute("INSERT INTO bulletins VALUES (?,?,?,?,?,?,?)", (hid, None, None, time.time(), None, status, error))


def get_bulletin(hid):
    with conn() as c:
        r = c.execute("SELECT * FROM bulletins WHERE holder_id=?", (hid,)).fetchone()
        if not r:
            return None
        d = dict(r)
        d["payload"] = json.loads(d["payload"]) if d["payload"] else None
        return d
