import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import crypto from 'node:crypto';
import express from 'express';
import { WebSocketServer } from 'ws';
import { config } from './config.js';
import { providers, providerById } from './providers/index.js';
import { pkcePair } from './providers/oauth.js';
import { demoItems } from './demo.js';

const PROMPT_TEXT = 'Tap the holder with your phone to choose what I read.';
const FEED_MAX_AGE_MS = 10 * 60 * 1000;

// ---------------------------------------------------------------------------
// State. Holder settings and counters are saved to disk; visitor accounts are
// kept in memory only and expire, because the holder is in a public space.
// ---------------------------------------------------------------------------

const today = () => new Date().toISOString().slice(0, 10);

function loadSaved() {
  try {
    return JSON.parse(fs.readFileSync(config.stateFile, 'utf8'));
  } catch {
    return {};
  }
}

const saved = loadSaved();
const holders = new Map();
for (const id of config.holders) {
  const s = saved[id] || {};
  holders.set(id, {
    id,
    // Visitor accounts don't survive a restart, so start on a source that needs none.
    source: providerById[s.source] && !providerById[s.source].needsAuth ? s.source : 'news',
    sheetsToday: s.day === today() ? s.sheetsToday || 0 : 0,
    sheetsTotal: s.sheetsTotal || 0,
    day: today(),
    online: false,
    lastSeen: s.lastSeen || null,
    firmware: s.firmware || null,
    line: null,
    lineSource: null,
    feed: { source: null, items: [], index: 0, fetchedAt: 0, error: null },
  });
}

let saveTimer = null;
function save() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    const out = {};
    for (const h of holders.values()) {
      out[h.id] = { source: h.source, sheetsToday: h.sheetsToday, sheetsTotal: h.sheetsTotal, day: h.day, lastSeen: h.lastSeen, firmware: h.firmware };
    }
    fs.mkdirSync(path.dirname(config.stateFile), { recursive: true });
    fs.writeFileSync(config.stateFile, JSON.stringify(out, null, 2));
  }, 500);
}

// sessions: "<holderId>:<providerId>" -> { accessToken, account, userId, lastUsed }
const sessions = new Map();
const sessionKey = (h, p) => `${h}:${p}`;
function getSession(holderId, providerId) {
  const s = sessions.get(sessionKey(holderId, providerId));
  if (!s) return null;
  if (Date.now() - s.lastUsed > config.authTtlMinutes * 60 * 1000) {
    sessions.delete(sessionKey(holderId, providerId));
    return null;
  }
  return s;
}

// Pending OAuth handshakes: state -> { holderId, providerId, verifier, createdAt }
const pending = new Map();

function isAvailable(p) {
  return config.demo || p.configured();
}

function snapshot(h) {
  return {
    holder: {
      id: h.id,
      online: h.online,
      lastSeen: h.lastSeen,
      firmware: h.firmware,
      source: h.source,
      line: h.line,
      lineSource: h.lineSource,
      sheetsToday: h.sheetsToday,
      sheetsTotal: h.sheetsTotal,
      feedError: h.feed.error,
    },
    demo: config.demo,
    authTtlMinutes: config.authTtlMinutes,
    sources: providers.map((p) => {
      const s = p.needsAuth ? getSession(h.id, p.id) : null;
      return {
        id: p.id,
        name: p.name,
        blurb: p.blurb,
        needsAuth: p.needsAuth,
        available: isAvailable(p),
        connected: p.needsAuth ? Boolean(s) : true,
        account: s?.account || null,
      };
    }),
  };
}

// ---------------------------------------------------------------------------
// Live updates to browsers (Server-Sent Events) and commands to the holder.
// ---------------------------------------------------------------------------

const watchers = new Map(); // holderId -> Set<res>
const devices = new Map(); // holderId -> ws

function broadcast(h) {
  const data = `data: ${JSON.stringify(snapshot(h))}\n\n`;
  for (const res of watchers.get(h.id) || []) res.write(data);
}

function sendDevice(h, msg) {
  const ws = devices.get(h.id);
  if (ws && ws.readyState === ws.OPEN) ws.send(JSON.stringify(msg));
}

// ---------------------------------------------------------------------------
// Feed: what the holder reads next.
// ---------------------------------------------------------------------------

