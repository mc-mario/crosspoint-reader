#!/usr/bin/env python3
"""
Karakeep Proxy for CrossPoint Reader
====================================

A lightweight middleware that sits between CrossPoint Reader (ESP32 e-ink)
and a Karakeep instance. It offloads all heavy processing from the device:

- Tag filtering and pagination
- HTML-to-text and HTML-to-EPUB conversion
- Read-state tracking via Karakeep tags
- Cover image extraction

The CrossPoint firmware only needs to speak simple HTTP+JSON to this proxy.
"""

import os
import sys
import re
import uuid
import json
import io
import zipfile
import unicodedata
import html
from datetime import datetime
from typing import Optional

import requests
from bs4 import BeautifulSoup, NavigableString
from flask import Flask, jsonify, Response, request
from PIL import Image

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
KARAKEEP_URL = os.environ.get("KARAKEEP_URL", "http://localhost:3000")
KARAKEEP_API_KEY = os.environ.get("KARAKEEP_API_KEY", "")
if not KARAKEEP_API_KEY:
    print("Error: KARAKEEP_API_KEY environment variable is required", file=sys.stderr)
    sys.exit(1)

KARAKEEP_HEADERS = {"Authorization": f"Bearer {KARAKEEP_API_KEY}"}
PROXY_PORT = int(os.environ.get("PROXY_PORT", "8787"))
READ_TAG = os.environ.get("READ_TAG", "Read")

app = Flask(__name__)

# ---------------------------------------------------------------------------
# Karakeep Client
# ---------------------------------------------------------------------------

def karakeep_get(path: str, params: dict = None) -> dict:
    url = f"{KARAKEEP_URL}/api/v1{path}"
    r = requests.get(url, headers=KARAKEEP_HEADERS, params=params, timeout=30)
    r.raise_for_status()
    return r.json()

def karakeep_post(path: str, json_body: dict = None) -> dict:
    url = f"{KARAKEEP_URL}/api/v1{path}"
    r = requests.post(url, headers=KARAKEEP_HEADERS, json=json_body, timeout=30)
    r.raise_for_status()
    return r.json()

def karakeep_patch(path: str, json_body: dict = None) -> dict:
    url = f"{KARAKEEP_URL}/api/v1{path}"
    r = requests.patch(url, headers=KARAKEEP_HEADERS, json=json_body, timeout=30)
    r.raise_for_status()
    return r.json()

def karakeep_delete(path: str, json_body: dict = None) -> None:
    url = f"{KARAKEEP_URL}/api/v1{path}"
    requests.delete(url, headers=KARAKEEP_HEADERS, json=json_body, timeout=30).raise_for_status()


def fetch_all_bookmarks(tag_filter: Optional[str] = None) -> list[dict]:
    """Fetch all bookmarks from Karakeep, optionally filtered by tag server-side."""
    all_bms = []
    cursor = None
    page = 0
    while True:
        params = {"archived": "false", "limit": 50}
        if cursor:
            params["cursor"] = cursor
        data = karakeep_get("/bookmarks", params)
        bookmarks = data.get("bookmarks", [])
        if not bookmarks:
            break
        all_bms.extend(bookmarks)
        if not data.get("hasMore"):
            break
        cursor = data.get("nextCursor")
        page += 1
        if page > 50:  # safety cap
            break
    return all_bms


def filter_bookmarks_by_tag(bookmarks: list[dict], tag_name: str) -> list[dict]:
    """Filter bookmarks whose tags contain the given tag name (case-insensitive)."""
    tag_lower = tag_name.lower()
    filtered = []
    for b in bookmarks:
        tags = [t.get("name", "").lower() for t in b.get("tags", [])]
        if tag_lower in tags:
            filtered.append(b)
    return filtered


def simplify_bookmark(bm: dict) -> dict:
    """Convert a full Karakeep bookmark into a lightweight CrossPoint-friendly object."""
    content = bm.get("content", {})
    title = bm.get("title") or content.get("title") or "Untitled"
    bm_type = content.get("type", "unknown")

    # Find the readable content asset (linkHtmlContent or bookmarkAsset)
    content_asset_id = None
    content_asset_name = None
    cover_asset_id = None
    for asset in bm.get("assets", []):
        atype = asset.get("assetType", "")
        if atype == "linkHtmlContent":
            content_asset_id = asset["id"]
            content_asset_name = asset.get("fileName") or f"{bm['id']}.html"
        elif atype == "bookmarkAsset":
            content_asset_id = asset["id"]
            content_asset_name = asset.get("fileName") or f"{bm['id']}"
        elif atype in ("screenshot", "bannerImage") and not cover_asset_id:
            cover_asset_id = asset["id"]

    is_read = any(
        t.get("name", "").lower() == READ_TAG.lower()
        for t in bm.get("tags", [])
    )

    return {
        "id": bm["id"],
        "title": title,
        "type": bm_type,
        "url": content.get("url", ""),
        "contentAssetId": content_asset_id,
        "contentAssetName": content_asset_name,
        "coverAssetId": cover_asset_id,
        "isRead": is_read,
        "tags": [t["name"] for t in bm.get("tags", [])],
        "createdAt": bm.get("createdAt", ""),
    }


