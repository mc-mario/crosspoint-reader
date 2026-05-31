#!/usr/bin/env python3
"""
Karakeep API probe script.
Mimics what the CrossPoint firmware would do:
- Paginate bookmarks via GET /api/v1/bookmarks
- Filter by tag containing 'Sync'
- Download relevant assets (linkHtmlContent -> txt, bookmarkAsset -> raw file)
- Verify file formats and report sizes
"""

import requests
import json
import os
import sys
import re
from html.parser import HTMLParser

API_URL = os.environ.get("KARAKEEP_URL", "http://localhost:8383")
API_KEY = os.environ.get("KARAKEEP_API_KEY", "")
if not API_KEY:
    print("Error: Set KARAKEEP_API_KEY environment variable")
    sys.exit(1)
HEADERS = {"Authorization": f"Bearer {API_KEY}"}
OUT_DIR = "/tmp/opencode/karakeep_test"
os.makedirs(OUT_DIR, exist_ok=True)


class HTMLToText(HTMLParser):
    """Minimal HTML-to-text converter (similar to what firmware would need)."""
    def __init__(self):
        super().__init__()
        self.text_parts = []
        self.skip_depth = 0
        self.block_tags = {"p", "div", "section", "article", "header", "footer",
                           "h1", "h2", "h3", "h4", "h5", "h6", "li", "blockquote", "pre"}
        self.inline_skip = {"script", "style", "nav", "aside"}

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        if tag in self.inline_skip:
            self.skip_depth += 1
        if tag in self.block_tags and self.text_parts and not self.text_parts[-1].endswith("\n"):
            self.text_parts.append("\n")
        if tag == "br":
            self.text_parts.append("\n")
        if tag == "img":
            alt = dict(attrs).get("alt", "")
            if alt:
                self.text_parts.append(f"[Image: {alt}]")

    def handle_endtag(self, tag):
        tag = tag.lower()
        if tag in self.inline_skip:
            self.skip_depth -= 1
        if tag in self.block_tags and self.text_parts and not self.text_parts[-1].endswith("\n"):
            self.text_parts.append("\n")

    def handle_data(self, data):
        if self.skip_depth == 0:
            self.text_parts.append(data)

    def get_text(self):
        raw = "".join(self.text_parts)
        # Collapse multiple newlines
        raw = re.sub(r"\n{3,}", "\n\n", raw)
        return raw.strip()


def fetch_bookmarks(limit=50):
    all_bookmarks = []
    cursor = None
    page = 0
    while True:
        params = {"archived": "false", "limit": limit}
        if cursor:
            params["cursor"] = cursor
        url = f"{API_URL}/api/v1/bookmarks"
        print(f"[Fetch] Page {page} -> {url} params={params}")
        r = requests.get(url, headers=HEADERS, params=params, timeout=30)
        r.raise_for_status()
        data = r.json()
        bookmarks = data.get("bookmarks", [])
        if not bookmarks:
            break
        all_bookmarks.extend(bookmarks)
        if not data.get("hasMore"):
            break
        cursor = data.get("nextCursor")
        page += 1
        if page > 20:
            print("[!] Safety break after 20 pages")
            break
    return all_bookmarks


def filter_by_tag(bookmarks, tag_substr="Sync"):
    filtered = []
    for b in bookmarks:
        tags = b.get("tags", [])
        tag_names = [t.get("name", "") for t in tags]
        if any(tag_substr.lower() in name.lower() for name in tag_names):
            filtered.append(b)
    return filtered


def sanitize_filename(name, max_len=80):
    if not name:
        name = "unnamed"
    s = re.sub(r'[^\w\s\-_.]', '_', str(name))
    s = s.strip().strip('.')
    if len(s) > max_len:
        s = s[:max_len]
    return s or "unnamed"


def download_asset(asset_id):
    url = f"{API_URL}/api/v1/assets/{asset_id}"
    r = requests.get(url, headers=HEADERS, timeout=60)
    r.raise_for_status()
    return r.content


def detect_mime(data):
    if data.startswith(b"PK"):
        return "application/zip"
    if data.startswith(b"%PDF"):
        return "application/pdf"
    if data.startswith(b"\x89PNG"):
        return "image/png"
    if data[:3] == b"\xff\xd8\xff":
        return "image/jpeg"
    try:
        data.decode("utf-8")
        return "text/plain"
    except UnicodeDecodeError:
        return "application/octet-stream"


def html_to_text(html_bytes):
    try:
        html = html_bytes.decode("utf-8")
    except UnicodeDecodeError:
        html = html_bytes.decode("utf-8", errors="ignore")
    parser = HTMLToText()
    parser.feed(html)
    return parser.get_text()


