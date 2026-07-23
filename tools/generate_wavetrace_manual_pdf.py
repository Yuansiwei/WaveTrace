#!/usr/bin/env python3
"""Render the user-facing WaveTrace Markdown manual as a polished PDF."""

from __future__ import annotations

import argparse
import html
import re
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    BaseDocTemplate,
    Frame,
    HRFlowable,
    ListFlowable,
    ListItem,
    PageBreak,
    PageTemplate,
    Paragraph,
    Preformatted,
    Spacer,
    Table,
    TableStyle,
)


NAVY = colors.HexColor("#15324B")
BLUE = colors.HexColor("#246B9E")
LIGHT_BLUE = colors.HexColor("#EAF2F8")
GRID = colors.HexColor("#C7D3DD")
TEXT = colors.HexColor("#263746")
MUTED = colors.HexColor("#687986")
CODE_RED = "#A63A35"


def register_fonts() -> None:
    fonts = Path("C:/Windows/Fonts")
    pdfmetrics.registerFont(TTFont("YaHei", str(fonts / "msyh.ttc")))
    pdfmetrics.registerFont(TTFont("YaHeiBold", str(fonts / "msyhbd.ttc")))
    pdfmetrics.registerFont(TTFont("Consolas", str(fonts / "consola.ttf")))


def inline_markup(text: str) -> str:
    escaped = html.escape(text, quote=False)
    escaped = re.sub(
        r"`([^`]+)`",
        lambda m: f'<font name="YaHei" color="{CODE_RED}">{m.group(1)}</font>',
        escaped,
    )
    escaped = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", escaped)
    return escaped


class ManualDocTemplate(BaseDocTemplate):
    def __init__(self, filename: str, **kwargs) -> None:
        super().__init__(filename, **kwargs)
        frame = Frame(
            self.leftMargin,
            self.bottomMargin,
            self.width,
            self.height,
            id="body",
        )
        self.addPageTemplates(PageTemplate(id="manual", frames=[frame], onPage=self.draw_page))

    def draw_page(self, canvas, doc) -> None:
        canvas.saveState()
        width, height = A4
        canvas.setStrokeColor(GRID)
        canvas.setLineWidth(0.45)
        canvas.line(self.leftMargin, height - 15 * mm, width - self.rightMargin, height - 15 * mm)
        canvas.setFont("YaHei", 7.6)
        canvas.setFillColor(MUTED)
        canvas.drawString(self.leftMargin, height - 11.5 * mm, "WaveTrace 波形系统使用说明")
        canvas.drawRightString(width - self.rightMargin, 10 * mm, f"第 {doc.page} 页")
        canvas.restoreState()


def build_styles():
    base = getSampleStyleSheet()
    return {
        "title": ParagraphStyle(
            "TitleCN", parent=base["Title"], fontName="YaHeiBold", fontSize=22,
            leading=30, textColor=NAVY, alignment=TA_CENTER, spaceAfter=10 * mm,
        ),
        "h2": ParagraphStyle(
            "H2CN", parent=base["Heading2"], fontName="YaHeiBold", fontSize=15,
            leading=22, textColor=NAVY, spaceBefore=7 * mm, spaceAfter=3.2 * mm,
            keepWithNext=True,
        ),
        "h3": ParagraphStyle(
            "H3CN", parent=base["Heading3"], fontName="YaHeiBold", fontSize=12,
            leading=18, textColor=BLUE, spaceBefore=5 * mm, spaceAfter=2.2 * mm,
            keepWithNext=True,
        ),
        "h4": ParagraphStyle(
            "H4CN", parent=base["Heading4"], fontName="YaHeiBold", fontSize=10.5,
            leading=16, textColor=NAVY, spaceBefore=3.5 * mm, spaceAfter=1.5 * mm,
            keepWithNext=True,
        ),
        "body": ParagraphStyle(
            "BodyCN", parent=base["BodyText"], fontName="YaHei", fontSize=9.3,
            leading=15.2, textColor=TEXT, alignment=TA_LEFT, spaceAfter=2.3 * mm,
            wordWrap="CJK",
        ),
        "bullet": ParagraphStyle(
            "BulletCN", parent=base["BodyText"], fontName="YaHei", fontSize=9.1,
            leading=14.8, textColor=TEXT, wordWrap="CJK", leftIndent=0,
        ),
        "code": ParagraphStyle(
            "Code", parent=base["Code"], fontName="Consolas", fontSize=7.4,
            leading=10.5, textColor=colors.HexColor("#E8EDF2"),
        ),
        "table_header": ParagraphStyle(
            "TableHeader", fontName="YaHeiBold", fontSize=8.2, leading=12,
            textColor=colors.white, wordWrap="CJK",
        ),
        "table_cell": ParagraphStyle(
            "TableCell", fontName="YaHei", fontSize=7.8, leading=11.5,
            textColor=TEXT, wordWrap="CJK",
        ),
    }


def is_table_separator(line: str) -> bool:
    cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
    return bool(cells) and all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells)


def split_table_row(line: str):
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


