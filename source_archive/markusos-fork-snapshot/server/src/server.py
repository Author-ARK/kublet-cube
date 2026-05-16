#!/usr/bin/env python3
"""
Unified Kublet app server.

Serves JSON endpoints for Kublet ESP32 apps. Apps are auto-discovered from
the apps/ directory — each sub-package exports a ROUTES dict and an optional
register() function for background work.

Adding a new app:
  1. Create a directory under server/src/apps/<name>/
  2. Add __init__.py that exports ROUTES (dict) and optionally register (fn)
  3. That's it — routes are picked up automatically on next server start

Environment variables:
  PORT       — Server port (default: 8198)
  CLAUDE_BIN — Path to claude binary (used by fetch_claude_usage.sh)

Usage:
  python server.py
"""

import http.server
import json
import os
from pathlib import Path
from urllib.parse import urlparse, parse_qs

from shared import log, cached
from apps import discover

PORT = int(os.environ.get("PORT", "8198"))

# ---------------------------------------------------------------------------
# Auto-discover apps and register routes
# ---------------------------------------------------------------------------

ROUTES, _register_fns = discover()

for name, fn in _register_fns:
    try:
        fn()
        log(f"Registered: {name}")
    except Exception as e:
        log(f"Failed to register {name}: {e}")


# ---------------------------------------------------------------------------
# HTTP server
# ---------------------------------------------------------------------------


class AppHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        handler = ROUTES.get(parsed.path)

        if handler is None:
            self.send_response(404)
            self.end_headers()
            return

        query_params = parse_qs(parsed.query)
        data = handler(log=log, cached=cached, query_params=query_params)

        if data is None:
            self.send_response(503)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"error": "not available"}).encode())
            return

        # HTML response (e.g. OAuth callback page)
        if isinstance(data, dict) and "_html" in data:
            body = data["_html"].encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        # Binary response (e.g. artwork image)
        if isinstance(data, dict) and "_binary" in data:
            path = data["_binary"]
            content_type = data.get("_content_type", "application/octet-stream")
            try:
                body = Path(path).read_bytes()
            except OSError:
                self.send_response(500)
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())

    def log_message(self, format, *args):
        log(format % args)


def main():
    log(f"Kublet server running on http://0.0.0.0:{PORT}")
    for path in sorted(ROUTES):
        log(f"  {path}")
    server = http.server.HTTPServer(("0.0.0.0", PORT), AppHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down")
        server.server_close()


if __name__ == "__main__":
    main()
