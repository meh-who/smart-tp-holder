import { config } from '../config.js';
import { speakable } from './oauth.js';

function parseRss(xml) {
  const items = [];
  for (const m of xml.matchAll(/<item[\s>][\s\S]*?<\/item>/g)) {
    const block = m[0];
    const pick = (tag) => {
      const r = block.match(new RegExp(`<${tag}[^>]*>([\\s\\S]*?)<\\/${tag}>`));
      return r ? r[1].replace(/^<!\[CDATA\[|\]\]>$/g, '') : '';
    };
    const title = speakable(pick('title'));
    const desc = speakable(pick('description'));
    if (!title) continue;
    items.push({ id: pick('guid') || pick('link') || title, text: desc && desc !== title ? `${title}. ${desc}` : title });
  }
  return items;
}

export const news = {
  id: 'news',
  name: 'News',
  blurb: 'Headlines from the wire, as they come in.',
  needsAuth: false,
  configured: () => config.news.feeds.length > 0,
  async fetchItems() {
    const results = await Promise.allSettled(
      config.news.feeds.map(async (url) => {
        const res = await fetch(url, { headers: { 'User-Agent': 'smart-tp-holder/0.1' } });
        if (!res.ok) throw new Error(`${url} returned ${res.status}`);
        return parseRss(await res.text());
      }),
    );
    const items = results.flatMap((r) => (r.status === 'fulfilled' ? r.value : []));
    if (!items.length) {
      const reasons = results.filter((r) => r.status === 'rejected').map((r) => r.reason.message);
      throw new Error(`No news items could be fetched. ${reasons.join('; ')}`);
    }
    // Interleave feeds so one source doesn't dominate.
    return items.sort(() => Math.random() - 0.5);
  },
};

export { parseRss };
