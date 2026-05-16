"""Build firmware and upload over WiFi OTA."""

import hashlib
import http.client
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from kublet_dev.config import USER_SETUP_H, find_pio
from kublet_dev.network import check_ota_endpoint


def write_user_setup(app_dir: Path) -> None:
    """Write the TFT_eSPI User_Setup.h with correct Kublet pin config."""
    setup_path = app_dir / ".pio/libdeps/esp32dev/TFT_eSPI/User_Setup.h"
    if not setup_path.parent.exists():
        pio = find_pio()
        print("  Dependencies not installed. Running pio lib install...")
        subprocess.run([pio, "lib", "install"], cwd=app_dir, check=True)

    setup_path.write_text(USER_SETUP_H)
    print("  ✓ Wrote User_Setup.h (SPI @ 40 MHz)")


def build_firmware(app_dir: Path) -> Path:
    """Compile firmware and return path to the .bin file."""
    pio = find_pio()
    app_name = app_dir.name
    print(f"Building '{app_name}'...")

    write_user_setup(app_dir)

    print("  Compiling...")
    result = subprocess.run([pio, "run"], cwd=app_dir)
    if result.returncode != 0:
        print("  ✗ Build failed")
        sys.exit(1)

    firmware = app_dir / ".pio/build/esp32dev/firmware.bin"
    size_kb = firmware.stat().st_size / 1024
    print(f"  ✓ Build succeeded ({size_kb:.0f} KB)")
    return firmware


def _verify_reboot(ip: str) -> None:
    """Check that the device rebooted and came back online after OTA."""
    print("  ⏳ Verifying reboot...", end="", flush=True)
    time.sleep(5)
    for _ in range(5):
        if check_ota_endpoint(ip):
            print(f"\r  ✓ Device back online{' ':30}")
            return
        time.sleep(3)
    print(f"\r  ⚠ Device not responding — may need power cycle{' ':10}")


def _print_progress(sent: int, total: int) -> None:
    """Print an inline progress bar."""
    pct = sent * 100 // total
    bar_len = 30
    filled = bar_len * sent // total
    bar = "█" * filled + "░" * (bar_len - filled)
    print(f"\r  {bar} {pct:3d}% ({sent // 1024}/{total // 1024} KB)", end="", flush=True)


def ota_send(ip: str, firmware: Path, app_name: str) -> None:
    """Upload firmware to device over WiFi OTA with progress bar."""
    size_kb = firmware.stat().st_size / 1024
    print(f"📡 Sending '{app_name}' ({size_kb:.0f} KB) to {ip}...")

    boundary = "----KubletOTA"
    firmware_data = firmware.read_bytes()

    # Compute MD5 hash for firmware integrity verification
    md5_hash = hashlib.md5(firmware_data).hexdigest()
    print(f"  🔒 Firmware MD5: {md5_hash}")

    header_part = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="filedata"; filename="firmware.bin"\r\n'
        f"Content-Type: application/octet-stream\r\n"
        f"\r\n"
    ).encode()
    footer_part = f"\r\n--{boundary}--\r\n".encode()

    content_length = len(header_part) + len(firmware_data) + len(footer_part)

    try:
        conn = http.client.HTTPConnection(ip, timeout=60)
        conn.putrequest("POST", "/update")
        conn.putheader("Content-Type", f"multipart/form-data; boundary={boundary}")
        conn.putheader("Content-Length", str(content_length))
        conn.putheader("X-Firmware-MD5", md5_hash)
        conn.endheaders()

        # Send multipart header
        conn.send(header_part)

        # Send firmware in chunks with progress
        chunk_size = 8192
        total = len(firmware_data)
        sent = 0
        while sent < total:
            end = min(sent + chunk_size, total)
            conn.send(firmware_data[sent:end])
            sent = end
            _print_progress(sent, total)

        # Send multipart footer
        conn.send(footer_part)
        print()  # newline after progress bar

        # Short timeout for response — device usually reboots before replying
        print("  ⏳ Waiting for device to flash...", end="", flush=True)
        conn.sock.settimeout(10)
        try:
            resp = conn.getresponse()
            resp.read()
            print(f"\r  ✅ '{app_name}' deployed to {ip}{'':30}")
        except (TimeoutError, ConnectionResetError, OSError):
            print(f"\r  ✅ '{app_name}' deployed to {ip} (device rebooting){'':10}")
        finally:
            conn.close()

    except (TimeoutError, ConnectionResetError, OSError) as e:
        print()
        print(f"  ✗ Upload failed: {e}")
        sys.exit(1)

    _verify_reboot(ip)
