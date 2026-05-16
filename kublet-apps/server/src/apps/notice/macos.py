"""macOS Notification Center source — real-time notification capture.

Uses `log stream` to detect new notifications instantly, then reads the
notification database for the full content. This gives near-instant
detection (~1s) compared to pure DB polling (~8-10s delay).

Captures notifications from ALL apps (Slack, Discord, iMessage, email, etc.)
Only requires Full Disk Access granted to the terminal running the server.
"""

import os
import plistlib
import sqlite3
import subprocess
import threading
import time

_DB_PATH = os.path.expanduser(
    "~/Library/Group Containers/group.com.apple.usernoted/db2/db"
)
_DB_POLL_AFTER_TRIGGER = 0.3  # seconds — rapid poll interval after log trigger
_DB_POLL_ATTEMPTS = 30        # try for ~9s after trigger before giving up
_FALLBACK_POLL = 10           # seconds — fallback if log stream not working

# Map bundle IDs to friendly source names
_APP_NAMES: dict[str, tuple[str, str]] = {
    # (source_type, display_name)
    "com.apple.MobileSMS": ("imessage", "iMessage"),
    "com.apple.mobilesms": ("imessage", "iMessage"),
    "com.apple.ichat": ("imessage", "iMessage"),
    "com.hnc.discord": ("discord", "Discord"),
    "com.tinyspeck.slackmacgap": ("slack", "Slack"),
    "com.apple.mail": ("email", "Mail"),
    "com.google.chrome": ("chrome", "Chrome"),
    "com.google.chrome.framework.alertnotificationservice": ("chrome", "Chrome"),
    "com.apple.ical": ("calendar", "Calendar"),
    "com.apple.reminders": ("reminder", "Reminders"),
    "com.apple.facetime": ("facetime", "FaceTime"),
    "com.apple.passbook": ("wallet", "Wallet"),
    "com.apple.Passbook": ("wallet", "Wallet"),
    "com.apple.news": ("news", "News"),
    "com.apple.findmy": ("findmy", "Find My"),
    "com.microsoft.teams": ("teams", "Teams"),
    "com.microsoft.outlook": ("email", "Outlook"),
    "com.apple.notes": ("notes", "Notes"),
}

# Apps to ignore (system noise)
_IGNORE_APPS: set[str] = {
    "com.apple.controlcenter.notifications.low-battery",
    "com.apple.controlcenter.notifications.focus",
    "com.apple.screentimenotifications",
    "com.apple.screentimeenablednotifications",
    "com.apple.siri.actionpredictionnotifications",
    "com.apple.siri",
    "com.apple.tips",
    "com.apple.tccd",
    "com.apple.lockdownmode",
}


def _decode_notification(data_blob: bytes) -> dict | None:
    """Decode a notification's binary plist data blob."""
    try:
        plist = plistlib.loads(data_blob)
    except Exception:
        return None

    req = plist.get("req", {})
    if not req:
        return None

    title = req.get("titl", "")
    body = req.get("body", "")
    subtitle = req.get("subt", "")

    if not title and not body:
        return None

    return {
        "title": title,
        "subtitle": subtitle,
        "body": body,
        "app": plist.get("app", ""),
    }


def _resolve_app(bundle_id: str) -> tuple[str, str]:
    """Map a bundle ID to (source_type, display_name)."""
    if bundle_id in _APP_NAMES:
        return _APP_NAMES[bundle_id]

    # Handle _system_center_: prefix
    if bundle_id.startswith("_system_center_:"):
        inner = bundle_id[len("_system_center_:"):]
        if inner in _APP_NAMES:
            return _APP_NAMES[inner]
        return ("system", inner.split(".")[-1].title())

    # Fallback: use last component of bundle ID
    parts = bundle_id.split(".")
    name = parts[-1] if parts else bundle_id
    return ("app", name.title())


