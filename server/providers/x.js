// X (Twitter) — OAuth 2.0 with PKCE. Reads the visitor's home timeline.
// Needs an X developer app with "Read" permission and this callback registered:
//   <PUBLIC_URL>/auth/x/callback
// Home timeline access depends on the API tier of your X developer account.
import { config } from '../config.js';
import { getJson, postForm, speakable } from './oauth.js';

export const x = {
  id: 'x',
  name: 'X',
  blurb: 'Your home timeline, post by post.',
  needsAuth: true,
  configured: () => Boolean(config.x.clientId),
  authorizeUrl({ state, challenge, redirectUri }) {
    const q = new URLSearchParams({
      response_type: 'code',
      client_id: config.x.clientId,
      redirect_uri: redirectUri,
      scope: 'tweet.read users.read',
      state,
      code_challenge: challenge,
      code_challenge_method: 'S256',
    });
    return `https://x.com/i/oauth2/authorize?${q}`;
  },
  async exchange({ code, verifier, redirectUri }) {
    const headers = config.x.clientSecret
      ? { Authorization: 'Basic ' + Buffer.from(`${config.x.clientId}:${config.x.clientSecret}`).toString('base64') }
      : {};
    const tok = await postForm('https://api.x.com/2/oauth2/token', {
      grant_type: 'authorization_code',
      code,
      redirect_uri: redirectUri,
      code_verifier: verifier,
      client_id: config.x.clientId,
    }, headers);
    const me = await getJson('https://api.x.com/2/users/me', { Authorization: `Bearer ${tok.access_token}` });
    return { accessToken: tok.access_token, account: `@${me.data.username}`, userId: me.data.id };
  },
  async fetchItems(session) {
    const url = `https://api.x.com/2/users/${session.userId}/timelines/reverse_chronological?max_results=50`;
    const body = await getJson(url, { Authorization: `Bearer ${session.accessToken}` });
    return (body.data || []).map((t) => ({ id: t.id, text: speakable(t.text) })).filter((t) => t.text);
  },
};