# ---------------------------------------------------------------------------
# HTML Converters
# ---------------------------------------------------------------------------

_UNICODE_TO_ASCII = {
    '\u2013': '-',   '\u2014': '--',   '\u2018': "'",   '\u2019': "'",
    '\u201c': '"',   '\u201d': '"',    '\u2026': '...',  '\u00a0': ' ',
    '\u2022': '*',   '\u00b0': 'deg',  '\u2122': '(TM)', '\u00ae': '(R)',
    '\u00a9': '(c)', '\u201a': ',',    '\u201e': '"',
}

def normalize_unicode(text: str) -> str:
    """Replace common Unicode characters with ASCII equivalents for e-ink rendering."""
    for uni, ascii_ in _UNICODE_TO_ASCII.items():
        text = text.replace(uni, ascii_)
    result = unicodedata.normalize('NFKD', text)
    return result.encode('ascii', 'replace').decode('ascii')


def crop_to_portrait(image_bytes: bytes) -> bytes:
    """Crop an image to portrait ratio (3:4) by center-cropping, then resize to 600x800."""
    try:
        img = Image.open(io.BytesIO(image_bytes))
        w, h = img.size
        if w <= 0 or h <= 0:
            return image_bytes

        # Target ratio: 3:4 (width:height)
        target_ratio = 3.0 / 4.0
        current_ratio = w / h

        if current_ratio > target_ratio:
            # Too wide — crop width from center
            new_w = int(h * target_ratio)
            left = (w - new_w) // 2
            img = img.crop((left, 0, left + new_w, h))
        elif current_ratio < target_ratio * 0.8:
            # Too tall — crop height from center
            new_h = int(w / target_ratio)
            top = (h - new_h) // 2
            img = img.crop((0, top, w, top + new_h))

        # Resize to 600x800 for e-ink
        img = img.resize((600, 800), Image.LANCZOS)
        buf = io.BytesIO()
        img.save(buf, format='JPEG', quality=75)
        return buf.getvalue()
    except Exception:
        return image_bytes


def fetch_asset(asset_id: str) -> bytes:
    url = f"{KARAKEEP_URL}/api/v1/assets/{asset_id}"
    r = requests.get(url, headers=KARAKEEP_HEADERS, timeout=60)
    r.raise_for_status()
    return r.content


def _format_element(elem, soup) -> list[str]:
    """Recursively format a single element into plain text lines."""
    tag = elem.name
    if tag is None:
        text = str(elem).strip()
        return [text] if text else []

    if tag in ("script", "style", "nav", "aside", "footer"):
        return []

    # Get all child outputs first (pre-order)
    child_lines = []
    for child in elem.children:
        child_lines.extend(_format_element(child, soup))

    text = " ".join(child_lines).strip()
    if not text:
        return []

    if tag in ("h1", "h2", "h3"):
        return ["", text.upper(), "=" * min(len(text), 60), ""]
    elif tag in ("h4", "h5", "h6"):
        return ["", text, "-" * min(len(text), 60), ""]
    elif tag == "blockquote":
        quoted = [f"> {line}" for line in text.split("\n")]
        return [""] + quoted + [""]
    elif tag == "pre":
        indented = [f"    {line}" for line in text.split("\n")]
        return [""] + indented + [""]
    elif tag == "li":
        return [f"  • {text}"]
    elif tag in ("p", "div", "section", "article", "header"):
        return [text, ""]
    else:
        return child_lines


def html_to_plaintext(html_bytes: bytes) -> str:
    """Convert readability-style HTML to plain text suitable for TxtReaderActivity."""
    soup = BeautifulSoup(html_bytes, "lxml")
    body = soup.find("body") or soup.find("main") or soup

    lines = []
    for child in body.children:
        lines.extend(_format_element(child, soup))

    # Deduplicate blank lines but keep paragraph separation
    result = []
    prev_blank = False
    for line in lines:
        blank = (line.strip() == "")
        if blank and prev_blank:
            continue
        result.append(line)
        prev_blank = blank

    return normalize_unicode("\n".join(result).strip())


