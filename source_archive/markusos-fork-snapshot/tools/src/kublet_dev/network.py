"""Device connectivity — serial IP reading, ping, mDNS, OTA health check."""

import re
import subprocess
import time
import urllib.error
import urllib.request

import serial


def read_ip_from_serial(port: str, timeout: int = 15) -> str | None:
    """Read the device's boot output from serial and extract the IP address."""
    print(f"\n📡 Reading boot output from {port} ({timeout}s)...")
    ip_pattern = re.compile(r"(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})")
    found_ip = None

    try:
        with serial.Serial(port, 460800, timeout=1) as ser:
            deadline = time.time() + timeout
            while time.time() < deadline:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                print(f"  {line}")
                if not found_ip:
                    match = ip_pattern.search(line)
                    if match:
                        ip = match.group(1)
                        if not ip.startswith("0.") and not ip.startswith("255."):
                            found_ip = ip
    except serial.SerialException as e:
        print(f"  ⚠ Serial error: {e}")

    return found_ip


def resolve_mdns_fallback() -> str | None:
    """Fallback: flush mDNS cache and do a fresh dns-sd lookup."""
    print("  Trying mDNS fallback...")
    for cmd in [["dscacheutil", "-flushcache"], ["killall", "-HUP", "mDNSResponder"]]:
        try:
            subprocess.run(cmd, capture_output=True, timeout=5)
        except (FileNotFoundError, subprocess.TimeoutExpired):
            pass

    time.sleep(1)

    ip_pattern = re.compile(r"(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})")
    try:
        proc = subprocess.Popen(
            ["dns-sd", "-G", "v4", "esp32.local"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            stdout, _ = proc.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, _ = proc.communicate()

        for line in stdout.splitlines():
            if "Add" in line:
                match = ip_pattern.search(line)
                if match:
                    return match.group(1)
    except FileNotFoundError:
        pass
    return None


def ping_device(ip: str, count: int = 3) -> bool:
    """Ping the device and return True if it responds."""
    print(f"\n🏓 Pinging {ip}...")
    result = subprocess.run(
        ["ping", "-c", str(count), "-t", "5", ip],
        capture_output=True,
        text=True,
    )
    if result.returncode == 0:
        for line in result.stdout.splitlines():
            if "packet loss" in line:
                print(f"  {line.strip()}")
                break
        return True
    return False


def check_ota_endpoint(ip: str) -> bool:
    """Check that the OTA HTTP server is responding."""
    print(f"  Checking OTA endpoint at http://{ip}/...")
    try:
        req = urllib.request.Request(f"http://{ip}/", method="GET")
        urllib.request.urlopen(req, timeout=5)
        return True
    except urllib.error.HTTPError:
        return True  # server responded with an error code — still alive
    except (urllib.error.URLError, OSError):
        return False


def wait_for_device(port: str, timeout: int = 15) -> str | None:
    """Wait for the device to come online and return its IP address."""
    ip = read_ip_from_serial(port, timeout=timeout)

    if not ip:
        ip = resolve_mdns_fallback()

    if not ip:
        print("\n  ⚠ Could not detect device IP.")
        print("  Check the device display or your router for the IP.")
        return None

    print(f"\n  ✓ Device IP: {ip}")

    if ping_device(ip):
        print("  ✓ Ping OK")
    else:
        print("  ⚠ Ping failed — device may still be booting")

    if check_ota_endpoint(ip):
        print("  ✓ OTA endpoint responding")
    else:
        print("  ⚠ OTA endpoint not reachable")

    return ip
