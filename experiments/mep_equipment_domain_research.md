# MEP Equipment Domain Research — Session Summary

## Goal

Build a structured vocabulary of MEP (Mechanical, Electrical, Plumbing) equipment
manufacturers and model numbers, suitable for automated classification of text
extracted from construction RFPs, scopes of work, and engineering drawings.

The target documents are NYC affordable housing heat pump retrofit projects —
"Basis of Design" RFPs and "Scope of Work" documents containing equipment
schedules, piping diagrams, and specification tables.

## Source Corpus

21 PDFs in a shared Google Drive folder (MEP evaluation set):
- 11 "Basis of Design RFP" documents for NYC buildings
- 10 "Sample Scope of Work" documents with engineering drawings

These are real MEP documents containing equipment schedules (tabular pages listing
manufacturer, model, capacity, voltage, dimensions) and schematic drawings with
rotated text labels.

## Extraction Method

PDF text extraction at the PDFium "object" level (one text object = one style run),
producing bounding boxes with page, x, y, w, h, text, and style_id. Style IDs
cross-reference to a font/size/color table.

Key observation: equipment schedule pages in engineering drawings contain rotated
text (90° labels) which PDFium reports with very large font sizes (700-2000pt) —
this is the raw font size before the CTM rotation transform, not the visual size.
The actual visual size is the `w` dimension of the bbox (typically 10-25pt).

## Manufacturers Found in Corpus

### Tier 1 — With specific model numbers in the documents

| Manufacturer | Models Found | Equipment Type |
|---|---|---|
| MITSUBISHI | MSZ-FS12NA, MXZ-2C20NAHZ, MXZ-3C24NAHZ, MXZ-3C30NAHZ, MXZ-SM48NAMHZ, SUZ-AA12NLHZ | Mini-split heat pumps |
| COLMAC | CXW-15 | Water-to-water heat pumps |
| AERMEC | NYG0500-G, NYK500 | Heat pumps |
| CALEFFI | 551006A | Air separators (hydronic) |
| TEKMAR | 294, 360 | Controls (outdoor reset, boiler) |
| HEAT-FLO | HF-22-BT | Buffer tanks |
| SANDEN/QAHV | QAHV-N136YAU-HPB(-BS) | CO2 heat pump water heaters |

### Tier 2 — Manufacturer names detected, no specific models

GRUNDFOS, DAIKIN, WATTS, RHEEM, TACO, LOCHINVAR, DANFOSS, A.O. SMITH, LG, YORK

## Industry Classification Codes

| Code | System | Description | Relevance |
|---|---|---|---|
| 333415 | NAICS | AC, Heating, Refrigeration Equipment Mfg | Heat pumps, chillers — 814 companies |
| 332919 | NAICS | Other Metal Valve & Pipe Fitting Mfg | Caleffi, Watts, Danfoss — 473 companies |
| 423720 | NAICS | Plumbing/Heating Equipment Wholesalers (Hydronics) | Distribution channel |
| 3585 | SIC | AC, Heating, Refrigeration Equipment | Same as NAICS 333415 in SEC EDGAR |
| 3443 | SIC | Heat Exchangers / Process Vessels | Heat exchangers, tanks |

SEC EDGAR full-text search for SIC 3585 10-K filers returns companies like
Johnson Controls (JCI), Carrier, Trane Technologies, Daikin.

## NEEA Qualified Products List (Primary External Source)

**URL**: `https://neea.org/wp-content/uploads/2025/03/residential-HPWH-qualified-products-list.pdf`

This is a 38-page PDF maintained by the Northwest Energy Efficiency Alliance listing
all certified residential heat pump water heaters. It is the single richest source found.

**Structure**: Tabular layout with columns:
- x ≈ 28: Manufacturer name
- x ≈ 149: Model number (with wildcards like `***` and `2**`)
- x ≈ 365: Capacity (gallons)
- x ≈ 403: Tier rating
- x ≈ 430: CCE (Cool Climate Efficiency)
- x ≈ 545: Type (Integrated/Split)

**Extracted**: 34 manufacturers, 629 model numbers.

### Manufacturers from NEEA (not already in corpus)

| Manufacturer | Models | Notes |
|---|---|---|
| Rheem | 171 | Largest — ProTerra line, many SKU variants |
| Ruud | 69 | Rheem subsidiary, shared platforms |
| A. O. Smith | 54 | HP1050, HPA10, HPACO series |
| Richmond | 45 | Rheem brand |
| American | 28 | American Water Heater (A.O. Smith subsidiary) |
| State | 27 | State Water Heaters (A.O. Smith subsidiary) |
| Reliance | 25 | Reliance Water Heaters |
| Friedrich | 24 | ProTerra OEM |
| Lochinvar | 22 | HPA, HPPA, HPSA series |
| American Standard | 16 | ASHPWH series |
| GE | 18 | PF/PH series |
| Bradford White | 15 | RE2H AeroTherm series |
| Ariston | 12 | ARIHPWH series |
| Midea | 12 | MAHW, CAN3 series |
| SANCO2 | 12 | GS5-45HPC CO2 heat pumps |
| Harvest Thermal | 12 | PD2A/PD2B series |
| Rinnai | 11 | REHP series |
| Bosch | 6 | TR7000T series |
| Eco-Logical | 6 | ECO-50/65/80HPM1A |
| Navien | 3 | NWP500S series (new entrant) |
| Vaughn Thermal | 4 | ME series |
| Senville | 4 | SENWH-HP series |
| Alsetria | 3 | HPWH series |
| ECO-AIR | 3 | HPWH series |
| Custom Comfort | 3 | CCOHPWH series |
| Novair | 3 | TCL series |
| Lennox | 4 | WHHP series |
| LG | 7 | APHWC series (different from VRF line) |
| stream33 | 2 | S33-HPWH series |
| Gridless | 2 | AYHW series |
| Perfect Aire | 1 | 1PAWH50G |

