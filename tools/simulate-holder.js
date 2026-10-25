#!/usr/bin/env node
// Pretends to be the holder, so the web control can be tested without hardware.
//
//   npm run simulate                       press Enter to pull a sheet
//   npm run simulate -- --every 4          pull automatically every 4 seconds
//
// Env: SERVER (default ws://localhost:8080), HOLDER_ID (default roll-1), DEVICE_KEY (default change-me)
import WebSocket from 'ws';
import readline from 'node:readline';

const server = (process.env.SERVER || 'ws://localhost:8080').replace(/^http/, 'ws');
const id = process.env.HOLDER_ID || 'roll-1';
const key = process.env.DEVICE_KEY || 'change-me';
const everyIdx = process.argv.indexOf('--every');
const every = everyIdx > -1 ? Number(process.argv[everyIdx + 1]) : 0;

const ws = new WebSocket(`${server}/device?id=${encodeURIComponent(id)}&key=${encodeURIComponent(key)}`);

function pull() {
  const sheets = 1 + Math.floor(Math.random() * 3);
  ws.send(JSON.stringify({ type: 'pull', sheets }));
  ws.send(JSON.stringify({ type: 'next' }));
  process.stdout.write(`  pulled ${sheets} sheet${sheets > 1 ? 's' : ''}\n`);
}

ws.on('open', () => {
  console.log(`Connected to ${server} as ${id}.`);
  ws.send(JSON.stringify({ type: 'hello', firmware: 'simulator-0.1' }));
  if (every > 0) {
    console.log(`Pulling every ${every}s. Ctrl+C to stop.`);
    setInterval(pull, every * 1000);
  } else {
    console.log('Press Enter to pull a sheet. Ctrl+C to stop.');
    readline.createInterface({ input: process.stdin }).on('line', pull);
  }
});

ws.on('message', (raw) => {
  const msg = JSON.parse(raw);
  if (msg.type === 'source') console.log(`[source] now reading: ${msg.source}`);
  else if (msg.type === 'item') console.log(`[speak] (${msg.source}) ${msg.text}`);
  else console.log('[server]', msg);
});

ws.on('close', (code, reason) => {
  console.log(`Disconnected (${code}) ${reason}`);
  process.exit(code === 4001 ? 1 : 0);
});
ws.on('error', (err) => {
  console.error(`Couldn't connect to ${server}: ${err.message}`);
  process.exit(1);
});
