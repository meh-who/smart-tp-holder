const $ = (sel) => document.querySelector(sel);
const roll = $('.roll');
const lineEl = $('#line');
const form = $('#sources');
const rows = new Map(); // source id -> { label, input, side }
let holderId = null;
let current = null;
let lastLine = undefined;

// ---------- helpers ----------

let toastTimer;
function toast(text, { error = false } = {}) {
  const el = $('#toast');
  el.textContent = text;
  el.classList.toggle('is-error', error);
  el.classList.add('is-shown');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('is-shown'), error ? 6000 : 3500);
}

function ago(iso) {
  if (!iso) return null;
  const s = Math.max(0, (Date.now() - new Date(iso).getTime()) / 1000);
  if (s < 60) return 'just now';
  const m = Math.round(s / 60);
  if (m < 60) return `${m} min ago`;
  const h = Math.round(m / 60);
  if (h < 48) return `${h} hr ago`;
  return `${Math.round(h / 24)} days ago`;
}

function el(tag, props = {}, text) {
  const node = Object.assign(document.createElement(tag), props);
  if (text != null) node.textContent = text;
  return node;
}

async function api(path, body) {
  const res = await fetch(path, {
    method: body ? 'POST' : 'GET',
    headers: body ? { 'Content-Type': 'application/json' } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw Object.assign(new Error(data.error || `Request failed (${res.status})`), data);
  return data;
}

// ---------- rendering ----------

function renderStatus(h) {
  roll.classList.toggle('is-online', h.online);
  roll.classList.toggle('is-offline', !h.online);
  const seen = ago(h.lastSeen);
  $('#status-text').textContent = h.online
    ? 'Connected'
    : seen ? `Not connected. Last seen ${seen}.` : 'Not connected yet. Check the holder has power and Wi-Fi.';
}

function renderLine(snap) {
  const h = snap.holder;
  const text = h.line || 'Pull a sheet to start.';
  const p = lineEl.querySelector('p');
  if (text !== lastLine) {
    p.textContent = text;
    lineEl.classList.toggle('is-idle', !h.line);
    if (lastLine !== undefined) {
      lineEl.classList.remove('is-new');
      void lineEl.offsetWidth; // restart the animation
      lineEl.classList.add('is-new');
    }
    lastLine = text;
  }

  const meta = $('#meta');
  meta.replaceChildren();
  const src = snap.sources.find((s) => s.id === (h.lineSource || h.source));
  if (h.line && src) {
    meta.append('From ', el('strong', {}, src.name), '. ');
  }
  meta.append(`${h.sheetsToday.toLocaleString()} ${h.sheetsToday === 1 ? 'sheet' : 'sheets'} pulled today.`);
  if (h.feedError) meta.append(el('span', { className: 'warn' }, h.feedError));
}

function buildRow(source) {
  const frag = $('#source-row').content.cloneNode(true);
  const label = frag.querySelector('.source');
  const input = frag.querySelector('input');
  input.value = source.id;
  input.id = `src-${source.id}`;
  label.querySelector('.name').textContent = source.name;
  label.querySelector('.blurb').textContent = source.blurb;
  form.append(frag);
  const row = { label, input, side: label.querySelector('.side') };
  rows.set(source.id, row);
  return row;
}

function renderSources(snap) {
  for (const s of snap.sources) {
    const row = rows.get(s.id) || buildRow(s);
    row.input.checked = snap.holder.source === s.id;
    row.input.disabled = !s.available;
    row.label.classList.toggle('is-disabled', !s.available);
    row.side.replaceChildren();
    if (!s.available) {
      row.side.textContent = 'Not set up';
    } else if (s.needsAuth && !s.connected) {
      row.side.append(el('a', { className: 'button', href: `/auth/${s.id}/start?h=${encodeURIComponent(holderId)}` }, 'Connect'));
    } else if (s.account) {
      row.side.append(el('span', { className: 'account-name' }, s.account));
    }
  }
}

function renderAccount(snap) {
  const connected = snap.sources.find((s) => s.needsAuth && s.connected);
  const section = $('#account');
  section.hidden = !connected;
  if (!connected) return;
  const text = $('#account-text');
  text.replaceChildren(
    'Connected as ', el('strong', {}, connected.account), ` on ${connected.name}. `,
    `The holder forgets it after ${snap.authTtlMinutes} minutes without use, or when you tap below.`,
  );
  $('#forget').dataset.source = connected.id;
}

function render(snap) {
  current = snap;
  roll.setAttribute('aria-busy', 'false');
  renderStatus(snap.holder);
  renderLine(snap);
  renderSources(snap);
  renderAccount(snap);
}

// ---------- actions ----------

form.addEventListener('change', async (e) => {
  const id = e.target.value;
  const source = current?.sources.find((s) => s.id === id);
  if (!source) return;
  if (source.needsAuth && !source.connected) {
    location.href = `/auth/${id}/start?h=${encodeURIComponent(holderId)}`;
    return;
  }
  try {
    render(await api(`/api/holders/${holderId}/source`, { source: id }));
    toast(`The roll now reads ${source.name}.`);
  } catch (err) {
    render(current); // put the selection back
    toast(err.message, { error: true });
  }
});

$('#forget').addEventListener('click', async (e) => {
  const id = e.currentTarget.dataset.source;
  try {
    render(await api(`/api/holders/${holderId}/forget/${id}`, {}));
    toast('Your account is forgotten. The roll is back on News.');
  } catch (err) {
    toast(err.message, { error: true });
  }
});

// ---------- start ----------

function connectEvents() {
  const es = new EventSource(`/api/holders/${encodeURIComponent(holderId)}/events`);
  es.onmessage = (e) => render(JSON.parse(e.data));
  es.onerror = () => {
    $('#status-text').textContent = 'Can’t reach the holder’s server. Retrying.';
    roll.classList.remove('is-online');
    roll.classList.add('is-offline');
  };
}

async function start() {
  const params = new URLSearchParams(location.search);
  holderId = params.get('h');
  if (!holderId) {
    try {
      holderId = (await api('/api/holders'))[0];
    } catch {
      $('#status-text').textContent = 'Can’t reach the holder’s server.';
      return;
    }
  }

  history.replaceState(null, '', `/?h=${encodeURIComponent(holderId)}`);

  try {
    render(await api(`/api/holders/${encodeURIComponent(holderId)}`));
  } catch (err) {
    $('#status-text').textContent = err.message;
    return;
  }

  // Messages coming back from an account sign-in.
  if (params.has('auth_error')) toast(params.get('auth_error'), { error: true });
  else if (params.has('connected')) {
    const name = current.sources.find((s) => s.id === params.get('connected'))?.name || 'Your account';
    toast(`${name} connected. Pull a sheet.`);
  }
  connectEvents();
  // Keep "last seen" fresh while the page is open.
  setInterval(() => current && renderStatus(current.holder), 30000);
}

start();
