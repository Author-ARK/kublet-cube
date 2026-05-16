"""Shared fixtures for emulator tests."""

import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
APPS_DIR = REPO_ROOT / "apps"
EMU_DIR = REPO_ROOT / "tools" / "src" / "emulate"


def _build_app(app_name: str, build_dir: Path) -> subprocess.CompletedProcess:
    """Configure and build an app in the emulator. Returns the build result."""
    app_dir = APPS_DIR / app_name

    result = subprocess.run(
        [
            "cmake", "-S", str(EMU_DIR), "-B", str(build_dir),
            f"-DAPP_DIR={app_dir}",
            "-DCMAKE_BUILD_TYPE=Debug",
        ],
        capture_output=True, text=True, timeout=30,
    )
    if result.returncode != 0:
        return result

    result = subprocess.run(
        ["cmake", "--build", str(build_dir), "--parallel"],
        capture_output=True, text=True, timeout=120,
    )
    return result


@pytest.fixture(scope="session")
def emu_builds(tmp_path_factory):
    """Session-scoped build cache. Maps app_name -> (build_dir, success)."""
    cache: dict[str, tuple[Path, bool]] = {}

    class BuildCache:
        def get(self, app_name: str) -> tuple[Path, bool]:
            """Build an app (or return cached result). Returns (build_dir, success)."""
            if app_name in cache:
                return cache[app_name]

            build_dir = tmp_path_factory.mktemp(f"emu_{app_name}")
            result = _build_app(app_name, build_dir)
            success = result.returncode == 0 and (build_dir / "emulate").exists()

            if not success:
                # Store error info for diagnostics
                cache[app_name] = (build_dir, False)
                # Stash error output for later retrieval
                err_file = build_dir / "_build_error.txt"
                err_file.write_text(
                    f"STDOUT:\n{result.stdout[-2000:]}\n"
                    f"STDERR:\n{result.stderr[-2000:]}"
                )
            else:
                cache[app_name] = (build_dir, True)

            return cache[app_name]

    return BuildCache()
