# Kublet Server

HTTP server that provides JSON endpoints for Kublet ESP32 apps. Apps are auto-discovered — add a new directory under `src/apps/` and it's picked up automatically.

## Quick Start

```bash
./server/run
```

The server runs on port 8198 by default (override with `PORT` env var).

## Endpoints

| Endpoint | App | Description |
|---|---|---|
| `GET /api/usage` | usage | Claude Code session/weekly usage percentages |
| `GET /api/music` | music | Now-playing track info from Music.app |
| `GET /api/music/artwork` | music | 240x240 JPEG album artwork for current track |
| `GET /api/notice/register` | notice | Device self-registration (`?ip=X`) |
| `GET /api/notice/push` | notice | Manual test push (`?source=X&sender=X&text=X`) |

## Structure

```
server/
├── run                          # uv script entrypoint
└── src/
    ├── server.py                # HTTP server, auto-discovery, routing
    ├── shared.py                # log(), cached() shared utilities
    └── apps/
        ├── __init__.py          # discover() — scans sub-packages for routes
        ├── music/
        │   ├── __init__.py      # ROUTES
        │   └── tracks.py        # Track info, artwork extraction
        ├── usage/
        │   ├── __init__.py      # ROUTES
        │   ├── claude.py        # Claude Code usage fetching
        │   └── fetch_claude_usage.sh
        └── notice/
            ├── __init__.py      # ROUTES + register()
            ├── hub.py           # Push logic, device registry
            ├── macos.py         # macOS Notification Center source
            └── emoji.py         # Emoji → text art replacement
```

## Adding a New App

1. Create a directory under `src/apps/<name>/`.
2. Add an `__init__.py` that exports a `ROUTES` dict:
   ```python
   from .my_handler import get_data

   ROUTES = {
       "/api/myapp": get_data,
   }
   ```
3. Write your handler(s) in a separate module:
   ```python
   from shared import log

   def get_data(log, cached, **_kwargs) -> dict | None:
       return cached("myapp", 60, _fetch_data)
   ```
4. Optionally export a `register()` function for background work (e.g. starting threads):
   ```python
   from .my_handler import get_data, register

   ROUTES = { ... }
   ```
5. That's it — restart the server and your routes are live.

## Handler Signature

All route handlers receive keyword arguments:

```python
def my_handler(log, cached, query_params) -> dict | None:
```

- `log(msg)` — timestamped logging
- `cached(key, ttl, fetch_fn)` — TTL-based cache helper
- `query_params` — parsed query string (dict of lists)

Return values:
- `dict` — serialized as JSON response
- `{"_binary": path, "_content_type": "..."}` — binary file response
- `{"_html": "<html>..."}` — HTML response
- `None` — 503 not available

## Environment Variables

- `PORT` — Server port (default: 8198)
- `CLAUDE_BIN` — Path to claude binary (used by `fetch_claude_usage.sh`)

## Notice App

The notice app reads the macOS Notification Center database to capture notifications from all apps (iMessage, Slack, Discord, Mail, etc.) and pushes them to registered Kublet devices.

Requirements:
- **Full Disk Access** granted to your terminal app (System Settings → Privacy & Security → Full Disk Access)

Emojis in notifications are automatically converted to text art equivalents (e.g. 😀 → `:D`, ❤️ → `<3`, 🎉 → `*party*`).

Test with:
```bash
curl "http://localhost:8198/api/notice/push?source=imessage&sender=Alice&text=Hello+world"
```
