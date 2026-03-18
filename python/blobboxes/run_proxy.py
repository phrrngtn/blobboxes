"""
Launch mitmdump with the blobboxes proxy addon.

Usage:
    uv run python -m blobboxes.run_proxy [mitmdump args...]

Examples:
    # Default: port 8080, max 4 pages
    uv run python -m blobboxes.run_proxy

    # Custom port and concurrency
    uv run python -m blobboxes.run_proxy -p 9090 --set blobboxes_max_pages=2

    # Composed with rule4 traffic observer
    uv run python -m blobboxes.run_proxy -p 8080 \\
        -s rule4/proxy_addon.py \\
        --set rate_limit=5

The blobboxes addon script is automatically prepended to the argument
list. Any additional -s scripts are composed with it (mitmproxy loads
addons in order).
"""

import sys
from pathlib import Path

# Locate the addon script relative to this file
_ADDON = str(Path(__file__).resolve().parent / "proxy_addon.py")


def main():
    # Build mitmdump argv: insert our addon script after 'mitmdump'
    argv = ["mitmdump", "-s", _ADDON] + sys.argv[1:]

    # Default port if not specified
    if "-p" not in argv and "--listen-port" not in argv:
        argv.extend(["-p", "8080"])

    sys.argv = argv
    from mitmproxy.tools.main import mitmdump
    mitmdump()


if __name__ == "__main__":
    main()
