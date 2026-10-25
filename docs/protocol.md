# Holder ↔ server protocol

The holder (ESP32 or similar) opens one WebSocket to the server and keeps it open. Connecting out from the holder means it works on gallery or home Wi-Fi without port forwarding.

```
ws://<server>/device?id=<HOLDER_ID>&key=<DEVICE_KEY>
wss://<server>/device?id=...            (when the server is behind HTTPS)
```

A wrong id or key closes the socket with code `4001`. If a second connection arrives for the same holder, the older one is closed with `4000`.

The server pings every 15 s; a holder that misses a pong is dropped and shown as "Not connected" in the web control. Most WebSocket libraries answer pings automatically.

All messages are JSON text frames.

## Holder → server

| Message | When | Effect |
|---|---|---|
| `{"type":"hello","firmware":"0.3.1"}` | Right after connecting | Records the firmware version |
| `{"type":"pull","sheets":2}` | The encoder has counted sheets pulled | Adds to the sheet counters shown on the web control |
| `{"type":"next"}` | The holder is ready to read the next item | Server replies with an `item` |

A typical pull: the encoder detects movement → send `pull` with the sheet count → send `next` → speak the `item` text that comes back. Debounce on the holder so one long pull asks for one item, not dozens.

## Server → holder

| Message | When |
|---|---|
| `{"type":"source","source":"news"}` | On connect, and whenever a visitor switches source |
| `{"type":"item","id":"…","source":"x","text":"…"}` | In reply to `next` |

`text` is already cleaned for speech: no URLs or HTML. If the chosen service needs an account and nobody is connected, `text` is a prompt asking the visitor to tap the holder with their phone. Source ids: `news`, `x`, `tiktok`, `instagram`.

## Sketch for ESP32 (Arduino)

Using the `ArduinoWebsockets` and `ArduinoJson` libraries:

```cpp
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
using namespace websockets;

WebsocketsClient ws;

void onMessage(WebsocketsMessage m) {
  JsonDocument doc;
  if (deserializeJson(doc, m.data())) return;
  if (doc["type"] == "item") speak(doc["text"].as<const char*>());   // your TTS / audio path
}

void connectServer() {
  ws.onMessage(onMessage);
  ws.connect("ws://192.168.1.20:8080/device?id=roll-1&key=YOUR_KEY");
  ws.send("{\"type\":\"hello\",\"firmware\":\"0.1\"}");
}

void onSheetsPulled(int n) {       // call from your scroll-wheel encoder handler
  ws.send(String("{\"type\":\"pull\",\"sheets\":") + n + "}");
  ws.send("{\"type\":\"next\"}");
}

void loop() {
  ws.poll();
  if (!ws.available()) { delay(2000); connectServer(); }
}
```
