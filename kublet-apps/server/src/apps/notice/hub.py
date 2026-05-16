"""Notification hub — collects from sources and pushes to Kublet devices."""

import json
import threading
import time
import urllib.request

from shared import log
from .emoji import replace_emojis

_lock = threading.Lock()
_devices: dict[str, float] = {}  # ip -> last_seen timestamp


def register():
    """Start notification sources in background threads."""
    try:
        from .macos import start as start_macos
        threading.Thread(target=_safe_run, args=(start_macos, push_notification), daemon=True).start()
        log("Notice: macOS notification source started")
    except Exception as e:
        log(f"Notice: macOS notification source not available: {e}")


def _safe_run(start_fn, callback):
    """Run a source's start function, catching and logging errors."""
    try:
        start_fn(callback, log)
    except Exception as e:
        log(f"Notice: source error: {e}")


def push_notification(source: str, sender: str, text: str, timestamp: float | None = None):
    """Push a notification to all registered devices."""
    ts = timestamp or time.time()
    payload = json.dumps({
        "source": source,
        "sender": replace_emojis(sender),
        "text": replace_emojis(text[:250]),
        "timestamp": int(ts),
    }).encode()

    with _lock:
        devices = dict(_devices)

    log(f"Notice: pushing to {len(devices)} device(s): [{source}] {sender}: {text[:60]}")

    for ip in devices:
        try:
            req = urllib.request.Request(
                f"http://{ip}/notify",
                data=payload,
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            urllib.request.urlopen(req, timeout=3)
        except Exception as e:
            log(f"Notice: push to {ip} failed: {e}")


# ---------------------------------------------------------------------------
# Route handlers
# ---------------------------------------------------------------------------

def get_register(log, cached, query_params):
    """GET /api/notice/register?ip=X — device self-registration."""
    ip_list = query_params.get("ip", [])
    if not ip_list:
        return {"error": "missing ip param"}

    ip = ip_list[0]
    with _lock:
        _devices[ip] = time.time()

    log(f"Notice: device registered: {ip} (total: {len(_devices)})")
    return {"ok": True, "ip": ip}


def get_push(log, cached, query_params):
    """GET /api/notice/push?source=X&sender=X&text=X — manual test push."""
    source = query_params.get("source", ["test"])[0]
    sender = query_params.get("sender", ["Test"])[0]
    text = query_params.get("text", ["Hello!"])[0]

    push_notification(source, sender, text)
    return {"ok": True, "pushed": True}
