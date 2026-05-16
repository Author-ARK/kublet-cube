"""Music now-playing endpoints (track info + artwork)."""

import hashlib
import json
import subprocess
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

from shared import log

_ARTWORK_CACHE_DIR = Path(tempfile.gettempdir()) / "kublet-artwork"
_artwork_cache: dict[str, str] = {}  # {"key": track_key, "path": jpeg_path}

_TRACK_INFO_SCRIPT = """\
tell application "Music"
    if player state is not stopped then
        set t to current track
        set n to name of t
        set a to artist of t
        set al to album of t
        set d to duration of t
        set pp to player position
        set ps to player state as string
        return n & "\\n" & a & "\\n" & al & "\\n" & d & "\\n" & pp & "\\n" & ps
    end if
end tell
"""


def _run_osascript(script: str, timeout: int = 5) -> str | None:
    """Run an AppleScript and return stdout, or None on failure."""
    try:
        result = subprocess.run(
            ["osascript", "-e", script],
            capture_output=True, text=True, timeout=timeout,
        )
        if result.returncode == 0 and result.stdout.strip():
            return result.stdout.strip()
    except subprocess.TimeoutExpired:
        log("osascript timed out")
    return None


def _fetch_track_info() -> dict | None:
    """Query Music.app for current track info."""
    raw = _run_osascript(_TRACK_INFO_SCRIPT)
    if not raw:
        return None
    lines = raw.split("\n")
    if len(lines) < 6:
        return None
    return {
        "title": lines[0],
        "artist": lines[1],
        "album": lines[2],
        "duration": round(float(lines[3])),
        "elapsed": round(float(lines[4])),
        "playing": lines[5] == "playing",
    }


def _extract_artwork(track_key: str) -> str | None:
    """Extract artwork for the current track and resize to 240x240 JPEG.

    Tries AppleScript first (works for library tracks), then falls back
    to the iTunes Search API (works for any Apple Music track).

    Results are cached on disk by track key hash.
    """
    cached_art = _artwork_cache.get("key")
    if cached_art == track_key:
        path = _artwork_cache.get("path", "")
        if path and Path(path).exists():
            return path

    slug = hashlib.md5(track_key.encode()).hexdigest()[:12]

    _ARTWORK_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    raw_path = _ARTWORK_CACHE_DIR / f"{slug}_raw.dat"
    jpeg_path = _ARTWORK_CACHE_DIR / f"{slug}.jpg"

    # Return existing file if already extracted for this track
    if jpeg_path.exists() and jpeg_path.stat().st_size > 0:
        _artwork_cache["key"] = track_key
        _artwork_cache["path"] = str(jpeg_path)
        return str(jpeg_path)

    got_raw = False

    # Method 1: AppleScript (works for library tracks with embedded artwork)
    script = f"""\
tell application "Music"
    if player state is not stopped then
        set artData to raw data of artwork 1 of current track
        set fRef to open for access POSIX file "{raw_path}" with write permission
        set eof fRef to 0
        write artData to fRef
        close access fRef
    end if
end tell
"""
    _run_osascript(script, timeout=10)
    if raw_path.exists() and raw_path.stat().st_size > 0:
        got_raw = True

    # Method 2: iTunes Search API fallback (works for streaming tracks)
    if not got_raw:
        title, artist = track_key.split("|", 1)
        got_raw = _fetch_artwork_from_itunes(title, artist, raw_path)

    if not got_raw:
        return None

    # Resize to 240x240 JPEG
    try:
        subprocess.run(
            ["sips", "-z", "240", "240", "-s", "format", "jpeg",
             str(raw_path), "--out", str(jpeg_path)],
            capture_output=True, timeout=10,
        )
    except subprocess.TimeoutExpired:
        log("sips resize timed out")
        return None

    raw_path.unlink(missing_ok=True)

    if not jpeg_path.exists() or jpeg_path.stat().st_size == 0:
        return None

    _artwork_cache["key"] = track_key
    _artwork_cache["path"] = str(jpeg_path)
    log(f"artwork: cached as {jpeg_path.name}")
    return str(jpeg_path)


def _fetch_artwork_from_itunes(title: str, artist: str, output_path: Path) -> bool:
    """Fetch album artwork from the iTunes Search API."""
    try:
        query = urllib.parse.quote(f"{title} {artist}")
        url = f"https://itunes.apple.com/search?term={query}&media=music&limit=1"
        resp = urllib.request.urlopen(url, timeout=5)
        data = json.loads(resp.read())
        if data.get("resultCount", 0) == 0:
            return False
        art_url = data["results"][0].get("artworkUrl100", "")
        if not art_url:
            return False
        # Request 600x600 for quality (sips will resize to 240x240)
        art_url = art_url.replace("100x100", "600x600")
        urllib.request.urlretrieve(art_url, str(output_path))
        log("artwork: fetched from iTunes Search API")
        return output_path.exists() and output_path.stat().st_size > 0
    except Exception as e:
        log(f"artwork: iTunes API error: {e}")
        return False


def get_music_data(log, cached, **_kwargs) -> dict | None:
    """Return current track info (cached 2s), or None if nothing is playing."""
    return cached("music_track", 2, _fetch_track_info)


def get_music_artwork(log, cached, **_kwargs) -> dict | str | None:
    """Return artwork JPEG path for the current track, or None."""
    info = cached("music_track", 2, _fetch_track_info)
    if not info:
        return None
    track_key = f"{info['title']}|{info['artist']}"
    path = _extract_artwork(track_key)
    if not path:
        return None
    return {"_binary": path, "_content_type": "image/jpeg"}