async function nextItem(h) {
  const p = providerById[h.source];
  const session = p.needsAuth ? getSession(h.id, p.id) : null;
  if (p.needsAuth && !session) {
    return { id: 'prompt', source: h.source, text: PROMPT_TEXT };
  }
  const feed = h.feed;
  const stale = feed.source !== h.source || feed.index >= feed.items.length || Date.now() - feed.fetchedAt > FEED_MAX_AGE_MS;
  if (stale) {
    try {
      const items = config.demo && (!p.configured() || session?.demo) ? demoItems(p.id) : await p.fetchItems(session);
      h.feed = { source: h.source, items, index: 0, fetchedAt: Date.now(), error: null };
    } catch (err) {
      console.error(`[feed] ${h.id}/${h.source}:`, err.message);
      const fallback = config.demo ? demoItems(p.id) : [];
      const error = `Couldn't reach ${p.name} right now.` + (config.demo ? ' Reading practice lines instead.' : '');
      h.feed = { source: h.source, items: fallback, index: 0, fetchedAt: Date.now(), error };
    }
    if (session) session.lastUsed = Date.now();
  }
  const item = h.feed.items[h.feed.index++];
  if (!item) return { id: 'empty', source: h.source, text: `Nothing to read from ${p.name} right now.` };
  return { ...item, source: h.source };
}

async function serveNext(h) {
  const item = await nextItem(h);
  h.line = item.text;
  h.lineSource = item.source;
  sendDevice(h, { type: 'item', ...item });
  broadcast(h);
}

