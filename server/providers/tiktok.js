// TikTok — Login Kit (OAuth 2.0). The public API does not expose the For You feed;
// with the video.list scope it can read the visitor's own videos, so the roll reads
// their captions. Register this redirect URI in the TikTok developer portal:
//   <PUBLIC_URL>/auth/tiktok/callback
import { config } from '../config.js';
import { postForm, speakable } from './oauth.js';

const API = 'https://open.tiktokapis.com/v2';

export const tiktok = {
  id: 'tiktok',
  name: 'TikTok',
  blurb: 'Captions from your videos.',
  needsAuth: true,
  configured: () => Boolean(config.tiktok.clientKey && config.tiktok.clientSecret),
  authorizeUrl({ state, redirectUri }) {
    const q = new URLSearchParams({
      client_key: config.tiktok.clientKey,
      scope: 'user.info.basic,video.list',
      response_type: 'code',
      redirect_uri: redirectUri,
      state,
    });
    return `https://www.tiktok.com/v2/auth/authorize/?${q}`;
  },
  async exchange({ code, redirectUri }) {
    const tok = await postForm(`${API}/oauth/token/`, {
      client_key: config.tiktok.clientKey,
      client_secret: config.tiktok.clientSecret,
      code,
      grant_type: 'authorization_code',
      redirect_uri: redirectUri,
    });
    const res = await fetch(`${API}/user/info/?fields=display_name`, {
      headers: { Authorization: `Bearer ${tok.access_token}` },
    });
    const info = await res.json().catch(() => ({}));
    return { accessToken: tok.access_token, account: info?.data?.user?.display_name || 'TikTok account' };
  },
  async fetchItems(session) {
    const res = await fetch(`${API}/video/list/?fields=id,title,video_description`, {
      method: 'POST',
      headers: { Authorization: `Bearer ${session.accessToken}`, 'Content-Type': 'application/json' },
      body: JSON.stringify({ max_count: 20 }),
    });
    const body = await res.json();
    if (!res.ok || (body.error && body.error.code !== 'ok')) {
      throw new Error(`TikTok video list failed: ${JSON.stringify(body.error || body).slice(0, 200)}`);
    }
    return (body.data?.videos || [])
      .map((v) => ({ id: v.id, text: speakable(v.video_description || v.title) }))
      .filter((v) => v.text);
  },
};
