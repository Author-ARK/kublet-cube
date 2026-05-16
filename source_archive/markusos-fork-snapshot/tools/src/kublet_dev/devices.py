"""Device registry — load, save, and resolve Kublet devices by name."""

import sys
from pathlib import Path

from kublet_dev.config import DEVICES_FILE, REPO_ROOT


def load_devices(devices_file: Path | None = None) -> dict[str, str]:
    """Load device registry from .devices.yml."""
    path = devices_file or DEVICES_FILE
    devices: dict[str, str] = {}
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and ":" in line:
                key, _, value = line.partition(":")
                devices[key.strip()] = value.strip()
    return devices


def save_devices(devices: dict[str, str], devices_file: Path | None = None) -> None:
    """Write device registry to .devices.yml."""
    path = devices_file or DEVICES_FILE
    lines = []
    default = devices.get("_default", "")
    if default:
        lines.append(f"_default: {default}")
    for name in sorted(devices):
        if name != "_default":
            lines.append(f"{name}: {devices[name]}")
    path.write_text("\n".join(lines) + "\n")


def save_device(name: str, ip: str, devices_file: Path | None = None) -> None:
    """Register a device name→IP and set it as the default."""
    path = devices_file or DEVICES_FILE
    devices = load_devices(path)
    devices[name] = ip
    devices["_default"] = name
    save_devices(devices, path)
    print(f"  ✓ Saved device '{name}' ({ip}) to {path.relative_to(REPO_ROOT)}")


def resolve_device_ip(
    device_name: str | None, ip_override: str | None, devices_file: Path | None = None
) -> str:
    """Resolve device IP: --ip flag > device name lookup > _default."""
    if ip_override:
        return ip_override

    devices = load_devices(devices_file)

    if device_name:
        ip = devices.get(device_name)
        if not ip:
            known = [n for n in sorted(devices) if n != "_default"]
            print(f"Error: Unknown device '{device_name}'.")
            if known:
                print(f"  Known devices: {', '.join(known)}")
            else:
                print(
                    "  No devices registered. Run './tools/dev init --name <name>' first."
                )
            sys.exit(1)
        return ip

    default_name = devices.get("_default")
    if default_name and default_name in devices:
        return devices[default_name]

    print("Error: No device specified and no default device set.")
    print("  Run './tools/dev init --name <name>' first, or pass a device name.")
    sys.exit(1)