function setSource(h, source) {
  h.source = source;
  h.feed = { source: null, items: [], index: 0, fetchedAt: 0, error: null };
  sendDevice(h, { type: 'source', source });
  save();
  broadcast(h);
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

const app = express();
app.use(express.json());
app.use(express.static(new URL('../public', import.meta.url).pathname));

function holderFrom(req, res) {
  const h = holders.get(req.params.id);
  if (!h) res.status(404).json({ error: `No holder called "${req.params.id}".` });
  return h;
}

app.get('/api/holders', (_req, res) => res.json(config.holders));

app.get('/api/holders/:id', (req, res) => {
  const h = holderFrom(req, res);
  if (h) res.json(snapshot(h));
});

app.get('/api/holders/:id/events', (req, res) => {
  const h = holderFrom(req, res);
  if (!h) return;
  res.set({ 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', Connection: 'keep-alive' });
  res.flushHeaders();
  res.write(`data: ${JSON.stringify(snapshot(h))}\n\n`);
  if (!watchers.has(h.id)) watchers.set(h.id, new Set());
  watchers.get(h.id).add(res);
  const ping = setInterval(() => res.write(': ping\n\n'), 25000);
  req.on('close', () => {
    clearInterval(ping);
    watchers.get(h.id).delete(res);
  });
});

app.post('/api/holders/:id/source', (req, res) => {
  const h = holderFrom(req, res);
  if (!h) return;
  const p = providerById[req.body?.source];
  if (!p) return res.status(400).json({ error: 'Unknown source.' });
  if (!isAvailable(p)) return res.status(409).json({ error: `${p.name} isn't set up on this holder yet.`, code: 'unavailable' });
  if (p.needsAuth && !getSession(h.id, p.id)) {
    return res.status(409).json({ error: `Connect ${p.name} first.`, code: 'auth_required' });
  }
  setSource(h, p.id);
  res.json(snapshot(h));
});

app.post('/api/holders/:id/forget/:provider', (req, res) => {
  const h = holderFrom(req, res);
  if (!h) return;
  sessions.delete(sessionKey(h.id, req.params.provider));
  if (h.source === req.params.provider) setSource(h, 'news');
  else broadcast(h);
  res.json(snapshot(h));
});

app.get('/auth/:provider/start', (req, res) => {
  const p = providerById[req.params.provider];
  const h = holders.get(String(req.query.h || ''));
  if (!p || !p.needsAuth || !h) return res.redirect('/?auth_error=' + encodeURIComponent('Unknown service or holder.'));
  const back = (q) => res.redirect(`/?h=${encodeURIComponent(h.id)}&${q}`);

  if (config.demo && !p.configured()) {
    sessions.set(sessionKey(h.id, p.id), { demo: true, account: 'demo account', lastUsed: Date.now() });
    setSource(h, p.id);
    return back(`connected=${p.id}`);
  }
  if (!p.configured()) return back('auth_error=' + encodeURIComponent(`${p.name} isn't set up on this holder yet.`));

  const state = crypto.randomBytes(16).toString('hex');
  const { verifier, challenge } = pkcePair();
  pending.set(state, { holderId: h.id, providerId: p.id, verifier, createdAt: Date.now() });
  res.redirect(p.authorizeUrl({ state, challenge, redirectUri: `${config.publicUrl}/auth/${p.id}/callback` }));
});

app.get('/auth/:provider/callback', async (req, res) => {
  const { state, code, error, error_description } = req.query;
  const pend = pending.get(String(state));
  pending.delete(String(state));
  const holderId = pend?.holderId || config.holders[0];
  const back = (q) => res.redirect(`/?h=${encodeURIComponent(holderId)}&${q}`);
  if (error) return back('auth_error=' + encodeURIComponent(String(error_description || error)));
  if (!pend || pend.providerId !== req.params.provider || Date.now() - pend.createdAt > 10 * 60 * 1000) {
    return back('auth_error=' + encodeURIComponent('That sign-in expired. Try connecting again.'));
  }
  const p = providerById[pend.providerId];
  try {
    const result = await p.exchange({ code: String(code), verifier: pend.verifier, redirectUri: `${config.publicUrl}/auth/${p.id}/callback` });
    // Only one visitor account per holder at a time: connecting replaces any other.
    for (const other of providers) if (other.needsAuth) sessions.delete(sessionKey(holderId, other.id));
    sessions.set(sessionKey(holderId, p.id), { ...result, lastUsed: Date.now() });
    setSource(holders.get(holderId), p.id);
    back(`connected=${p.id}`);
  } catch (err) {
    console.error(`[auth] ${p.id}:`, err.message);
    back('auth_error=' + encodeURIComponent(`Couldn't connect ${p.name}. Try again.`));
  }
});

// ---------------------------------------------------------------------------
// Device link: the holder connects out over WebSocket (works behind any router).
//   ws(s)://<server>/device?id=<holderId>&key=<DEVICE_KEY>
// See docs/protocol.md.
// ---------------------------------------------------------------------------

const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: '/device' });

wss.on('connection', (ws, req) => {
  const url = new URL(req.url, 'http://x');
  const h = holders.get(url.searchParams.get('id') || '');
  if (!h || url.searchParams.get('key') !== config.deviceKey) {
    ws.close(4001, 'Unknown holder or wrong key');
    return;
  }
  devices.get(h.id)?.close(4000, 'Replaced by a newer connection');
  devices.set(h.id, ws);
  h.online = true;
  h.lastSeen = new Date().toISOString();
  console.log(`[device] ${h.id} connected`);
  ws.send(JSON.stringify({ type: 'source', source: h.source }));
  broadcast(h);

  let alive = true;
  ws.on('pong', () => (alive = true));
  const heartbeat = setInterval(() => {
    if (!alive) return ws.terminate();
    alive = false;
    ws.ping();
  }, 15000);

  ws.on('message', async (raw) => {
    let msg;
    try {
      msg = JSON.parse(raw);
    } catch {
      return;
    }
    h.lastSeen = new Date().toISOString();
    if (msg.type === 'hello') {
      h.firmware = msg.firmware || null;
    } else if (msg.type === 'pull') {
      const n = Math.max(0, Math.min(1000, Number(msg.sheets) || 0));
      if (h.day !== today()) {
        h.day = today();
        h.sheetsToday = 0;
      }
      h.sheetsToday += n;
      h.sheetsTotal += n;
    } else if (msg.type === 'next') {
      await serveNext(h);
      save();
      return;
    }
    save();
    broadcast(h);
  });

  ws.on('close', () => {
    clearInterval(heartbeat);
    if (devices.get(h.id) === ws) {
      devices.delete(h.id);
      h.online = false;
      h.lastSeen = new Date().toISOString();
      console.log(`[device] ${h.id} disconnected`);
      save();
      broadcast(h);
    }
  });
});

// Expire idle visitor accounts and stale OAuth handshakes.
setInterval(() => {
  for (const h of holders.values()) {
    for (const p of providers) {
      if (!p.needsAuth || !sessions.has(sessionKey(h.id, p.id))) continue;
      if (!getSession(h.id, p.id)) {
        console.log(`[auth] forgot ${p.id} on ${h.id} after ${config.authTtlMinutes} idle minutes`);
        if (h.source === p.id) setSource(h, 'news');
        else broadcast(h);
      }
    }
  }
  for (const [k, v] of pending) if (Date.now() - v.createdAt > 10 * 60 * 1000) pending.delete(k);
}, 60 * 1000);

server.listen(config.port, () => {
  console.log(`Web control on ${config.publicUrl}  (holders: ${config.holders.join(', ')}${config.demo ? ', demo mode' : ''})`);
  if (config.deviceKey === 'change-me') console.warn('DEVICE_KEY is the default. Set your own before exhibiting.');
});
