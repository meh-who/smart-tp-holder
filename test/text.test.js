import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseRss } from '../server/providers/news.js';
import { speakable } from '../server/providers/oauth.js';

test('speakable strips links, tags and entities', () => {
  assert.equal(speakable('Read <b>this</b> &amp; that https://t.co/abc  now'), 'Read this & that now');
});

test('parseRss reads titles and descriptions, including CDATA', () => {
  const xml = `<rss><channel>
    <item><title><![CDATA[Rain due]]></title><description>Through Tuesday.</description><guid>a</guid></item>
    <item><title>Same</title><description>Same</description></item>
  </channel></rss>`;
  assert.deepEqual(parseRss(xml), [
    { id: 'a', text: 'Rain due. Through Tuesday.' },
    { id: 'Same', text: 'Same' },
  ]);
});
