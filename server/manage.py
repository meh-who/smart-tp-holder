#!/usr/bin/env python3
"""manage.py — small admin CLI.

  python manage.py add-holder lisbon-01 <key> --name "TEI Lisbon"
  python manage.py list
  python manage.py build lisbon-01 news        # build a bulletin now (also warms the TTS cache)
  python manage.py tag-url lisbon-01            # the URL to write to the NFC tag
  python manage.py key                          # print a fresh random holder key
"""
import argparse, json, os, secrets, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import db, bulletin, oauth

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("add-holder"); a.add_argument("id"); a.add_argument("key"); a.add_argument("--name")
    sub.add_parser("list")
    b = sub.add_parser("build"); b.add_argument("id"); b.add_argument("source", choices=["news", "x", "tiktok", "reels"])
    t = sub.add_parser("tag-url"); t.add_argument("id")
    sub.add_parser("key")
    args = ap.parse_args()
    db.init()
    if args.cmd == "add-holder":
        db.add_holder(args.id, args.key, args.name); print(f"holder {args.id} saved. Tag URL: {oauth.PUBLIC_URL}/r/{args.id}")
    elif args.cmd == "list":
        for h in db.list_holders():
            ts, hb = db.get_heartbeat(h["id"]); bl = db.get_bulletin(h["id"])
            print(f"{h['id']:14} source={h['source']:7} wants={h['desired_source']:7} "
                  f"bulletin={(bl or {}).get('version') or '-'} tokens={list(db.connected_providers(h['id']))}")
    elif args.cmd == "build":
        p = bulletin.build(args.id, args.source)
        print(json.dumps({k: v for k, v in p.items() if k != "items"}, indent=1)); print(f"{len(p['items'])} items")
        for it in p["items"][:5]: print(f"  {it['duration_ms']:5d} ms  {it['text']}")
    elif args.cmd == "tag-url":
        print(f"{oauth.PUBLIC_URL}/r/{args.id}")
    elif args.cmd == "key":
        print(secrets.token_urlsafe(24))

if __name__ == "__main__":
    main()