class _LogStreamTrigger:
    """Watches `log stream` for notification events and signals the DB poller."""

    def __init__(self, log):
        self._log = log
        self._event = threading.Event()
        self._running = False
        self._process = None

    def start(self):
        """Start the log stream watcher in a background thread."""
        self._running = True
        threading.Thread(target=self._run, daemon=True).start()

    def wait(self, timeout: float) -> bool:
        """Wait for a notification trigger. Returns True if triggered, False on timeout."""
        triggered = self._event.wait(timeout=timeout)
        self._event.clear()
        return triggered

    def _run(self):
        """Background thread: run `log stream` and watch for notification events."""
        while self._running:
            try:
                self._process = subprocess.Popen(
                    [
                        "log", "stream",
                        "--predicate",
                        'process == "usernoted" AND eventMessage CONTAINS "successfully processed by pipeline"',
                        "--style", "compact",
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.DEVNULL,
                    text=True,
                )
                self._log("Notice/macOS: log stream watcher started")

                for line in self._process.stdout:
                    if "successfully processed" in line:
                        self._event.set()

                # Process ended unexpectedly — restart after a brief delay
                self._process.wait()
                self._log("Notice/macOS: log stream ended, restarting...")
                time.sleep(2)

            except Exception as e:
                self._log(f"Notice/macOS: log stream error: {e}")
                time.sleep(5)


def start(push_callback, log):
    """Start polling for macOS notifications. Blocks forever (run in a thread).

    Args:
        push_callback: function(source, sender, text, timestamp)
        log: logging function
    """
    if not os.path.exists(_DB_PATH):
        log("Notice/macOS: notification database not found, source disabled")
        log("Notice/macOS: expected at: " + _DB_PATH)
        return

    try:
        conn = sqlite3.connect(f"file:{_DB_PATH}?mode=ro", uri=True, timeout=5)
        conn.row_factory = sqlite3.Row
    except sqlite3.OperationalError as e:
        log(f"Notice/macOS: cannot open notification db: {e}")
        log("Notice/macOS: grant Full Disk Access to your terminal in System Settings")
        return

    # Load app ID -> bundle ID mapping
    cursor = conn.cursor()
    app_map: dict[int, str] = {}
    try:
        cursor.execute("SELECT app_id, identifier FROM app")
        for row in cursor.fetchall():
            app_map[row["app_id"]] = row["identifier"]
    except sqlite3.OperationalError as e:
        log(f"Notice/macOS: error reading app table: {e}")
        return

    log(f"Notice/macOS: found {len(app_map)} registered apps")

    # Track by delivered_date (not rec_id) because macOS purges old records
    # and rec_ids can reset to lower values
    cursor.execute("SELECT MAX(delivered_date) FROM record")
    row = cursor.fetchone()
    last_date = row[0] or 0.0
    log(f"Notice/macOS: starting from delivered_date {last_date:.2f}")

    # Also track seen UUIDs to avoid duplicates during the same poll window
    seen_uuids: set[bytes] = set()
    cursor.execute("SELECT uuid FROM record")
    for row in cursor.fetchall():
        if row["uuid"]:
            seen_uuids.add(row["uuid"])

    # Start log stream watcher for near-instant notification detection
    trigger = _LogStreamTrigger(log)
    trigger.start()

    while True:
        try:
            # Refresh app map periodically (new apps can register)
            cursor.execute("SELECT app_id, identifier FROM app")
            for row in cursor.fetchall():
                app_map[row["app_id"]] = row["identifier"]

            cursor.execute("""
                SELECT rec_id, app_id, uuid, data, delivered_date
                FROM record
                WHERE delivered_date > ?
                ORDER BY delivered_date ASC
                LIMIT 20
            """, (last_date,))

            for row in cursor.fetchall():
                app_id = row["app_id"]
                uuid = row["uuid"]
                data_blob = row["data"]
                delivered_date = row["delivered_date"]

                # Update timestamp tracker
                if delivered_date and delivered_date > last_date:
                    last_date = delivered_date

                # Skip if we've already seen this notification
                if uuid and uuid in seen_uuids:
                    continue
                if uuid:
                    seen_uuids.add(uuid)
                    # Cap seen set size
                    if len(seen_uuids) > 1000:
                        seen_uuids.clear()

                bundle_id = app_map.get(app_id, "unknown")

                # Skip ignored system notifications
                if bundle_id in _IGNORE_APPS or bundle_id.startswith("_system_center_:"):
                    continue

                if not data_blob:
                    continue

                notif = _decode_notification(data_blob)
                if not notif:
                    continue

                source_type, app_name = _resolve_app(bundle_id)

                # Build sender: use title, fall back to app name
                sender = notif["title"] or app_name
                if notif["subtitle"]:
                    sender = f"{sender} · {notif['subtitle']}"

                # Build text: use body
                text = notif["body"] or notif["title"] or ""
                if not text:
                    continue

                # Convert Core Data timestamp (seconds since 2001-01-01) to Unix
                unix_ts = (delivered_date or 0) + 978307200

                log(f"Notice/macOS: [{source_type}] {sender}: {text[:60]}")
                push_callback(source_type, sender, text, unix_ts)

        except sqlite3.OperationalError as e:
            log(f"Notice/macOS: db error (will retry): {e}")

        # Wait for log stream trigger or fallback timeout
        triggered = trigger.wait(timeout=_FALLBACK_POLL)
        if triggered:
            # Log stream detected a notification — rapid-poll the DB until it appears
            for _ in range(_DB_POLL_ATTEMPTS):
                time.sleep(_DB_POLL_AFTER_TRIGGER)
                try:
                    cursor.execute("""
                        SELECT delivered_date FROM record
                        WHERE delivered_date > ?
                        LIMIT 1
                    """, (last_date,))
                    if cursor.fetchone():
                        break  # New data available — main loop will process it
                except sqlite3.OperationalError:
                    pass
