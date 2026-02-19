#!/usr/bin/env python3
"""Test script for the pdf_bboxes Python bindings."""

import json
import sys
from pathlib import Path

import pdf_bboxes


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <file.pdf>", file=sys.stderr)
        sys.exit(1)

    data = Path(sys.argv[1]).read_bytes()

    print("--- fonts (struct) ---")
    for f in pdf_bboxes.fonts(data):
        print(f"  [{f['font_id']}] {f['name']!r} style={f['style']} flags={f['flags']}")

    print("\n--- bboxes (struct, first 5) ---")
    count = 0
    for b in pdf_bboxes.extract(data):
        print(
            f"  p{b['page']} ({b['x']:.1f},{b['y']:.1f} {b['w']:.1f}x{b['h']:.1f}) "
            f"font={b['font_id']} size={b['font_size']} {b['style']} "
            f"{b['color']} {b['text']!r}"
        )
        count += 1
        if count >= 5:
            break

    print("\n--- fonts (json) ---")
    for line in pdf_bboxes.fonts_json(data):
        f = json.loads(line)
        print(f"  [{f['font_id']}] {f['name']!r} style={f['style']} flags={f['flags']}")

    print("\n--- bboxes (json, first 5) ---")
    count = 0
    for line in pdf_bboxes.extract_json(data):
        b = json.loads(line)
        print(
            f"  p{b['page']} ({b['x']:.1f},{b['y']:.1f} {b['w']:.1f}x{b['h']:.1f}) "
            f"font={b['font_id']} size={b['font_size']} {b['style']} "
            f"{b['color']} {b['text']!r}"
        )
        count += 1
        if count >= 5:
            break


if __name__ == "__main__":
    main()
