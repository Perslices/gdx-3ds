#!/usr/bin/env python3
"""Embed current-layout.json into index.html's seed block so the page stays self-contained."""
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent


def main() -> int:
    layout = json.loads((HERE / "current-layout.json").read_text())
    page_path = HERE / "index.html"
    page = page_path.read_text()
    payload = json.dumps(layout, separators=(",", ":")).replace("</", "<\\/")
    pattern = re.compile(r'(<script id="seed" type="application/json">)/\*SEED\*/.*?/\*END-SEED\*/(</script>)', re.S)
    if not pattern.search(page):
        print("seed markers not found in index.html", file=sys.stderr)
        return 1
    page = pattern.sub(lambda m: m.group(1) + "/*SEED*/" + payload + "/*END-SEED*/" + m.group(2), page, count=1)
    page_path.write_text(page)
    print(f"embedded {len(layout['screens'])} screens, {sum(len(s['elements']) for s in layout['screens'])} elements")
    return 0


if __name__ == "__main__":
    sys.exit(main())
