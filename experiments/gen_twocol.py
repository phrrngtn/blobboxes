"""Generate a synthetic two-column PDF with known layout."""
from reportlab.lib.pagesizes import letter
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.units import inch
from reportlab.lib import colors

OUT = "/Users/paulharrington/checkouts/blobboxes/test_data/synthetic/two_column.pdf"

styles = getSampleStyleSheet()
col_style = ParagraphStyle('col', parent=styles['Normal'], fontSize=9, leading=11)
header_style = ParagraphStyle('header', parent=styles['Heading1'], fontSize=14)

left_text = """Lorem ipsum dolor sit amet, consectetur adipiscing elit.
Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.
Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris.
Duis aute irure dolor in reprehenderit in voluptate velit esse cillum.
Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia."""

right_text = """Nulla facilisi morbi tempus iaculis urna id volutpat lacus.
Amet consectetur adipiscing elit pellentesque habitant morbi tristique.
Vitae sapien pellentesque habitant morbi tristique senectus et netus.
Feugiat in ante metus dictum at tempor commodo ullamcorper.
Id diam vel quam elementum pulvinar etiam non quam lacus."""

# Table data (spans full width)
table_data = [
    ["Method", "Precision", "Recall", "F1"],
    ["Baseline CNN", "0.891", "0.834", "0.862"],
    ["DETR", "0.923", "0.901", "0.912"],
    ["Table Transformer", "0.956", "0.943", "0.949"],
    ["Our Method", "0.971", "0.958", "0.964"],
]

doc = SimpleDocTemplate(OUT, pagesize=letter,
                        leftMargin=0.75*inch, rightMargin=0.75*inch,
                        topMargin=0.75*inch, bottomMargin=0.75*inch)

elements = []

# Full-width title
elements.append(Paragraph("Deep Learning for Table Detection: A Survey", header_style))
elements.append(Spacer(1, 0.3*inch))

# Two-column body text (using a 2-cell table with no borders)
col_table = Table(
    [[Paragraph(left_text, col_style), Paragraph(right_text, col_style)]],
    colWidths=[3.2*inch, 3.2*inch],
    spaceBefore=0, spaceAfter=0
)
col_table.setStyle(TableStyle([
    ('VALIGN', (0,0), (-1,-1), 'TOP'),
    ('LEFTPADDING', (0,0), (-1,-1), 6),
    ('RIGHTPADDING', (0,0), (-1,-1), 6),
]))
elements.append(col_table)
elements.append(Spacer(1, 0.3*inch))

# Full-width table
elements.append(Paragraph("Table 1: Comparison of Detection Methods", styles['Heading3']))
t = Table(table_data, repeatRows=1)
t.setStyle(TableStyle([
    ('GRID', (0,0), (-1,-1), 0.5, colors.grey),
    ('BACKGROUND', (0,0), (-1,0), colors.HexColor('#4472C4')),
    ('TEXTCOLOR', (0,0), (-1,0), colors.white),
    ('FONTNAME', (0,0), (-1,0), 'Helvetica-Bold'),
    ('FONTSIZE', (0,0), (-1,-1), 9),
]))
elements.append(t)
elements.append(Spacer(1, 0.3*inch))

# More two-column text
col_table2 = Table(
    [[Paragraph(right_text, col_style), Paragraph(left_text, col_style)]],
    colWidths=[3.2*inch, 3.2*inch],
)
col_table2.setStyle(TableStyle([
    ('VALIGN', (0,0), (-1,-1), 'TOP'),
    ('LEFTPADDING', (0,0), (-1,-1), 6),
    ('RIGHTPADDING', (0,0), (-1,-1), 6),
]))
elements.append(col_table2)

doc.build(elements)
print(f"Generated {OUT}")
