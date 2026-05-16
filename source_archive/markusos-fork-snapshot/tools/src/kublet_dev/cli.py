"""CLI entry point — argparse setup and command dispatch."""

import argparse
import subprocess
import sys

from kublet_dev.build import build_firmware, ota_send, write_user_setup
from kublet_dev.config import (
    APPS_DIR,
    DEV_FIRMWARE,
    DEV_PARTITIONS,
    FACTORY_BOOTLOADER,
    FACTORY_FIRMWARE,
    FACTORY_PARTITIONS,
    NVS_BIN,
    REPO_ROOT,
    find_pio,
    get_local_ip,
    load_env,
    resolve_app_dir,
)
from kublet_dev.devices import load_devices, resolve_device_ip, save_device
from kublet_dev.flash import (
    blank_region,
    cleanup_generated_files,
    detect_serial_port,
    flash,
    generate_nvs,
    get_wifi_credentials,
)
from kublet_dev.network import wait_for_device


# ===========================================================================
# Commands
# ===========================================================================


def cmd_build(args: argparse.Namespace) -> None:
    """Build/compile the firmware."""
    app_dir = resolve_app_dir(getattr(args, "app", None))
    build_firmware(app_dir)


def cmd_init(args: argparse.Namespace) -> None:
    """Flash dev firmware (or factory) with WiFi credentials."""
    print("╔══════════════════════════════════════╗")
    print("║       Kublet Dev Init                ║")
    print("╚══════════════════════════════════════╝")

    # 0. Device name
    device_name = args.name
    if not device_name and not args.factory:
        device_name = input("\n  Device name: ").strip()
        if not device_name:
            print("Error: device name cannot be empty.")
            sys.exit(1)

    # 1. WiFi credentials
    print("\n📶 WiFi Credentials")
    env = load_env()
    ssid, pw = get_wifi_credentials(env)

    # 2. Detect local IP for app server URL
    app_config: dict[str, str] = {}
    local_ip = get_local_ip()
    if local_ip:
        server_url = f"http://{local_ip}:8198"
        print(f"\n🖥  Server URL: {server_url}")
        app_config["server_url"] = server_url
    else:
        print("\n  ⚠ Could not detect local IP — server_url not set in NVS")

    # 3. Generate NVS
    generate_nvs(ssid, pw, app_config=app_config if app_config else None)

    try:
        # 4. Detect serial port
        port = args.port or detect_serial_port()
        print(f"\n🔌 Serial port: {port}")

        # 5. Flash
        if args.factory:
            flash(
                port,
                [
                    ("0x1000", FACTORY_BOOTLOADER),
                    ("0x8000", FACTORY_PARTITIONS),
                    ("0x9000", NVS_BIN),
                    ("0x10000", FACTORY_FIRMWARE),
                ],
                "factory firmware (full restore)",
            )
            print("\n✅ Factory firmware restored.")
            print("  Note: Factory firmware does not have WiFi OTA.")
            print(
                "  Run `./tools/dev init --name <name>` to enable wireless app deployment."
            )
            return

        blank_region(port, "0xE000", 0x2000, "OTA data")
        flash(
            port,
            [
                ("0x8000", DEV_PARTITIONS),
                ("0x9000", NVS_BIN),
                ("0x10000", DEV_FIRMWARE),
            ],
            "dev firmware + WiFi credentials",
        )

        # 6. Verify and register device
        if not args.no_wait:
            ip = wait_for_device(port)
            if ip:
                save_device(device_name, ip)
                print("\n🎉 Ready! Deploy apps with:")
                print(f"    ./tools/dev deploy <app> {device_name}")
    finally:
        cleanup_generated_files()


def cmd_logs(args: argparse.Namespace) -> None:
    """Stream serial logs from the Kublet via USB."""
    port = args.port or detect_serial_port()
    pio = find_pio()

    cwd = None
    if args.app:
        app_dir = APPS_DIR / args.app
        if not app_dir.is_dir() or not (app_dir / "platformio.ini").exists():
            print(
                f"Error: App '{args.app}' not found at {app_dir.relative_to(REPO_ROOT)}/"
            )
            available = sorted(
                p.name
                for p in APPS_DIR.iterdir()
                if p.is_dir() and (p / "platformio.ini").exists()
            )
            if available:
                print(f"  Available apps: {', '.join(available)}")
            sys.exit(1)
        cwd = app_dir

    baud = "460800"
    label = f" (app: {args.app})" if args.app else ""
    print(f"📡 Streaming logs from {port}{label}  — press Ctrl+C to stop\n")

    try:
        subprocess.run(
            [pio, "device", "monitor", "--port", port, "--baud", baud],
            cwd=cwd,
        )
    except KeyboardInterrupt:
        print("\n\n🛑 Log stream stopped.")


def cmd_devices(_args: argparse.Namespace) -> None:
    """List all registered Kublet devices."""
    devices = load_devices()
    default = devices.get("_default", "")
    names = sorted(n for n in devices if n != "_default")

    if not names:
        print("No devices registered.")
        print("  Run './tools/dev init --name <name>' to register a device.")
        return

    for name in names:
        suffix = "  (default)" if name == default else ""
        print(f"  {name:<16} {devices[name]}{suffix}")


