"""Emulator tests — build smoke tests, screenshot regression, and asset validation.

Uses a session-scoped build cache (conftest.py) so each app is compiled
only once across all test classes.
"""

import json
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
APPS_DIR = REPO_ROOT / "apps"
EMU_DIR = REPO_ROOT / "tools" / "src" / "emulate"

# Apps with known build issues (pre-existing, not regressions)
KNOWN_BROKEN = {"icinga"}


def _discover_apps():
    """Find all apps that have a main.cpp."""
    return sorted(
        d.name for d in APPS_DIR.iterdir()
        if (d / "src" / "main.cpp").exists()
    )


ALL_APPS = _discover_apps()
BUILDABLE_APPS = [a for a in ALL_APPS if a not in KNOWN_BROKEN]


def _run_emulator(build_dir: Path, extra_args: list[str] = None,
                  timeout: int = 15) -> subprocess.CompletedProcess:
    """Run the built emulator binary with given args."""
    binary = build_dir / "emulate"
    cmd = [str(binary)] + (extra_args or [])
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def _compare_screenshots(ref_path: Path, new_path: Path, diff_path: Path) -> float:
    """Compare two PNG screenshots pixel-by-pixel.

    Returns the fraction of pixels that differ (0.0–1.0).
    If they differ, writes a diff image to diff_path: unchanged pixels
    are dimmed, changed pixels are highlighted in magenta.
    """
    from PIL import Image

    ref = Image.open(ref_path).convert("RGB")
    new = Image.open(new_path).convert("RGB")

    if ref.size != new.size:
        return 1.0

    ref_px = ref.load()
    new_px = new.load()
    w, h = ref.size

    diff_count = 0
    diff_img = Image.new("RGB", (w, h))
    diff_px = diff_img.load()

    for y in range(h):
        for x in range(w):
            if ref_px[x, y] != new_px[x, y]:
                diff_count += 1
                diff_px[x, y] = (255, 0, 255)  # magenta
            else:
                # Dim the unchanged pixels
                r, g, b = ref_px[x, y]
                diff_px[x, y] = (r // 4, g // 4, b // 4)

    frac = diff_count / (w * h)
    if frac > 0:
        diff_img.save(diff_path)

    return frac


# ---------------------------------------------------------------------------
# Build smoke tests — every app must compile
# ---------------------------------------------------------------------------

class TestBuildSmoke:
    """Verify all apps compile against emulator mocks."""

    @pytest.mark.parametrize("app_name", BUILDABLE_APPS)
    def test_app_compiles(self, app_name, emu_builds):
        """App should compile without errors."""
        build_dir, success = emu_builds.get(app_name)

        if not success:
            err_file = build_dir / "_build_error.txt"
            err_msg = err_file.read_text() if err_file.exists() else "Unknown error"
            pytest.fail(f"Build failed for '{app_name}':\n{err_msg}")

        assert (build_dir / "emulate").exists()

    @pytest.mark.parametrize("app_name", sorted(KNOWN_BROKEN))
    def test_known_broken_app_is_documented(self, app_name):
        """Known broken apps should still have main.cpp (not silently removed)."""
        assert (APPS_DIR / app_name / "src" / "main.cpp").exists(), (
            f"Known-broken app '{app_name}' no longer exists — remove from KNOWN_BROKEN"
        )


# ---------------------------------------------------------------------------
# Screenshot tests — apps render expected output
# ---------------------------------------------------------------------------

class TestScreenshot:
    """Verify apps produce expected visual output.

    Apps with a reference screenshot get a regression check (which also
    proves non-blankness). All other apps get a simple non-blank check.
    """

    # Apps that need HTTP fixtures to render — skip if no fixtures
    _NEEDS_DATA = {"bored"}

    @pytest.mark.parametrize("app_name", BUILDABLE_APPS)
    def test_app_screenshot(self, app_name, emu_builds, tmp_path):
        """App should produce a screenshot that is non-blank and, if a
        reference exists, matches it."""
        if app_name in self._NEEDS_DATA:
            fixtures = APPS_DIR / app_name / "assets" / "http_fixtures.json"
            if not fixtures.exists():
                pytest.skip(f"{app_name} needs HTTP fixtures to render")

        build_dir, success = emu_builds.get(app_name)
        if not success:
            pytest.skip(f"Build failed for {app_name}")

        screenshot = tmp_path / f"{app_name}.png"
        result = _run_emulator(build_dir, [
            "--screenshot", str(screenshot),
            "--after", "3",
        ], timeout=15)

        assert screenshot.exists(), (
            f"Screenshot not produced for '{app_name}':\n"
            f"STDOUT: {result.stdout[-500:]}\n"
            f"STDERR: {result.stderr[-500:]}"
        )
        size = screenshot.stat().st_size
        assert size > 500, (
            f"Screenshot for '{app_name}' looks blank ({size} bytes)"
        )

        # If a reference screenshot exists, compare against it
        ref_path = APPS_DIR / app_name / "assets" / "reference.png"
        if ref_path.exists():
            diff_path = tmp_path / f"{app_name}_diff.png"
            frac = _compare_screenshots(ref_path, screenshot, diff_path)
            assert frac < 0.05, (
                f"Screenshot for '{app_name}' differs from reference by {frac:.1%}.\n"
                f"  diff image: {diff_path}\n"
                f"  new image:  {screenshot}\n"
                f"  reference:  {ref_path}\n"
                f"If this is intentional, update the reference:\n"
                f"  ./tools/emulate {app_name} --generate-reference"
            )


# ---------------------------------------------------------------------------
# HTTP fixture tests
# ---------------------------------------------------------------------------

def _find_http_apps():
    """Find apps that use HTTPClient for data fetching."""
    no_fixture_needed = {"notice"}
    known_missing = {"bored"}
    apps = []
    for name in BUILDABLE_APPS:
        if name in no_fixture_needed or name in known_missing:
            continue
        src = (APPS_DIR / name / "src" / "main.cpp").read_text()
        if "#include" in src and "HTTPClient" in src:
            apps.append(name)
    return apps


HTTP_APPS = _find_http_apps()


class TestHTTPFixtures:
    """Verify apps with HTTP dependencies have fixture files."""

    @pytest.mark.parametrize("app_name", HTTP_APPS)
    def test_http_app_has_fixtures(self, app_name):
        """Apps using HTTPClient should have http_fixtures.json."""
        fixture_path = APPS_DIR / app_name / "assets" / "http_fixtures.json"
        assert fixture_path.exists(), (
            f"App '{app_name}' uses HTTPClient but has no assets/http_fixtures.json"
        )

        data = json.loads(fixture_path.read_text())
        assert isinstance(data, list), "http_fixtures.json should be a JSON array"
        assert len(data) > 0, "http_fixtures.json should have at least one fixture"


# ---------------------------------------------------------------------------
# Notification test (notice app specific)
# ---------------------------------------------------------------------------

class TestNoticeApp:
    """Tests specific to the notice app's notification handling."""

    def test_notification_via_notify_at(self, emu_builds, tmp_path):
        """Notice app should accept --notify-at and render the notification."""
        build_dir, success = emu_builds.get("notice")
        if not success:
            pytest.skip("Notice app build failed")

        screenshot = tmp_path / "notice_notif.png"
        result = _run_emulator(build_dir, [
            "--notify-at", "1:imessage:Alice:Hello from test",
            "--screenshot", str(screenshot),
            "--after", "2.5",
        ], timeout=10)

        assert screenshot.exists(), "Screenshot not produced"
        assert screenshot.stat().st_size > 500, "Screenshot looks blank"

    def test_notifications_json_loads(self):
        """notifications.json should be valid JSON with required fields."""
        path = APPS_DIR / "notice" / "assets" / "notifications.json"
        if not path.exists():
            pytest.skip("No notifications.json")

        data = json.loads(path.read_text())
        assert isinstance(data, list)
        for entry in data:
            assert "time" in entry, f"Missing 'time' in {entry}"
            assert "source" in entry, f"Missing 'source' in {entry}"
            assert "sender" in entry, f"Missing 'sender' in {entry}"
            assert "text" in entry, f"Missing 'text' in {entry}"
            assert isinstance(entry["time"], (int, float))


# ---------------------------------------------------------------------------
# Asset validation
# ---------------------------------------------------------------------------

def _find_apps_with_asset(filename: str):
    """Find apps that have a specific asset file."""
    return [a for a in BUILDABLE_APPS if (APPS_DIR / a / "assets" / filename).exists()]


APPS_WITH_PREFERENCES = _find_apps_with_asset("preferences.json")
APPS_WITH_FIXTURES = _find_apps_with_asset("http_fixtures.json")


class TestAssets:
    """Validate app asset files are well-formed."""

    @pytest.mark.parametrize("app_name", APPS_WITH_PREFERENCES)
    def test_preferences_json_valid(self, app_name):
        """preferences.json should be valid JSON object."""
        path = APPS_DIR / app_name / "assets" / "preferences.json"
        data = json.loads(path.read_text())
        assert isinstance(data, dict), "preferences.json should be a JSON object"

    @pytest.mark.parametrize("app_name", APPS_WITH_FIXTURES)
    def test_http_fixtures_json_valid(self, app_name):
        """http_fixtures.json should have required fields."""
        path = APPS_DIR / app_name / "assets" / "http_fixtures.json"
        data = json.loads(path.read_text())
        assert isinstance(data, list)
        for i, fixture in enumerate(data):
            assert "url" in fixture, f"Fixture {i} missing 'url'"
            assert "file" in fixture or "status" in fixture, (
                f"Fixture {i} must have 'file' or 'status'"
            )
