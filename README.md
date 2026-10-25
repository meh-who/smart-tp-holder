# smart-tp-holder

Web control for *Untitled (toilet roll holder)*: a toilet roll holder that reads a feed aloud as paper is pulled.

A visitor taps their phone on the NFC tag on the holder. That opens this page, which shows:

- whether the holder is connected, and the line it's reading right now
- which source the roll reads (News, X, TikTok, Reels), with a switch to change it
- a way to connect their own account, and to forget it again

```
 phone ──NFC tap──▶ web control (this repo) ◀──WebSocket── holder (ESP32 + scroll-wheel encoder)
                         │
                         └── News RSS / X / TikTok / Instagram APIs
```

The holder connects *out* to the server, so it works on any Wi-Fi without port forwarding. The server does the API work and sends the holder plain text to speak.

## Run it

Needs Node 20+.

```sh
npm install
DEMO=1 npm start            # http://localhost:8080
npm run simulate            # in a second terminal: a fake holder; press Enter to pull a sheet
```

`DEMO=1` lets every service "connect" without real credentials and read invented text, so you can rehearse the whole flow. News still reads real RSS headlines when the network allows.

For real use, copy `.env.example` to `.env`, fill it in, and run `node --env-file=.env server/index.js`.

## The NFC tag

Write one URL record to the tag (NTAG213 or larger) with any NFC writer app:

```
https://<your-server>/?h=roll-1
```

`h` is the holder id from `HOLDER_IDS`. One server can run several holders, each with its own tag.

## Hosting

The page must be reachable from visitors' phones, and OAuth needs HTTPS. Options:

- **A small host** (Render, Fly.io, Railway, a VPS): deploy this repo, set the env vars, point the holder at `wss://<host>/device`.
- **A laptop or Raspberry Pi at the venue**, exposed with a tunnel (Cloudflare Tunnel, ngrok). Good for offline-ish shows; run with `DEMO=1` if the venue network blocks the APIs.

Set `PUBLIC_URL` to the HTTPS address, and register `<PUBLIC_URL>/auth/<service>/callback` with each service.

## Services

| Source | Account needed | What it reads | Setup |
|---|---|---|---|
| News | No | Headlines and summaries from `NEWS_FEEDS` | Any RSS URLs |
| X | Yes | The visitor's home timeline | X developer app, OAuth 2.0, `tweet.read users.read`. Timeline access depends on your API tier. |
| TikTok | Yes | Captions of the visitor's own videos | TikTok Login Kit app with `user.info.basic`, `video.list`; needs TikTok app review for public use. |
| Reels | Yes | Captions of the visitor's own Reels | Meta app using Instagram API with Instagram Login, `instagram_business_basic`. Works with professional (business/creator) accounts. |

None of these platforms expose their recommendation feeds (For You, Reels tab) through public APIs, so TikTok and Reels read the visitor's own posts. The provider code lives in `server/providers/`; endpoints should be checked against each platform's current docs when you register the apps.

## Privacy

The holder sits in a bathroom, so visitor accounts are treated as borrowed:

- tokens are held in memory only, never written to disk
- only one visitor account per holder at a time; connecting a new one replaces the old
- an account is forgotten after `AUTH_TTL_MINUTES` without use (default 30), or straight away with **Forget my account**, and the roll falls back to News

## Holder firmware

See [docs/protocol.md](docs/protocol.md) for the WebSocket messages and an ESP32 sketch.

## Layout

```
server/index.js        HTTP, live updates, OAuth, device WebSocket
server/providers/      one file per source
server/demo.js         invented lines for DEMO=1
public/                the web control (no build step)
tools/simulate-holder.js
docs/protocol.md
```
