"""USB serial flashing, NVS generation, and WiFi credential management."""

import csv
import io
import os
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
    """Locate nvs_partition_gen.py — prefer the pip-installed package
    (esp-idf-nvs-partition-gen), fall back to PlatformIO's bundled copy."""
    try:
        import esp_idf_nvs_partition_gen  # noqa: F401

        pkg_path = (
            Path(esp_idf_nvs_partition_gen.__file__).parent / "nvs_partition_gen.py"
        )
        if pkg_path.exists():
            return str(pkg_path)
    except ImportError:
        pass

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
    print("  Make sure PlatformIO + ESP-IDF framework are installed,")
    print("  or `pip install esp-idf-nvs-partition-gen` in your venv.")
    sys.exit(1)


def get_wifi_credentials(env: dict[str, str] | None = None) -> tuple[str, str]:
    """Return (ssid, password) from env, prompting if missing.

    Lookup order: explicit env-file dict > process environment (os.environ) >
    interactive prompt. Process env support lets webui/CI invoke init
    non-interactively by setting KUBLET_SSID + KUBLET_PW + KUBLET_NONINTERACTIVE=1.
    """
    if env is None:
        env = load_env()

    ssid = env.get("KUBLET_SSID", "").strip()
    pw = env.get("KUBLET_PW", "").strip()
    if not ssid:
        ssid = os.environ.get("KUBLET_SSID", "").strip()
    if not pw:
        pw = os.environ.get("KUBLET_PW", "").strip()
    noninteractive = os.environ.get("KUBLET_NONINTERACTIVE", "").strip() in ("1", "true", "yes")

    if ssid and pw:
        print(f"  WiFi SSID: {ssid}")
        print(f"  WiFi Pass: {'•' * len(pw)}")
        if not noninteractive:
            try:
                use = input("  Use these credentials? [Y/n] ").strip().lower()
                if use not in ("", "y", "yes"):
                    ssid, pw = "", ""
            except EOFError:
                pass  # no stdin available — accept the credentials

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
            sys.executable,
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