## Spec Sheet PDFs Successfully Shredded

Web search found direct PDF URLs for manufacturer spec sheets. Successfully
downloaded and extracted from:

| Document | Pages | Bboxes | Models Extracted |
|---|---|---|---|
| Caleffi 551 DISCAL spec (caleffi.com) | 9 | 3143 | 551003A–551054A + accessories |
| Caleffi 551 features (caleffi.com) | 2 | 420 | Full 551 series range |
| Caleffi 551050AT submittal (supplyhouse.com) | 1 | 444 | Flanged variants |
| Mitsubishi MSZ-FS12NA submittal (acdirect.com) | 7 | 688 | 30 models incl. accessories |
| Mitsubishi MSZ-FS12NA multi-zone (acdirect.com) | 3 | 290 | 17 models |
| Rheem ProTerra Plug-in spec (media.rheem.com) | 4 | 461 | 13 part numbers |
| Rheem ProTerra Hybrid with LeakGuard (media.rheem.com) | 4 | 564 | 12 part numbers |
| Bradford White AeroTherm G2 (docs.bradfordwhite.com) | 2 | 523 | RE2HP5010/6510/8010 |
| Navien NWP500 prelaunch (navieninc.com) | 2 | 56 | NWP500S50/S65/S80 |
| Navien NWP500 QuickFacts (activeplumbing.com) | 2 | 231 | NWP500-50/65/80 |

### PDF Sources That Failed

Most manufacturer websites use JavaScript-rendered SPAs or Cloudflare protection:
- caleffi.com product pages → 404 or 403
- colmacwaterheaters.com → connection refused (Cloudflare)
- supplyhouse.com → 403 (bot protection)
- mylinkdrive.com (Mitsubishi) → JS SPA, returns HTML shell
- lochinvar.com → JS redirect
- tacocomfort.com → 404

**Lesson**: Distributor sites (acdirect.com, supplyhouse.com PDF links, fwwebb.com)
and manufacturer CDN paths (media.rheem.com, docs.bradfordwhite.com) are more
reliable for direct PDF access than manufacturer product pages.

## Model Number Patterns

| Manufacturer | Pattern | Example | Encoding |
|---|---|---|---|
| Mitsubishi | MSZ-{series}{capacity}NA | MSZ-FS12NA | FS=series, 12=12kBTU |
| Mitsubishi | MXZ-{n}C{capacity}NAHZ | MXZ-3C30NAHZ | 3=zones, 30=30kBTU |
| Caleffi | 551{variant}{size} | 551006A | 006=size code, A=variant |
| Colmac | CXW-{capacity} | CXW-15 | 15=tons |
| Aermec | NY{series}{capacity} | NYG0500-G | 500=kW×10 |
| Rheem | PRO H{cap} T2 {suffix} | PRO H50 T2 RH310BM | 50=gallons |
| Bradford White | RE2H{cap}{type}-{rev} | RE2H50S10-1NCTT | 50=gallons, S=short |
| Navien | NWP500S{cap} | NWP500S065 | 065=65 gallons |
| A.O. Smith | HP{series}{cap}H45DV | HP1050H45DV | 50=gallons |
| SANDEN | QAHV-N{cap}YAU-HPB | QAHV-N136YAU-HPB | 136=capacity code |
| Tekmar | {3-digit number} | 294, 360 | Number IS the model |

## Combined Domain Inventory

| Source | Manufacturers | Models | Notes |
|---|---|---|---|
| Corpus (RFP/SoW PDFs) | 17 | ~50 | Real-world usage in NYC projects |
| NEEA Qualified Products | 34 | 629 | Certified HPWH products, structured table |
| Spec sheet shredding | 6 | ~100 | Detailed specs, accessories, part numbers |
| **Total (deduplicated)** | **~40** | **~700+** | Ready for domain registration |

## Recommendations for Domain Construction

1. **Primary domain: `mep_hpwh_manufacturers`** — manufacturer names only (34 from NEEA + 17 from corpus, deduplicated). Small, high-signal vocabulary for classifying whether a document is about heat pump equipment.

2. **Secondary domain: `mep_hpwh_models`** — all 700+ model numbers. High cardinality but very specific — a match is almost certainly an equipment reference.

3. **Tertiary domain: `mep_schedule_terms`** — schedule header vocabulary extracted from the engineering drawings: "HEAT EXCHANGER SCHEDULE", "PUMP SCHEDULE", "PIPE INSULATION SCHEDULE", "BUFFER TANK SCHEDULE", "AIR SEPARATOR SCHEDULE", etc. These are structural markers that identify equipment schedule pages.

4. **Future expansion**: Use the NAICS/SIC codes to query EDGAR for more public HVAC companies, then search for their spec sheets. The AHRI directory (ahridirectory.org) is another rich source but requires an API or browser automation.
