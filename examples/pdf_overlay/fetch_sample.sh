#!/usr/bin/env bash
# Public-domain sample: IRS Schedule C (Form 1040). Born-digital, complex layout.
set -euo pipefail
curl -sL --max-time 40 "https://www.irs.gov/pub/irs-pdf/f1040sc.pdf" -o f1040sc.pdf
echo "fetched f1040sc.pdf ($(wc -c < f1040sc.pdf) bytes)"
