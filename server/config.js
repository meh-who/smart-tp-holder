// All configuration comes from environment variables (see .env.example).
const env = process.env;

function list(v, fallback) {
  return (v ?? fallback).split(',').map((s) => s.trim()).filter(Boolean);
}

export const config = {
  port: Number(env.PORT || 8080),
  // Public URL of this server, used for OAuth redirect URIs.
  publicUrl: (env.PUBLIC_URL || `http://localhost:${env.PORT || 8080}`).replace(/\/$/, ''),
  // Shared secret the holder presents when it connects. Change this.
  deviceKey: env.DEVICE_KEY || 'change-me',
  // Holders this server knows about. The NFC tag on each points to /?h=<id>.
  holders: list(env.HOLDER_IDS, 'roll-1'),
  // Visitor accounts are kept in memory only and forgotten after this many idle minutes.
  authTtlMinutes: Number(env.AUTH_TTL_MINUTES || 30),
  // Demo mode: services "connect" without real OAuth and read canned text. For testing and rehearsals.
  demo: env.DEMO === '1' || env.DEMO === 'true',
  news: {
    feeds: list(env.NEWS_FEEDS, 'https://feeds.bbci.co.uk/news/world/rss.xml,https://rss.nytimes.com/services/xml/rss/nyt/HomePage.xml'),
  },
  x: {
    clientId: env.X_CLIENT_ID || '',
    clientSecret: env.X_CLIENT_SECRET || '',
  },
  tiktok: {
    clientKey: env.TIKTOK_CLIENT_KEY || '',
    clientSecret: env.TIKTOK_CLIENT_SECRET || '',
  },
  instagram: {
    appId: env.INSTAGRAM_APP_ID || '',
    appSecret: env.INSTAGRAM_APP_SECRET || '',
  },
  stateFile: env.STATE_FILE || new URL('../data/state.json', import.meta.url).pathname,
};
