// Instagram Reels — Instagram API with Instagram Login. Meta does not expose the Reels
// feed; this reads captions from the visitor's own Reels. Works with professional
// (business/creator) accounts. Register this redirect URI in the Meta app dashboard:
//   <PUBLIC_URL>/auth/instagram/callback
import { config } from '../config.js';
import { getJson, postForm, speakable } from './oauth.js';

export const instagram = {
  id: 'instagram',
  name: 'Reels',
  blurb: 'Captions from your Instagram Reels.',
  needsAuth: true,
  configured: () => Boolean(config.instagram.appId && config.instagram.appSecret),
  authorizeUrl({ state, redirectUri }) {
    const q = new URLSearchParams({
      client_id: config.instagram.appId,
      redirect_uri: redirectUri,
      response_type: 'code',
      scope: 'instagram_business_basic',
      state,
    });
    return `https://www.instagram.com/oauth/authorize?${q}`;
  },
  async exchange({ code, redirectUri }) {
    const tok = await postForm('https://api.instagram.com/oauth/access_token', {
      client_id: config.instagram.appId,
      client_secret: config.instagram.appSecret,
      grant_type: 'authorization_code',
      redirect_uri: redirectUri,
      code,
    });
    const me = await getJson(`https://graph.instagram.com/me?fields=username&access_token=${encodeURIComponent(tok.access_token)}`);
    return { accessToken: tok.access_token, account: `@${me.username}` };
  },
  async fetchItems(session) {
    const body = await getJson(
      `https://graph.instagram.com/me/media?fields=id,caption,media_product_type&limit=50&access_token=${encodeURIComponent(session.accessToken)}`,
    );
    const media = body.data || [];
    const reels = media.filter((m) => m.media_product_type === 'REELS');
    return (reels.length ? reels : media)
      .map((m) => ({ id: m.id, text: speakable(m.caption) }))
      .filter((m) => m.text);
  },
};
