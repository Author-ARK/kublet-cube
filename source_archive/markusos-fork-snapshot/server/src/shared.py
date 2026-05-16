"""Shared utilities for all server apps."""

import time
from datetime import datetime
from pathlib import Path

SRC_DIR = Path(__file__).parent

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------


def log(msg: str):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}")


# ---------------------------------------------------------------------------
# Cache helper
# ---------------------------------------------------------------------------

_caches: dict[str, dict] = {}


def cached(key: str, ttl: int, fetch_fn):
    """Return cached data or call fetch_fn to refresh."""
    now = time.time()
    entry = _caches.get(key)
    if entry and (now - entry["ts"]) < ttl:
        return entry["data"]
    data = fetch_fn()
    _caches[key] = {"data": data, "ts": now}
    return data
