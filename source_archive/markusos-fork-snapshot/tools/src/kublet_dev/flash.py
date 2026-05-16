"""USB serial flashing, NVS generation, and WiFi credential management."""

import csv
import io
import subprocess
import sys
from pathlib import Path

from kublet_dev.config import (
    BAUD,
    FLASH_FREQ,
    FLASH_MODE,
    NVS_BIN,
    NVS_CSV,
    NVS_PARTITION_SIZE,
    REPO_ROOT,
    load_env,
    save_env,
)


def detect_serial_port() -> str:
    """Auto-detect the Kublet's USB serial port on macOS."""
    dev = Path("/dev")
    for pattern in ["cu.usbserial-*", "cu.SLAB_USBtoUART*", "cu.wchusbserial*"]:
        ports = sorted(dev.glob(pattern))
        if ports:
            return str(ports[0])

    print("Error: No USB serial port detected.")
    print("  Is the Kublet connected via USB?")
    print("  You can specify a port manually with: -p /dev/cu.usbserial-0001")
    sys.exit(1)


def find_nvs_gen() -> str:
    """Locate nvs_partition_gen.py from the PlatformIO ESP-IDF package."""
    pio = Path.home() / ".platformio"
    direct = (
        pio
        / "packages"
        / "framework-espidf"
        / "components"
        / "nvs_flash"
        / "nvs_partition_generator"
        / "nvs_partition_gen.py"
    )
    if direct.exists():
        return str(direct)

    for path in pio.glob("**/nvs_partition_gen.py"):
        return str(path)

    print("Error: Could not find nvs_partition_gen.py.")
    print("  Make sure PlatformIO + ESP-IDF framework are installed.")
    sys.exit(1)


def get_wifi_credentials(env: dict[str, str] | None = None) -> tuple[str, str]:
    """Return (ssid, password) from env, prompting if missing."""
    if env is None:
        env = load_env()

    ssid = env.get("KUBLET_SSID", "").strip()
    pw = env.get("KUBLET_PW", "").strip()

    if ssid and pw:
        print(f"  WiFi SSID: {ssid}")
        print(f"  WiFi Pass: {'•' * len(pw)}")
        use = input("  Use these credentials? [Y/n] ").strip().lower()
        if use not in ("", "y", "yes"):
            ssid, pw = "", ""

    if not ssid:
        ssid = input("  WiFi SSID: ").strip()
        if not ssid:
            print("Error: SSID cannot be empty.")
            sys.exit(1)

    if not pw:
        pw = input("  WiFi Password: ").strip()
        if not pw:
            print("Error: password cannot be empty.")
            sys.exit(1)

    env["KUBLET_SSID"] = ssid
    env["KUBLET_PW"] = pw
    save_env(env)

    return ssid, pw


def generate_nvs(ssid: str, pw: str, app_config: dict[str, str] | None = None) -> Path:
    """Generate NVS partition binary with WiFi credentials and optional app config."""
    print("\n🔧 Generating NVS partition...")

    buf = io.StringIO()
    writer = csv.writer(buf)
    writer.writerow(["key", "type", "encoding", "value"])
    writer.writerow(["core", "namespace", "", ""])
    writer.writerow(["ssid", "data", "string", ssid])
    writer.writerow(["pw", "data", "string", pw])

    if app_config:
        writer.writerow(["app", "namespace", "", ""])
        for key, value in app_config.items():
            writer.writerow([key, "data", "string", value])

    NVS_CSV.write_text(buf.getvalue())
    print(f"  ✓ Wrote {NVS_CSV.relative_to(REPO_ROOT)}")

    nvs_gen = find_nvs_gen()

    result = subprocess.run(
        [
            "uv",
            "run",
            "python",
            nvs_gen,
            "generate",
            str(NVS_CSV),
            str(NVS_BIN),
            NVS_PARTITION_SIZE,
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0 or not NVS_BIN.exists():
        print("  ✗ Failed to generate NVS binary.")
        if result.stderr:
            print(f"  {result.stderr.strip()}")
        sys.exit(1)

    size = NVS_BIN.stat().st_size
    print(f"  ✓ Generated {NVS_BIN.relative_to(REPO_ROOT)} ({size} bytes)")
    return NVS_BIN


def cleanup_generated_files() -> None:
    """Remove generated files that contain secrets."""
    for path in (NVS_CSV, NVS_BIN):
        if path.exists():
            path.unlink()


def blank_region(port: str, offset: str, size: int, label: str) -> None:
    """Overwrite a flash region with 0xFF using write_flash (avoids erase_region security restrictions)."""
    import tempfile

    print(f"\n🗑  Clearing {label} ({offset}, {size} bytes)...")
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(b"\xff" * size)
        tmp_path = Path(f.name)

    try:
        flash(port, [(offset, tmp_path)], label)
    finally:
        tmp_path.unlink(missing_ok=True)


def flash(port: str, flash_items: list[tuple[str, Path]], label: str) -> None:
    """Flash one or more offset/file pairs using esptool.py."""
    print(f"\n⚡ Flashing {label} to {port}...")

    for offset, path in flash_items:
        if not path.exists():
            print(f"  ✗ Missing: {path}")
            sys.exit(1)
        size_kb = path.stat().st_size / 1024
        print(f"  {offset}: {path.name} ({size_kb:.1f} KB)")

    cmd = [
        "uv",
        "run",
        "esptool.py",
        "-p",
        port,
        "-b",
        BAUD,
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "--chip",
        "esp32",
        "write_flash",
        "--flash_mode",
        FLASH_MODE,
        "--flash_size",
        "detect",
        "--flash_freq",
        FLASH_FREQ,
    ]
    for offset, path in flash_items:
        cmd.extend([offset, str(path)])

    result = subprocess.run(cmd, cwd=REPO_ROOT)
    if result.returncode != 0:
        print(f"\n  ✗ Flash failed (exit code {result.returncode})")
        sys.exit(1)

    print(f"\n  ✓ {label} flashed successfully!")
