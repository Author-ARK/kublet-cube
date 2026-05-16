"""Paths, constants, and shared helpers for the Kublet dev tool."""

import shutil
import subprocess
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent
TOOLS_DIR = REPO_ROOT / "tools"
APPS_DIR = REPO_ROOT / "apps"
FIRMWARE_DIR = REPO_ROOT / "firmware"
ENV_FILE = TOOLS_DIR / ".env"
DEVICES_FILE = TOOLS_DIR / ".devices.yml"
NVS_CSV = TOOLS_DIR / "nvs_wifi.csv"
NVS_BIN = TOOLS_DIR / "nvs_wifi.bin"

DEV_FIRMWARE = FIRMWARE_DIR / "dev_firmware.bin"
DEV_PARTITIONS = FIRMWARE_DIR / "dev_partitions.bin"
FACTORY_FIRMWARE = FIRMWARE_DIR / "firmware.bin"
FACTORY_PARTITIONS = FIRMWARE_DIR / "partitions.bin"
FACTORY_BOOTLOADER = FIRMWARE_DIR / "bootloader.bin"

# ---------------------------------------------------------------------------
# Hardware constants
# ---------------------------------------------------------------------------

NVS_PARTITION_SIZE = "0x5000"  # 20 KB, must match partition table
BAUD = "460800"
FLASH_MODE = "dio"
FLASH_FREQ = "40m"

# ST7789 display configuration for Kublet hardware
USER_SETUP_H = """\
#define USER_SETUP_INFO "User_Setup"
#define ST7789_DRIVER
#define TFT_RGB_ORDER TFT_BGR
#define TFT_WIDTH  240
#define TFT_HEIGHT 240
#define TFT_MISO -1
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS 5
#define TFT_DC 2
#define TFT_RST 4
#define TFT_BL 15
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
#define SPI_FREQUENCY 40000000
#define SPI_READ_FREQUENCY 27000000
#define SPI_TOUCH_FREQUENCY 2500000
"""


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def find_pio() -> str:
    """Locate the PlatformIO CLI binary."""
    pio = shutil.which("pio")
    if pio:
        return pio
    pio_venv = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    if pio_venv.exists():
        return str(pio_venv)
    print("Error: PlatformIO CLI (pio) not found.")
    print(
        "  Install it via: https://docs.platformio.org/en/latest/core/installation.html"
    )
    sys.exit(1)


def load_env(env_file: Path | None = None) -> dict[str, str]:
    """Load key=value pairs from an env file."""
    path = env_file or ENV_FILE
    env: dict[str, str] = {}
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, _, value = line.partition("=")
                env[key.strip()] = value.strip()
    return env


def save_env(env: dict[str, str], env_file: Path | None = None) -> None:
    """Write env dict back to an env file."""
    path = env_file or ENV_FILE
    lines = [f"{k}={v}" for k, v in env.items()]
    path.write_text("\n".join(lines) + "\n")
    print(f"  ✓ Saved credentials to {path.relative_to(REPO_ROOT)}")


def resolve_app_dir(app_name: str | None, apps_dir: Path | None = None) -> Path:
    """Resolve the app directory from name or current working directory."""
    apps = apps_dir or APPS_DIR

    if app_name:
        app_dir = apps / app_name
        if not app_dir.exists():
            try:
                label = f"{apps.relative_to(REPO_ROOT)}/"
            except ValueError:
                label = f"{apps}/"
            print(f"Error: app '{app_name}' not found in {label}")
            _print_available_apps(apps)
            sys.exit(1)
        return app_dir

    cwd = Path.cwd()
    if (cwd / "platformio.ini").exists():
        return cwd

    print("Error: no app specified and no platformio.ini in current directory")
    _print_available_apps(apps)
    sys.exit(1)


def get_local_ip() -> str | None:
    """Get this computer's local IP address."""
    try:
        result = subprocess.run(
            ["ipconfig", "getifaddr", "en0"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        ip = result.stdout.strip()
        if ip:
            return ip
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    return None


def _print_available_apps(apps_dir: Path) -> None:
    available = sorted(
        p.name for p in apps_dir.iterdir() if (p / "platformio.ini").exists()
    )
    if available:
        print(f"  Available apps: {', '.join(available)}")