def flash(port: str, flash_items: list[tuple[str, Path]], label: str,
          after: str = "hard_reset") -> None:
    """Flash one or more offset/file pairs using esptool.py.

    `after` controls esptool's post-flash action — default `hard_reset`
    fires RTS, but the smart-USB OTA-style deploy uses `no_reset` so it
    can perform multiple sequential writes (firmware → otadata) without
    bouncing the cube between them."""
    print(f"\n⚡ Flashing {label} to {port}...")

    for offset, path in flash_items:
        if not path.exists():
            print(f"  ✗ Missing: {path}")
            sys.exit(1)
        size_kb = path.stat().st_size / 1024
        print(f"  {offset}: {path.name} ({size_kb:.1f} KB)")

    cmd = [
        "esptool.py",
        "-p",
        port,
        "-b",
        BAUD,
        "--before",
        "default_reset",
        "--after",
        after,
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


def read_flash_region(port: str, offset_hex: str, size: int) -> bytes | None:
    """Read `size` bytes from `offset_hex` of the device's flash and return
    them. Returns None (without exiting) on any failure — some Kublet
    units don't handshake cleanly with esptool's stub-loader during
    read_flash, so the caller can fall back to a no-read path. We try
    two strategies before giving up:

    1. Default invocation (stub-loader on, BAUD=460800).
    2. `--no-stub` plus a slower 115200 baud — talks directly to the
       ROM bootloader, far more tolerant of serial noise but ~5x slower.
    """
    import tempfile

    attempts = [
        # (extra-args, baud, label)
        ([], BAUD, "stub@460800"),
        (["--no-stub"], "115200", "ROM@115200"),
    ]

    for extra, baud, label in attempts:
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            tmp_path = Path(f.name)
        try:
            cmd = [
                "esptool.py", "-p", port, "-b", baud,
                "--before", "default_reset", "--after", "no_reset",
                "--chip", "esp32",
            ] + extra + [
                "read_flash",
                offset_hex, f"0x{size:X}", str(tmp_path),
            ]
            result = subprocess.run(cmd, cwd=REPO_ROOT,
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
            if result.returncode == 0:
                data = tmp_path.read_bytes()
                if label != "stub@460800":
                    print(f"  ↺ read_flash succeeded via fallback ({label})")
                return data
            print(f"  ✗ read_flash {label} failed (rc={result.returncode}) — trying next")
        finally:
            tmp_path.unlink(missing_ok=True)

    print("  ✗ read_flash attempts exhausted — caller should pick a fallback path")
    return None


# ---------------------------------------------------------------------------
# Smart USB deploy — mirrors OTA's partition handling.
# ---------------------------------------------------------------------------
# Standard ESP-IDF dual-OTA layout used by the community fork's dev firmware:
#   nvs       0x09000  0x05000
#   otadata   0x0E000  0x02000   (two 4 KB blocks)
#   ota_0     0x10000  0x140000
#   ota_1     0x150000 0x140000
#
# These offsets are baked into the firmware that init flashed; if you change
# the partition table you must update these too.
_OTADATA_OFFSET = 0x0E000
_OTADATA_SIZE   = 0x02000
_OTA_BLOCK_SIZE = 0x01000          # 4 KB per otadata slot
_OTA_SLOT_OFFSETS = [0x10000, 0x150000]   # offsets of ota_0, ota_1


def _ota_crc(seq: int) -> int:
    """Compute the CRC32 the ESP-IDF bootloader stores alongside ota_seq.

    Per esp_ota_ops / bootloader_common_ota_select_crc():
        crc = crc32_le(UINT32_MAX, &ota_seq, 4)
    which corresponds to zlib.crc32() XOR'd with 0xFFFFFFFF."""
    import zlib
    return (zlib.crc32(seq.to_bytes(4, "little")) ^ 0xFFFFFFFF) & 0xFFFFFFFF


def _parse_ota_block(block: bytes) -> int | None:
    """Return ota_seq if the block is valid (CRC matches), else None."""
    import struct
    if len(block) < 32:
        return None
    seq = struct.unpack("<I", block[0:4])[0]
    if seq == 0xFFFFFFFF:  # erased flash
        return None
    stored_crc = struct.unpack("<I", block[28:32])[0]
    if stored_crc != _ota_crc(seq):
        return None
    return seq


def smart_usb_deploy(port: str, firmware_bin: Path, app_name: str) -> None:
    """OTA-style USB deploy that mirrors Arduino's Update.h behavior:

      1. Read the cube's otadata partition to learn which OTA slot is
         currently active.
      2. Write the new firmware to the *inactive* slot — never overwrite
         the running image.
      3. Build a new otadata entry with seq = max_seq + 1 and write it
         to the older/invalid otadata block.
      4. Hard-reset; the bootloader picks the higher-seq block, computes
         (seq - 1) % 2, and boots from the slot we just wrote.

    If the bootloader doesn't accept our CRC (algorithm mismatch) it
    falls back to the other otadata block, which is still valid, so the
    cube remains bootable in the worst case."""
    import hashlib, struct, tempfile

    size_kb = firmware_bin.stat().st_size / 1024
    fw_md5 = hashlib.md5(firmware_bin.read_bytes()).hexdigest()

    print(f"\n📡 OTA-style USB deploy of '{app_name}' on {port}")
    print(f"   firmware = {size_kb:.1f} KB · md5 = {fw_md5}")

    # ── 1. Read otadata ────────────────────────────────────────────
    print(f"\n[1/4] Reading otadata @ 0x{_OTADATA_OFFSET:X} ({_OTADATA_SIZE} bytes)...")
    otadata = read_flash_region(port, f"0x{_OTADATA_OFFSET:X}", _OTADATA_SIZE)

    if otadata is None:
        # esptool couldn't get a clean read (stub handshake noise, weak
        # USB cable, marginal CP210x driver, etc.). Drop to the simple
        # path: blank otadata so the bootloader falls back to ota_0,
        # then write the firmware there. Functionally equivalent to a
        # successful slot rotation that always picks ota_0.
        print("\n[fallback] Using simple write — read_flash unavailable on this cube.")
        print(f"  → writing firmware to ota_0 (0x{_OTA_SLOT_OFFSETS[0]:X})")
        print(f"  → clearing otadata so the bootloader uses ota_0")
        blank_region(port, f"0x{_OTADATA_OFFSET:X}", _OTADATA_SIZE, "OTA data")
        flash(port, [(f"0x{_OTA_SLOT_OFFSETS[0]:X}", firmware_bin)],
              f"'{app_name}' → ota_0", after="hard_reset")
        print(f"\n[done] ✅ '{app_name}' deployed (fallback path)")
        print( "       cube will boot ota_0 after the RTS hard reset")
        return

    seq0 = _parse_ota_block(otadata[:_OTA_BLOCK_SIZE])
    seq1 = _parse_ota_block(otadata[_OTA_BLOCK_SIZE:2 * _OTA_BLOCK_SIZE])
    valid = [s for s in (seq0, seq1) if s is not None]

    if not valid:
        # No valid otadata → bootloader defaults to ota_0. Target ota_1
        # so the next run has a valid pointer and proper OTA rotation
        # going forward.
        active_slot = 0
        new_seq = 1
        write_block_idx = 0  # write into block 0 to seed otadata
        print(f"   otadata blank/invalid (seq0={seq0}, seq1={seq1}) — seeding seq=1")
    else:
        max_seq = max(valid)
        active_slot = (max_seq - 1) % 2
        new_seq = max_seq + 1
        # Write into the older / invalid block so the bootloader picks
        # the new (higher-seq) one next time.
        if seq0 is None or (seq1 is not None and seq0 < seq1):
            write_block_idx = 0
        else:
            write_block_idx = 1
        print(f"   active = ota_{active_slot} (max_seq={max_seq})")

    target_slot = 1 - active_slot if valid else 0
    target_offset = _OTA_SLOT_OFFSETS[target_slot]
    print(f"   → writing firmware to ota_{target_slot} @ 0x{target_offset:X}")
    print(f"   → new otadata block #{write_block_idx} will carry seq={new_seq}")

    # ── 2. Write firmware to inactive slot (no_reset between steps) ─
    print(f"\n[2/4] Transferring firmware binary ({size_kb:.1f} KB)...")
    flash(port, [(f"0x{target_offset:X}", firmware_bin)],
          f"'{app_name}' → ota_{target_slot}", after="no_reset")

    # ── 3. Write new otadata block ─────────────────────────────────
    print(f"\n[3/4] Bumping otadata pointer (seq={new_seq}, slot=ota_{target_slot})...")
    new_block = bytearray(b"\xFF" * _OTA_BLOCK_SIZE)
    struct.pack_into("<I", new_block, 0, new_seq)
    # ota_state at offset 24 — leave at 0xFFFFFFFF (UNDEFINED) so the
    # bootloader treats the image as bootable. ESP-IDF marks images as
    # VALID/INVALID via app-level esp_ota_mark_app_valid_cancel_rollback();
    # for our purposes "undefined" is bootable.
    struct.pack_into("<I", new_block, 28, _ota_crc(new_seq))

    new_block_offset = _OTADATA_OFFSET + write_block_idx * _OTA_BLOCK_SIZE
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(bytes(new_block))
        tmp_path = Path(f.name)
    try:
        flash(port, [(f"0x{new_block_offset:X}", tmp_path)],
              f"otadata block #{write_block_idx} (seq={new_seq})",
              after="hard_reset")
    finally:
        tmp_path.unlink(missing_ok=True)

    # ── 4. Done ────────────────────────────────────────────────────
    print(f"\n[4/4] ✅ '{app_name}' deployed via USB (OTA-style)")
    print(f"      cube will boot ota_{target_slot} after the RTS hard reset")
    print(f"      (if it doesn't, manually power-cycle the USB cable)")
