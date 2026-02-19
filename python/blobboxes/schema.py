"""Canonical schemas for bboxes object types.

These Pydantic models define the shape of dicts returned by cursor methods.
They serve as documentation and can be used for runtime validation.
"""

from __future__ import annotations

from typing import Optional

from pydantic import BaseModel


class Doc(BaseModel):
    document_id: int
    source_type: str
    filename: Optional[str]
    checksum: str              # MD5 hex of source bytes
    page_count: int


class Page(BaseModel):
    page_id: int
    document_id: int
    page_number: int
    width: float
    height: float


class Font(BaseModel):
    font_id: int
    name: str


class Style(BaseModel):
    style_id: int
    font_id: int
    font_size: float
    color: str
    weight: str
    italic: int
    underline: int


class BBox(BaseModel):
    """Base bbox — shared by all backends."""
    page_id: int
    style_id: int
    x: float
    y: float
    w: float
    h: float
    text: str


class XlsxBBox(BBox):
    """XLSX bbox — adds formula field."""
    formula: Optional[str] = None
