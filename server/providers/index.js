// Each provider describes one thing the roll can read.
//
// Shape:
//   id, name, blurb         - shown in the web control
//   needsAuth               - true if a visitor must connect an account first
//   configured()            - true if the server has the API credentials it needs
//   authorizeUrl({state, challenge, redirectUri})  - OAuth start URL (needsAuth only)
//   exchange({code, verifier, redirectUri})        - returns { accessToken, account }
//   fetchItems(session)     - returns [{ id, text }] for the holder to read

import { news } from './news.js';
import { x } from './x.js';
import { tiktok } from './tiktok.js';
import { instagram } from './instagram.js';

export const providers = [news, x, tiktok, instagram];
export const providerById = Object.fromEntries(providers.map((p) => [p.id, p]));