def markdown_story(text: str, styles):
    lines = text.replace("\r\n", "\n").split("\n")
    story = []
    paragraph = []
    i = 0

    def flush_paragraph() -> None:
        if paragraph:
            content = " ".join(part.strip() for part in paragraph if part.strip())
            if content:
                story.append(Paragraph(inline_markup(content), styles["body"]))
            paragraph.clear()

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if stripped.startswith("```"):
            flush_paragraph()
            code_lines = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code_lines.append(lines[i].replace("\t", "    "))
                i += 1
            code = Preformatted("\n".join(code_lines), styles["code"], maxLineLength=110)
            code_box = Table([[code]], colWidths=[173 * mm], hAlign="LEFT")
            code_box.setStyle(TableStyle([
                ("BACKGROUND", (0, 0), (-1, -1), colors.HexColor("#20303D")),
                ("BOX", (0, 0), (-1, -1), 0.5, colors.HexColor("#243746")),
                ("LEFTPADDING", (0, 0), (-1, -1), 7),
                ("RIGHTPADDING", (0, 0), (-1, -1), 7),
                ("TOPPADDING", (0, 0), (-1, -1), 6),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 6),
            ]))
            story.extend([Spacer(1, 1.5 * mm), code_box, Spacer(1, 3 * mm)])
        elif stripped.startswith("# "):
            flush_paragraph()
            story.append(Spacer(1, 7 * mm))
            story.append(Paragraph(inline_markup(stripped[2:]), styles["title"]))
            story.append(HRFlowable(width="100%", thickness=1.2, color=BLUE, spaceAfter=5 * mm))
        elif stripped.startswith("## "):
            flush_paragraph()
            story.append(Paragraph(inline_markup(stripped[3:]), styles["h2"]))
        elif stripped.startswith("### "):
            flush_paragraph()
            story.append(Paragraph(inline_markup(stripped[4:]), styles["h3"]))
        elif stripped.startswith("#### "):
            flush_paragraph()
            story.append(Paragraph(inline_markup(stripped[5:]), styles["h4"]))
        elif stripped.startswith("|") and i + 1 < len(lines) and is_table_separator(lines[i + 1]):
            flush_paragraph()
            rows = [split_table_row(stripped)]
            i += 2
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(split_table_row(lines[i]))
                i += 1
            i -= 1
            column_count = max(len(row) for row in rows)
            data = []
            for row_index, row in enumerate(rows):
                row += [""] * (column_count - len(row))
                style = styles["table_header"] if row_index == 0 else styles["table_cell"]
                data.append([Paragraph(inline_markup(cell), style) for cell in row])
            if column_count == 3:
                widths = [34 * mm, 43 * mm, 96 * mm]
            elif column_count == 2:
                widths = [45 * mm, 128 * mm]
            else:
                widths = [173 * mm / column_count] * column_count
            table = Table(data, colWidths=widths, repeatRows=1, hAlign="LEFT")
            table.setStyle(TableStyle([
                ("BACKGROUND", (0, 0), (-1, 0), NAVY),
                ("GRID", (0, 0), (-1, -1), 0.45, GRID),
                ("VALIGN", (0, 0), (-1, -1), "TOP"),
                ("LEFTPADDING", (0, 0), (-1, -1), 5),
                ("RIGHTPADDING", (0, 0), (-1, -1), 5),
                ("TOPPADDING", (0, 0), (-1, -1), 4),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
                ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, LIGHT_BLUE]),
            ]))
            story.extend([table, Spacer(1, 3 * mm)])
        elif stripped.startswith("- "):
            flush_paragraph()
            items = []
            while i < len(lines) and lines[i].strip().startswith("- "):
                items.append(ListItem(
                    Paragraph(inline_markup(lines[i].strip()[2:]), styles["bullet"]),
                    leftIndent=4 * mm,
                ))
                i += 1
            i -= 1
            story.append(ListFlowable(
                items, bulletType="bullet", bulletFontName="YaHei", bulletFontSize=7,
                leftIndent=6 * mm, bulletOffsetY=1.8, spaceAfter=2.5 * mm,
            ))
        elif re.match(r"^\d+\.\s+", stripped):
            flush_paragraph()
            items = []
            while i < len(lines) and re.match(r"^\d+\.\s+", lines[i].strip()):
                item_text = re.sub(r"^\d+\.\s+", "", lines[i].strip())
                items.append(ListItem(Paragraph(inline_markup(item_text), styles["bullet"])))
                i += 1
            i -= 1
            story.append(ListFlowable(
                items, bulletType="1", start="1", bulletFontName="YaHei",
                bulletFontSize=8.5, leftIndent=8 * mm, spaceAfter=2.5 * mm,
            ))
        elif not stripped:
            flush_paragraph()
        else:
            paragraph.append(stripped)
        i += 1
    flush_paragraph()
    return story


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    register_fonts()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    styles = build_styles()
    doc = ManualDocTemplate(
        str(args.output), pagesize=A4, leftMargin=18 * mm, rightMargin=18 * mm,
        topMargin=20 * mm, bottomMargin=17 * mm,
        title="WaveTrace 波形系统使用说明", author="WaveTrace",
    )
    doc.build(markdown_story(args.source.read_text(encoding="utf-8"), styles))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