def cmd_deploy(args: argparse.Namespace) -> None:
    """Build and send an app to the Kublet over WiFi OTA."""
    app_dir = resolve_app_dir(args.app)
    app_name = app_dir.name

    ip = resolve_device_ip(getattr(args, "device", None), args.ip)

    print(f"🚀 Deploying '{app_name}' to {ip}")

    # Run app-specific setup if present (e.g. OAuth flows, code generation)
    setup_script = app_dir / "setup.py"
    if setup_script.exists() and not args.skip_build:
        print(f"\n⚙  Running setup for {app_name}...")
        result = subprocess.run([sys.executable, str(setup_script)], cwd=app_dir)
        if result.returncode != 0:
            print("  ✗ Setup failed.")
            sys.exit(1)

    firmware_bin = app_dir / ".pio" / "build" / "esp32dev" / "firmware.bin"

    if not args.skip_build:
        pio = find_pio()
        print(f"\n🔨 Building {app_name}...")
        write_user_setup(app_dir)
        print("  Compiling...")
        result = subprocess.run([pio, "run"], cwd=app_dir)
        if result.returncode != 0 or not firmware_bin.exists():
            print("\n  ✗ Build failed.")
            sys.exit(1)
        size_kb = firmware_bin.stat().st_size / 1024
        print(f"  ✓ Build succeeded ({size_kb:.0f} KB)")

    if not firmware_bin.exists():
        print("  ✗ No firmware found. Run without --skip-build first.")
        sys.exit(1)

    print()
    ota_send(ip, firmware_bin, app_name)


# ===========================================================================
# Main
# ===========================================================================


def main():
    parser = argparse.ArgumentParser(
        prog="dev",
        description="Kublet development tool — build, deploy, and manage your device.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
commands:
  build    Compile firmware for an app
  deploy   Build and send an app to a device over WiFi
  devices  List registered Kublet devices
  logs     Stream serial logs from the Kublet via USB
  init     Flash dev firmware + WiFi credentials via USB

examples:
  ./tools/dev init --name kitchen             # flash & register as "kitchen"
  ./tools/dev build stock                     # compile the stock app
  ./tools/dev deploy stock                    # deploy to default device
  ./tools/dev deploy stock kitchen            # deploy to "kitchen"
  ./tools/dev deploy stock --ip 192.168.1.50  # override with explicit IP
  ./tools/dev devices                         # list registered devices
  ./tools/dev logs                            # stream logs (auto-detect port)
        """,
    )
    subparsers = parser.add_subparsers(dest="command", title="commands")

    # -- build --
    p_build = subparsers.add_parser(
        "build",
        help="Compile firmware (writes User_Setup.h with 40 MHz SPI)",
    )
    p_build.add_argument(
        "app",
        nargs="?",
        help="App name (default: current directory)",
    )

    # -- init --
    p_init = subparsers.add_parser(
        "init",
        help="Flash dev firmware + WiFi credentials via USB serial",
    )
    p_init.add_argument(
        "--name",
        help="Device name (e.g. kitchen, office). Prompted if omitted.",
    )
    p_init.add_argument(
        "-p",
        "--port",
        help="USB serial port (auto-detected if omitted)",
    )
    p_init.add_argument(
        "--factory",
        action="store_true",
        help="Flash factory firmware instead of dev firmware",
    )
    p_init.add_argument(
        "--no-wait",
        action="store_true",
        help="Skip post-flash connectivity checks",
    )

    # -- logs --
    p_logs = subparsers.add_parser(
        "logs",
        help="Stream serial logs from the Kublet via USB",
    )
    p_logs.add_argument(
        "app",
        nargs="?",
        help="App directory name (uses its platformio.ini settings)",
    )
    p_logs.add_argument(
        "-p",
        "--port",
        help="USB serial port (auto-detected if omitted)",
    )

    # -- devices --
    subparsers.add_parser(
        "devices",
        help="List all registered Kublet devices",
    )

    # -- deploy --
    p_deploy = subparsers.add_parser(
        "deploy",
        help="Build and send an app to the Kublet over WiFi OTA",
    )
    p_deploy.add_argument(
        "app",
        help="App directory name under apps/ (e.g. badgers, hello)",
    )
    p_deploy.add_argument(
        "device",
        nargs="?",
        help="Device name from registry (default: last initialized device)",
    )
    p_deploy.add_argument(
        "--ip",
        help="Override device IP directly (bypasses registry)",
    )
    p_deploy.add_argument(
        "--skip-build",
        action="store_true",
        help="Skip the build step and only send the existing firmware",
    )

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    commands = {
        "build": cmd_build,
        "init": cmd_init,
        "logs": cmd_logs,
        "devices": cmd_devices,
        "deploy": cmd_deploy,
    }
    commands[args.command](args)