def process_bookmark(b):
    bid = b.get("id")
    content = b.get("content", {})
    btype = content.get("type", "unknown")
    # Title: prefer root title, fallback to content.title
    title = b.get("title") or content.get("title") or "Untitled"
    url = content.get("url", "")
    assets = b.get("assets", [])
    tags = [t.get("name", "") for t in b.get("tags", [])]

    print(f"\n[ID {bid}] {title}")
    print(f"  Type: {btype}")
    print(f"  Tags: {tags}")

    results = []

    if btype == "link":
        # Look for linkHtmlContent asset
        html_asset = None
        for a in assets:
            if a.get("assetType") == "linkHtmlContent":
                html_asset = a
                break
        if html_asset:
            asset_id = html_asset["id"]
            print(f"  -> Downloading linkHtmlContent asset {asset_id}")
            data = download_asset(asset_id)
            mime = detect_mime(data)
            print(f"     Detected MIME: {mime}, size: {len(data)} bytes")
            # Convert HTML to plain text for e-reader compatibility
            text = html_to_text(data)
            safe = sanitize_filename(title)
            path = os.path.join(OUT_DIR, f"{safe}.txt")
            counter = 1
            orig = path
            while os.path.exists(path):
                path = f"{os.path.splitext(orig)[0]}_{counter}.txt"
                counter += 1
            with open(path, "w", encoding="utf-8") as f:
                f.write(f"# {title}\n\nSource: {url}\n\n{text}")
            print(f"     [Saved TXT] {path} ({os.path.getsize(path)} bytes)")
            results.append((path, "text"))
        else:
            print(f"  -> No linkHtmlContent; saving placeholder")
            safe = sanitize_filename(title)
            path = os.path.join(OUT_DIR, f"{safe}_placeholder.md")
            with open(path, "w", encoding="utf-8") as f:
                f.write(f"# {title}\n\nURL: {url}\n\n(No crawled content available)\n")
            print(f"     [Saved placeholder] {path}")
            results.append((path, "placeholder"))

    elif btype == "asset":
        # Find the actual file asset (bookmarkAsset)
        file_asset = None
        for a in assets:
            if a.get("assetType") == "bookmarkAsset":
                file_asset = a
                break
        if file_asset:
            asset_id = file_asset["id"]
            filename = file_asset.get("fileName") or f"asset_{asset_id}"
            print(f"  -> Downloading bookmarkAsset {asset_id} ({filename})")
            data = download_asset(asset_id)
            mime = detect_mime(data)
            print(f"     Detected MIME: {mime}, size: {len(data)} bytes")
            ext = os.path.splitext(filename)[1].lower()
            if not ext:
                if mime == "application/zip":
                    ext = ".epub"
                elif mime == "text/plain":
                    ext = ".txt"
                elif mime == "application/pdf":
                    ext = ".pdf"
                else:
                    ext = ".bin"
                filename += ext
            safe = sanitize_filename(os.path.splitext(filename)[0]) + os.path.splitext(filename)[1]
            path = os.path.join(OUT_DIR, safe)
            counter = 1
            orig = path
            while os.path.exists(path):
                base, e = os.path.splitext(orig)
                path = f"{base}_{counter}{e}"
                counter += 1
            with open(path, "wb") as f:
                f.write(data)
            print(f"     [Saved RAW] {path} ({os.path.getsize(path)} bytes)")
            results.append((path, mime))
        else:
            print(f"  -> No bookmarkAsset found")

    else:
        print(f"  -> Unhandled type: {btype}")

    return results


def main():
    print("=" * 60)
    print("Karakeep API Probe — Tag 'Sync' filter")
    print(f"Target: {API_URL}")
    print(f"Output: {OUT_DIR}")
    print("=" * 60)

    print("\n[1] Fetching all bookmarks...")
    all_bms = fetch_bookmarks(limit=50)
    print(f"    Total bookmarks fetched: {len(all_bms)}")

    print("\n[2] Filtering by tag 'Sync'...")
    synced = filter_by_tag(all_bms, "Sync")
    print(f"    Matching bookmarks: {len(synced)}")

    if not synced:
        print("\n[!] No bookmarks with tag 'Sync' found.")
        sys.exit(0)

    print("\n[3] Processing & downloading...")
    all_results = []
    for b in synced:
        all_results.extend(process_bookmark(b))

    print("\n[4] Summary of downloaded files:")
    files = sorted(os.listdir(OUT_DIR))
    total_bytes = 0
    for f in files:
        fp = os.path.join(OUT_DIR, f)
        sz = os.path.getsize(fp)
        total_bytes += sz
        print(f"  {f:50s} {sz:>10d} bytes")
    print(f"  {'TOTAL':>50s} {total_bytes:>10d} bytes")
    print("\n[Done]")


if __name__ == "__main__":
    main()
