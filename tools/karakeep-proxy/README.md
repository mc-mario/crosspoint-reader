# Karakeep Proxy for CrossPoint Reader

Lightweight Python middleware that sits between a CrossPoint Reader (ESP32-C3 e-ink) and a [Karakeep](https://karakeep.app) instance. It offloads all heavy lifting so the device only needs simple HTTP+JSON.

## What it does

- **Tag filtering & pagination** — Karakeep's API doesn't filter by tag; the proxy paginates and filters server-side.
- **HTML → clean TXT** — Converts readability-style HTML to plain text with headers, blockquotes, and lists preserved.
- **HTML → EPUB** — Generates valid EPUB2 with cover image, TOC (NCX), CSS, and structured chapters. CrossPoint's native EPUB reader renders this with full formatting.
- **Read-state tracking** — Adds/removes a configurable tag (default: `"Read"`) on Karakeep bookmarks.
- **Tag management** — Add or remove tags via a simple JSON API.

## Quick Start

### 1. Fork this repo (for GHCR publishing)

This repo is a fork of `crosspoint-reader/crosspoint-reader`. To publish the proxy image to your own GitHub Container Registry, you need your own fork:

1. Go to https://github.com/crosspoint-reader/crosspoint-reader and click **Fork**.
2. Clone your fork locally:
   ```bash
   git clone https://github.com/TU_USUARIO/crosspoint-reader.git
   cd crosspoint-reader/tools/karakeep-proxy
   ```

### 2. Local development (Python)

```bash
pip install -r requirements.txt

export KARAKEEP_URL="http://your-karakeep:3000"
export KARAKEEP_API_KEY="ak2_..."
python3 main.py
```

The proxy listens on `0.0.0.0:8787` by default (override with `PROXY_PORT`).

### 3. Build Docker image locally (~126 MB Alpine)

```bash
docker build -t karakeep-proxy .
```

### 4. Publish to GitHub Container Registry (GHCR) — automated

A GitHub Actions workflow is included at `.github/workflows/build-proxy-image.yml`. It automatically builds and pushes the image on every push to `main` that touches `tools/karakeep-proxy/`, and on every release.

**Steps:**

1. Push your fork to GitHub:
   ```bash
   git push origin main
   ```
2. Go to your fork on GitHub → **Settings → Actions → General → Workflow permissions**.
3. Select **Read and write permissions** and click Save.
4. Create a release (or push a tag starting with `proxy-v`):
   ```bash
   git tag proxy-v1.0.0
   git push origin proxy-v1.0.0
   ```
5. The workflow will run and publish to:
   ```
   ghcr.io/TU_USUARIO/crosspoint-reader/karakeep-proxy:1.0.0
   ghcr.io/TU_USUARIO/crosspoint-reader/karakeep-proxy:latest
   ```

**Pull the published image:**
```bash
docker pull ghcr.io/TU_USUARIO/crosspoint-reader/karakeep-proxy:latest
```

### 5. Docker Compose

Add to your existing `docker-compose.yaml` (alongside Karakeep):

```yaml
services:
  karakeep-proxy:
    image: ghcr.io/TU_USUARIO/crosspoint-reader/karakeep-proxy:latest
    container_name: karakeep-proxy
    restart: unless-stopped
    ports:
      - "8787:8787"
    environment:
      - KARAKEEP_URL=http://karakeep:3000
      - KARAKEEP_API_KEY=${KARAKEEP_API_KEY}
      - PROXY_PORT=8787
      - READ_TAG=Read
    networks:
      - karakeep-net

  # ... your existing Karakeep services ...

networks:
  karakeep-net:
    driver: bridge
```

Then:
```bash
docker compose up -d karakeep-proxy
```

## Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/health` | Health check |
| GET | `/bookmarks?tag=Sync&limit=20` | Filtered bookmark list |
| GET | `/bookmarks/<id>/content?format=txt` | Download as plain text |
| GET | `/bookmarks/<id>/content?format=epub` | Download as EPUB |
| POST | `/bookmarks/<id>/read` | Mark as read (adds tag) |
| POST | `/bookmarks/<id>/unread` | Mark as unread (removes tag) |
| POST | `/bookmarks/<id>/tags` | `{"add":["..."], "remove":["..."]}` |
| GET | `/tags` | List all tags |

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `KARAKEEP_URL` | `http://localhost:3000` | Upstream Karakeep URL |
| `KARAKEEP_API_KEY` | *(required)* | Bearer token |
| `PROXY_PORT` | `8787` | Proxy listen port |
| `READ_TAG` | `Read` | Tag used for read-state |

## Why a proxy?

CrossPoint runs on an ESP32-C3 with **~380 KB RAM** and no PSRAM. Parsing HTML, generating EPUBs, or filtering large JSON arrays would exhaust the device. This proxy handles all complexity so the firmware only needs:

- Small HTTP client (2 KB TLS buffers)
- Streaming JSON parser (512 B token buffer)
- File write to SD card

## Privacy: Can the GHCR image be private?

**Yes.** GHCR images inherit the visibility of the repository by default, but you can change it:

- **Public repo** → image is public by default.
- **Private repo** → image is private by default.
- You can also toggle image visibility independently in:
  **GitHub → Your profile → Packages → karakeep-proxy → Package settings → Change visibility**

If you make the image **private**, you will need to authenticate Docker when pulling from the Xteink (or any device):
```bash
docker login ghcr.io -u TU_USUARIO -p $GITHUB_TOKEN
docker pull ghcr.io/TU_USUARIO/crosspoint-reader/karakeep-proxy:latest
```

For the ESP32 firmware, private images make no difference — the proxy runs on a server, not on the device. The device only needs to reach the proxy's HTTP endpoint (e.g. `http://192.168.1.100:8787`). The proxy itself pulls from Karakeep using your API key. So the image visibility only matters for **where** you deploy the proxy (your NAS, VPS, etc.).

---

## Architecture

```
┌─────────────────┐      HTTP+JSON      ┌─────────────────┐      HTTP+Bearer    ┌─────────────────┐
│  CrossPoint     │  <──────────────>  │  Karakeep Proxy │  <──────────────>  │    Karakeep     │
│  (ESP32-C3)     │   /bookmarks?tag=   │   (Python)      │   /api/v1/...     │   (Node.js)     │
│  ~380 KB RAM    │   .txt / .epub     │                 │                   │                 │
└─────────────────┘                    └─────────────────┘                   └─────────────────┘
```

## EPUB Generation

The proxy generates minimal valid EPUB2 files using only Python standard library (`zipfile`, `xml.etree`). No external EPUB libraries required. Each EPUB contains:

- `mimetype` — uncompressed per spec
- `META-INF/container.xml` — points to `content.opf`
- `content.opf` — metadata, manifest, spine
- `toc.ncx` — table of contents for CrossPoint's `TocNcxParser`
- `chapter1.xhtml` — cleaned HTML content
- `style.css` — minimal e-ink optimized styles
- `cover.jpg` — cover image (if available)

CrossPoint's `Epub` class reads these natively.
