#!/usr/bin/env python3
"""Test script for the bboxes Python bindings."""

import json
import sys
from pathlib import Path

import bboxes


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <file.pdf>", file=sys.stderr)
        sys.exit(1)

    data = Path(sys.argv[1]).read_bytes()
    cur = bboxes.open_pdf(data)

    print("--- doc ---")
    d = cur.doc()
    print(f"  source={d['source_type']} pages={d['page_count']}")

    print("\n--- pages ---")
    for p in cur.pages():
        print(f"  page {p['page_number']}: {p['width']:.0f}x{p['height']:.0f}")

    print("\n--- fonts ---")
    for f in cur.fonts():
        print(f"  [{f['font_id']}] {f['name']!r}")

    print("\n--- styles ---")
    for s in cur.styles():
        print(
            f"  [{s['style_id']}] font={s['font_id']} size={s['font_size']:.0f} "
            f"{s['weight']} {s['color']} italic={s['italic']}"
        )

    print("\n--- bboxes (first 5) ---")
    for b in cur.bboxes()[:5]:
        print(
            f"  [{b['bbox_id']}] page={b['page_id']} style={b['style_id']} "
            f"({b['x']:.1f},{b['y']:.1f} {b['w']:.1f}x{b['h']:.1f}) {b['text']!r}"
        )

    cur.close()

    print("\n--- JSON: doc ---")
    print(f"  {bboxes.doc_json(data)}")

    print("\n--- JSON: fonts ---")
    fonts = json.loads(bboxes.fonts_json(data))
    for f in fonts:
        print(f"  [{f['font_id']}] {f['name']!r}")


if __name__ == "__main__":
    main()