def html_to_epub(html_bytes: bytes, title: str, author: str = "", cover_bytes: Optional[bytes] = None) -> bytes:
    """Convert readability-style HTML into a minimal valid EPUB (ZIP of OPF+NCX+HTML+CSS+cover)."""
    uid = str(uuid.uuid4())
    safe_title = re.sub(r"[^\w\s-]", "_", title).strip() or "untitled"

    # Parse and clean HTML
    soup = BeautifulSoup(html_bytes, "lxml")
    for tag in soup.find_all(["script", "style", "nav", "aside", "footer"]):
        tag.decompose()

    # Extract body or main content
    body = soup.find("body") or soup.find("main") or soup
    if body:
        # Flatten: wrap direct text nodes in <p>
        for child in list(body.children):
            if isinstance(child, NavigableString) and str(child).strip():
                p = soup.new_tag("p")
                p.string = str(child)
                child.replace_with(p)

    # Generate chapter HTML with normalized text for e-ink rendering
    body_html = body.decode_contents() if body else "<p>(No content)</p>"
    body_html = normalize_unicode(body_html)
    chapter_html = f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head>
<meta charset="UTF-8"/>
<title>{html.escape(title)}</title>
<link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
{body_html}
</body>
</html>"""

    # CSS optimized for CrossPoint's ChapterHtmlSlimParser
    css = """/* Minimal CSS for CrossPoint e-ink */
body { font-family: serif; line-height: 1.5; }
h1, h2, h3 { font-weight: bold; margin-top: 1em; margin-bottom: 0.5em; }
p { margin: 0.5em 0; text-indent: 0; }
blockquote { margin: 1em; font-style: italic; }
pre, code { font-family: monospace; }
img { max-width: 100%; }
"""

    # NCX (EPUB2 TOC)
    ncx = f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN" "http://www.daisy.org/z3986/2005/ncx-2005-1.dtd">
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
<head>
<meta name="dtb:uid" content="{uid}"/>
<meta name="dtb:depth" content="1"/>
<meta name="dtb:totalPageCount" content="0"/>
<meta name="dtb:maxPageNumber" content="0"/>
</head>
<docTitle><text>{html.escape(title)}</text></docTitle>
<navMap>
<navPoint id="navpoint-1" playOrder="1">
<navLabel><text>{html.escape(title)}</text></navLabel>
<content src="chapter1.xhtml"/>
</navPoint>
</navMap>
</ncx>"""

    # OPF
    cover_meta = '<meta name="cover" content="cover-image"/>' if cover_bytes else ""
    cover_item = '\n<item id="cover-image" href="cover.jpg" media-type="image/jpeg"/>' if cover_bytes else ""
    guide_ref = '\n<reference type="cover" title="Cover" href="cover.jpg"/>' if cover_bytes else ""

    opf = f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="BookId">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:title>{html.escape(title)}</dc:title>
