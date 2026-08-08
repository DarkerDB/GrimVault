from __future__ import annotations

import argparse
import json
import re
import urllib.parse
import urllib.request
from pathlib import Path


def request_json(base: str, path: str, key: str, params: dict | None = None) -> dict:
    url = base.rstrip("/") + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    request = urllib.request.Request(url, headers={
        "Accept": "application/json",
        "Origin": "https://dev.darkerdb.com",
        "X-Api-Key": key,
    })
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def onsite_key(web_root: Path) -> str:
    source = (web_root / "util/api.js").read_text(encoding="utf-8")
    match = re.search(r"export const API_KEY = '([^']+)'", source)
    if not match:
        raise RuntimeError(f"publishable onsite key not found in {web_root}/util/api.js")
    return match.group(1)


def main() -> None:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="Cache authoritative DDB OCR vocabulary")
    parser.add_argument("--base", default="https://api.dev.darkerdb.com")
    parser.add_argument("--web-root", type=Path,
                        default=Path.home()/".katforge/realms/darkerdb.com")
    parser.add_argument("--output", type=Path, default=here/"cache/ddb-catalog.json")
    args = parser.parse_args()
    key = onsite_key(args.web_root)

    items: list[dict] = []
    cursor = None
    while True:
        params = {"limit": 200, "locale": "en", "condense": "true"}
        if cursor:
            params["cursor"] = cursor
        envelope = request_json(args.base, "/v2/items", key, params)
        items.extend(envelope.get("body", []))
        cursor = envelope.get("pagination", {}).get("next")
        if not cursor:
            break

    attributes = request_json(args.base, "/v2/attributes", key,
                              {"locale": "en", "condense": "true", "limit": 200}).get("body", [])
    payload = {
        "source": args.base,
        "patch": envelope.get("patch"),
        "build": envelope.get("build"),
        "items": items,
        "attributes": attributes,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"items={len(items)} attributes={len(attributes)} patch={payload['patch']} output={args.output}")


if __name__ == "__main__":
    main()
