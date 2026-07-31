# blobboxes Project

Every document is a table of boxes. A PDF page, a spreadsheet, a Word file, a web
page — extract each piece of text with its position, and the differences between
the formats stop mattering.

## The idea

`bb('anything.pdf')` returns rows of `(page_id, style_id, x, y, w, h, text,
formula)`. So does `bb('anything.xlsx')`, `.xls`, `.docx`, `.html`, `.txt`.
Downstream analysis is written once against that shape rather than once per
format. See [[BBox As Universal IR]].

Coordinates differ in kind, deliberately: PDF is real geometry in points, while
the grid formats use integer row and column indices. `bboxes_format_int_coords`
is the single source of truth for which is which — hosts must not re-decide it.

## Backends

| format | reader |
| --- | --- |
| PDF | PDFium, `dlopen`'d so the extension loads without it |
| XLSX | xlnt for fonts and styles, plus a pugixml fast path ~7-9x quicker that cannot produce them |
| XLS | libxls for cells and palette, plus our own BIFF/OLE2 walker for formulas, defined names and VBA |
| DOCX | miniz + pugixml |
| HTML | lexbor DOM walk — tables to a grid, flow to reading order |
| text | line-oriented |

Nine parsing engines are linked in total. That redundancy is inherited rather
than chosen, and shrinking it is a deferred piece of work.

## What it is for

The boxes are raw material for the **sieve**: layered SQL that classifies each
cell without embeddings — style signals, `TRY_CAST`, regex, spatial role, then
domain probing against [[blobfilters Project|blobfilters]] bitmaps. Conflicting
readings are resolved as weighted MAX-SAT by [[blobsolver Project|blobsolver]].

## Building

`zig build`. One prerequisite: Zig 0.16.0 — no CMake, no Make, no `configure`.
Optional backends `-Dxlsx -Dxls -Dhtml -Ddocx -Dtext` are all on by default.
`libpdfium` ships beside the extension; because it is `dlopen`'d rather than
linked, the extension still loads without it and only the PDF backend errors.
See [[Building the Blob Family]].

## Related

- [[Application Architecture — Documents as a Table of Boxes]]
- [[Evidence Pipeline Architecture]] — how evidence accumulates across passes
- [[Progressive Masking and Treemap Layout]] — the unbuilt next layer
- [[Document Metadata Schema]], [[blobstemma — Ancestry of Tables Across Manifestations]]