<dc:creator>{html.escape(author or "Karakeep")}</dc:creator>
<dc:language>en</dc:language>
<dc:identifier id="BookId">urn:uuid:{uid}</dc:identifier>
{cover_meta}
</metadata>
<manifest>
<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
<item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
<item id="css" href="style.css" media-type="text/css"/>{cover_item}
</manifest>
<spine toc="ncx">
<itemref idref="chapter1"/>
</spine>
<guide>
<reference type="toc" title="Table of Contents" href="toc.ncx"/>{guide_ref}
</guide>
</package>"""

    # Assemble ZIP (EPUB)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        # mimetype must be first and uncompressed per EPUB spec
        zf.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        zf.writestr("META-INF/container.xml", '<?xml version="1.0"?>\n<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">\n<rootfiles>\n<rootfile full-path="content.opf" media-type="application/oebps-package+xml"/>\n</rootfiles>\n</container>')
        zf.writestr("content.opf", opf)
        zf.writestr("toc.ncx", ncx)
        zf.writestr("chapter1.xhtml", chapter_html)
        zf.writestr("style.css", css)
        if cover_bytes:
            zf.writestr("cover.jpg", cover_bytes)

    return buf.getvalue()


# ---------------------------------------------------------------------------
# Proxy Routes
# ---------------------------------------------------------------------------

@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok", "karakeep_url": KARAKEEP_URL})


@app.route("/bookmarks", methods=["GET"])
def list_bookmarks():
    tag = request.args.get("tag", "").strip()
    limit = min(int(request.args.get("limit", "50")), 100)

    bookmarks = fetch_all_bookmarks()
    if tag:
        bookmarks = filter_bookmarks_by_tag(bookmarks, tag)

    simplified = [simplify_bookmark(bm) for bm in bookmarks[:limit]]
    return jsonify({
        "bookmarks": simplified,
        "total": len(simplified),
        "tagFilter": tag,
    })


@app.route("/bookmarks/<bookmark_id>/content", methods=["GET"])
def get_content(bookmark_id):
    fmt = request.args.get("format", "txt").lower()
    if fmt not in ("txt", "epub"):
        return jsonify({"error": "Unsupported format. Use txt or epub"}), 400

    bm = karakeep_get(f"/bookmarks/{bookmark_id}")
    simple = simplify_bookmark(bm)
    asset_id = simple.get("contentAssetId")
    if not asset_id:
        return jsonify({"error": "No content asset available for this bookmark"}), 404

    try:
        raw = fetch_asset(asset_id)
    except requests.HTTPError as e:
        return jsonify({"error": f"Failed to fetch asset: {e}"}), 502

    # Fetch cover if available, crop to portrait ratio for e-ink
    cover_bytes = None
    if simple.get("coverAssetId"):
        try:
            cover_bytes = crop_to_portrait(fetch_asset(simple["coverAssetId"]))
        except Exception:
            pass  # cover is optional

    author = bm.get("content", {}).get("author", "")

    if fmt == "epub":
        data = html_to_epub(raw, simple["title"], author, cover_bytes)
        filename = re.sub(r"[^\w\s-]", "_", simple["title"]).strip() + ".epub"
        return Response(data, mimetype="application/epub+zip", headers={
            "Content-Disposition": f'attachment; filename="{filename}"'
        })
    else:
        text = html_to_plaintext(raw)
        filename = re.sub(r"[^\w\s-]", "_", simple["title"]).strip() + ".txt"
        return Response(text, mimetype="text/plain; charset=utf-8", headers={
            "Content-Disposition": f'attachment; filename="{filename}"'
        })


@app.route("/bookmarks/<bookmark_id>/read", methods=["POST"])
def mark_read(bookmark_id):
    """Mark a bookmark as read by adding the configured read-tag."""
    bm = karakeep_get(f"/bookmarks/{bookmark_id}")
    current_tags = [t["name"] for t in bm.get("tags", [])]
    if READ_TAG in current_tags:
        return jsonify({"status": "already_read"})

    karakeep_post(f"/bookmarks/{bookmark_id}/tags", {"tags": [{"tagName": READ_TAG}]})
    return jsonify({"status": "marked_read", "tag": READ_TAG})


@app.route("/bookmarks/<bookmark_id>/unread", methods=["POST"])
def mark_unread(bookmark_id):
    """Remove the read-tag from a bookmark."""
    bm = karakeep_get(f"/bookmarks/{bookmark_id}")
    current_tags = [t["name"] for t in bm.get("tags", [])]
    if READ_TAG not in current_tags:
        return jsonify({"status": "already_unread"})

    # DELETE /bookmarks/{id}/tags expects body {"tags": [{"tagName": "..."}]}
    try:
        karakeep_delete(f"/bookmarks/{bookmark_id}/tags", {"tags": [{"tagName": READ_TAG}]})
    except requests.HTTPError:
        pass
    return jsonify({"status": "marked_unread", "tag": READ_TAG})


@app.route("/bookmarks/<bookmark_id>/tags", methods=["POST"])
def update_tags(bookmark_id):
    """Add or remove tags on a bookmark.
    Body: {"add": ["tag1", "tag2"], "remove": ["tag3"]}
    """
    body = request.get_json(force=True) or {}
    added = []
    removed = []

    if body.get("add"):
        for tag_name in body["add"]:
            try:
                karakeep_post(f"/bookmarks/{bookmark_id}/tags", {"tags": [{"tagName": tag_name}]})
                added.append(tag_name)
            except requests.HTTPError as e:
                pass  # tag might already exist

    if body.get("remove"):
        for tag_name in body["remove"]:
            try:
                karakeep_delete(f"/bookmarks/{bookmark_id}/tags", {"tags": [{"tagName": tag_name}]})
                removed.append(tag_name)
            except requests.HTTPError:
                pass

    return jsonify({"added": added, "removed": removed})


@app.route("/tags", methods=["GET"])
def list_tags():
    """Return all tags from Karakeep."""
    data = karakeep_get("/tags")
    tags = [t["name"] for t in data.get("tags", [])]
    return jsonify({"tags": tags})


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    print(f"Karakeep Proxy for CrossPoint Reader")
    print(f"  Listening on :{PROXY_PORT}")
    print(f"  Upstream: {KARAKEEP_URL}")
    print(f"  Read tag: '{READ_TAG}'")
    print(f"  Endpoints:")
    print(f"    GET  /health")
    print(f"    GET  /bookmarks?tag=...&limit=...")
    print(f"    GET  /bookmarks/<id>/content?format=txt|epub")
    print(f"    POST /bookmarks/<id>/read")
    print(f"    POST /bookmarks/<id>/unread")
    print(f"    POST /bookmarks/<id>/tags  {{add:[], remove:[]}}")
    print(f"    GET  /tags")
    app.run(host="0.0.0.0", port=PROXY_PORT, threaded=True)
