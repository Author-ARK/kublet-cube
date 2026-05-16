"""Kublet fleet web UI — pick app + target device, build with per-device
KUBLET_NUM, OTA-deploy. Also handles first-time USB flashing of new cubes,
and a desktop SDL2 emulator target for debugging without hardware.
Runs on port 1666."""

import concurrent.futures
import glob
import json
import os
import re
import signal
import socket
import subprocess
import threading
import time
import uuid
from pathlib import Path

from flask import Flask, jsonify, render_template, request

ROOT = Path(__file__).resolve().parent
APPS_REPO = ROOT.parent / "kublet-apps"
DEVICES_FILE = ROOT / "devices.json"
ENV_FILE = APPS_REPO / "tools" / ".env"  # dev tool persists creds here on init
EMULATE_TOOL = APPS_REPO / "tools" / "emulate"
# The emulator dumps the live framebuffer here as <device>.png on a fixed
# interval (atomically via .tmp + rename). The Simulator tab polls these files.
FRAME_DIR = Path(os.environ.get("KUBLET_FRAME_DIR", "/tmp/kublet-emu"))
FRAME_DIR.mkdir(parents=True, exist_ok=True)
FRAME_INTERVAL_MS = 200  # ~5 fps — generous for the cube's 1 Hz refresh apps
JOBS: dict[str, dict] = {}
JOBS_LOCK = threading.Lock()

# Tracks live SDL2 emulator processes keyed by device name. Re-deploying to
# the same emulator slot kills the previous process before launching the new
# one; deleting the device kills it too.
EMULATOR_PROCS: dict[str, subprocess.Popen] = {}
EMULATOR_LOCK = threading.Lock()

app = Flask(__name__)


def load_devices() -> list[dict]:
    if not DEVICES_FILE.exists():
        return []
    data = json.loads(DEVICES_FILE.read_text())
    return data.get("devices", [])


def save_devices(devices: list[dict]) -> None:
    DEVICES_FILE.write_text(json.dumps({"devices": devices}, indent=2))


def update_device_field(name: str, **fields) -> None:
    """Patch a single device record by name. No-op if the device isn't registered."""
    devices = load_devices()
    changed = False
    for d in devices:
        if d.get("name") == name:
            d.update(fields)
            changed = True
            break
    if changed:
        save_devices(devices)


def load_saved_wifi() -> dict[str, str]:
    """Read the SSID + (optionally) password persisted by the dev tool.

    The dev tool writes KUBLET_SSID/KUBLET_PW to kublet-apps/tools/.env after
    a successful init, gitignored. We pre-fill the Flash Me form from there
    so the user doesn't re-type their WiFi password every time. The PW is
    sent back as a marker only ('saved') so it isn't echoed in HTML."""
    out = {"ssid": "", "pw_saved": False}
    if not ENV_FILE.exists():
        return out
    for line in ENV_FILE.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        v = v.strip().strip('"').strip("'")
        if k.strip() == "KUBLET_SSID":
            out["ssid"] = v
        elif k.strip() == "KUBLET_PW":
            out["pw_saved"] = bool(v)
    return out


def get_saved_password() -> str | None:
    """Return the persisted password (used when the form posts pw='__saved__').
    Kept separate from load_saved_wifi() so the password never enters the
    rendered HTML."""
    if not ENV_FILE.exists():
        return None
    for line in ENV_FILE.read_text().splitlines():
        line = line.strip()
        if line.startswith("KUBLET_PW="):
            return line.split("=", 1)[1].strip().strip('"').strip("'")
    return None


def list_apps() -> list[dict]:
    """Inventory of every buildable app. Pull description from manifest.yaml
    if present, else from the first H1 of README.md. Note any preview image
    in assets/preview.{gif,png,jpg} so the catalog can render thumbnails."""
    apps_dir = APPS_REPO / "apps"
    if not apps_dir.is_dir():
        return []
    out = []
    for entry in sorted(apps_dir.iterdir()):
        if not entry.is_dir():
            continue
        manifest = entry / "manifest.yaml"
        readme = entry / "README.md"
        summary = ""
        if manifest.exists():
            for line in manifest.read_text().splitlines():
                m = re.match(r"^(summary|desc|description)\s*:\s*(.+)", line.strip())
                if m:
                    summary = m.group(2).strip().strip('"').strip("'")
                    break
        if not summary and readme.exists():
            for line in readme.read_text().splitlines():
                if line.startswith("# "):
                    summary = line[2:].strip()
                    break
        preview = ""
        for ext in ("gif", "png", "jpg", "jpeg"):
            cand = entry / "assets" / f"preview.{ext}"
            if cand.exists():
                preview = ext
                break
        try:
            added_at = int(entry.stat().st_mtime)
        except OSError:
            added_at = 0
        out.append({
            "name": entry.name,
            "summary": summary,
            "preview": preview,
            "added_at": added_at,
        })
    # Default order: most-recently-added first. The catalog grid can be
    # re-sorted A→Z / Z→A client-side without re-fetching.
    out.sort(key=lambda a: -a["added_at"])
    return out


def list_serial_ports() -> list[str]:
    """List USB-serial ports likely to be a Kublet (macOS + Linux conventions)."""
    patterns = [
        "/dev/cu.usbserial-*",
        "/dev/cu.SLAB_USBtoUART*",
        "/dev/cu.wchusbserial*",
        "/dev/ttyUSB*",
        "/dev/ttyACM*",
    ]
    found = []
    for p in patterns:
        found.extend(sorted(glob.glob(p)))
    return found


def run_init_job(job_id: str, name: str, ssid: str, pw: str, port: str | None) -> None:
    """Flash dev firmware + WiFi credentials to a freshly-plugged kublet."""
    log_lines: list[str] = []

    def log(msg: str) -> None:
        with JOBS_LOCK:
            log_lines.append(msg)
            JOBS[job_id]["log"] = "\n".join(log_lines)

    log(f"=== Initializing kublet '{name}' ===")
    env = os.environ.copy()
    env["KUBLET_SSID"] = ssid
    env["KUBLET_PW"] = pw
    env["KUBLET_NONINTERACTIVE"] = "1"
    env["PATH"] = venv_path_prefix() + os.pathsep + env.get("PATH", "")

    cmd = [
        venv_python(),
        str(APPS_REPO / "tools" / "dev"),
        "init",
        "--name",
        name,
    ]
    if port:
        cmd += ["-p", port]

    log(f"$ KUBLET_SSID=*** KUBLET_PW=*** {' '.join(cmd)}")

    try:
        proc = subprocess.Popen(
            cmd,
            cwd=str(APPS_REPO),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            log(line.rstrip())
        rc = proc.wait()
        log(f"=== exit {rc} ===")
        with JOBS_LOCK:
            JOBS[job_id]["status"] = "ok" if rc == 0 else "failed"
    except Exception as exc:
        log(f"!! exception: {exc}")
        with JOBS_LOCK:
            JOBS[job_id]["status"] = "failed"


def venv_python() -> str:
    """Locate the kublet-apps venv python so subprocess has pio on PATH."""
    candidate = APPS_REPO / "kublet_env" / "bin" / "python"
    if candidate.exists():
        return str(candidate)
    return "python3"


def venv_path_prefix() -> str:
    """PATH entry for kublet-apps venv bin so pio is discoverable."""
    return str(APPS_REPO / "kublet_env" / "bin")


def stop_emulator(name: str) -> bool:
    """Kill any running SDL2 emulator process for device `name`. Returns True
    if a live process was found and signalled."""
    with EMULATOR_LOCK:
        proc = EMULATOR_PROCS.pop(name, None)
    if proc is None or proc.poll() is not None:
        return False
    try:
        # tools/emulate spawns cmake/the binary; killing the process group
        # makes sure children (sdl window, ffmpeg, etc.) go too.
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except ProcessLookupError:
        return False
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
    return True


def emulator_running(name: str) -> bool:
    with EMULATOR_LOCK:
        proc = EMULATOR_PROCS.get(name)
    return proc is not None and proc.poll() is None


def run_emulate_job(job_id: str, app_name: str, device: dict) -> None:
    """Build the SDL2 emulator for `app_name` and launch it natively on the
    host. Streams build output AND the running emulator's stdout (which is
    where Serial.print* goes via the mock layer) into the job log — that's
    the whole point of this target, since it lets us see the Serial output
    a real cube would have printed."""
    log_lines: list[str] = []

    def log(msg: str) -> None:
        with JOBS_LOCK:
            log_lines.append(msg)
            JOBS[job_id]["log"] = "\n".join(log_lines)

    name = device["name"]
    log(f"=== Launching '{app_name}' in emulator slot '{name}' (#{device['num']}) ===")

    if Path("/.dockerenv").exists():
        log("!! webui is running inside Docker — cannot open an SDL window on the host.")
        log("   Run app.py directly with the venv python for emulator targets.")
        with JOBS_LOCK:
            JOBS[job_id]["status"] = "failed"
        return

    if not EMULATE_TOOL.exists():
        log(f"!! emulator wrapper not found at {EMULATE_TOOL}")
        with JOBS_LOCK:
            JOBS[job_id]["status"] = "failed"
        return

    if stop_emulator(name):
        log(f"(stopped previous emulator process for '{name}')")

    env = os.environ.copy()
    env["KUBLET_NUM"] = str(device["num"])  # picked up by future apps that read it
    env["PATH"] = venv_path_prefix() + os.pathsep + env.get("PATH", "")

    frame_path = FRAME_DIR / f"{name}.png"
    # Remove any stale frame from a previous session so the Simulator tab
    # doesn't show a misleading old image during the build phase.
    try:
        frame_path.unlink(missing_ok=True)
    except OSError:
        pass

    cmd = [
        "bash", str(EMULATE_TOOL), app_name,
        "--frame-dump", str(frame_path),
        "--frame-interval", str(FRAME_INTERVAL_MS),
    ]
    log(f"$ KUBLET_NUM={device['num']} {' '.join(cmd)}")
    started = time.time()

    try:
        proc = subprocess.Popen(
            cmd,
            cwd=str(APPS_REPO),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            start_new_session=True,  # own process group, so killpg works
        )
    except Exception as exc:
        log(f"!! exception launching emulator: {exc}")
        with JOBS_LOCK:
            JOBS[job_id]["status"] = "failed"
        return

    with EMULATOR_LOCK:
        EMULATOR_PROCS[name] = proc

    update_device_field(name, last_app=app_name, last_deployed_at=int(started))

    assert proc.stdout is not None
    try:
        for line in proc.stdout:
            log(line.rstrip())
    except Exception as exc:
        log(f"!! log stream error: {exc}")

    rc = proc.wait()
    log(f"=== emulator exited (rc={rc}) ===")

    with EMULATOR_LOCK:
        if EMULATOR_PROCS.get(name) is proc:
            EMULATOR_PROCS.pop(name, None)

    with JOBS_LOCK:
        # rc 0 = clean quit (user pressed Q or closed the window)
        # rc -SIGTERM/-SIGKILL = we killed it on stop or re-deploy — still "ok"
        JOBS[job_id]["status"] = "ok" if rc in (0, -signal.SIGTERM, -signal.SIGKILL) else "failed"


def run_deploy_job(job_id: str, app_name: str, device: dict,
                   usb: bool = False, port: str | None = None) -> None:
    """Build firmware with KUBLET_NUM baked in, then deploy. Defaults to OTA;
    if `usb=True`, flashes over esptool via the named serial port (or
    auto-detect if `port` is None) — the fallback for cubes with WiFi signal
    too weak to complete a 1 MB OTA push."""
    log_lines: list[str] = []

    def log(msg: str) -> None:
        with JOBS_LOCK:
            log_lines.append(msg)
            JOBS[job_id]["log"] = "\n".join(log_lines)

    mode = "USB" if usb else "OTA"
    log(f"=== Deploying '{app_name}' to {device['name']} ({device['ip']}, #{device['num']}) via {mode} ===")
    env = os.environ.copy()
    env["PLATFORMIO_BUILD_FLAGS"] = f"-DKUBLET_NUM={device['num']}"
    env["PATH"] = venv_path_prefix() + os.pathsep + env.get("PATH", "")

    cmd = [
        venv_python(),
        str(APPS_REPO / "tools" / "dev"),
        "deploy",
        app_name,
    ]
    if usb:
        cmd.append("--usb")
        if port:
            cmd += ["-p", port]
    else:
        cmd += ["--ip", device["ip"]]
    log(f"$ KUBLET_NUM={device['num']} {' '.join(cmd)}")
    deploy_started = time.time()

    try:
        # Pipe "y\n" repeatedly so any "[Y/n]" confirmation prompt the dev
        # tool emits gets accepted without blocking on stdin
        proc = subprocess.Popen(
            cmd,
            cwd=str(APPS_REPO),
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert proc.stdout is not None and proc.stdin is not None
        try:
            proc.stdin.write("y\ny\ny\n")
            proc.stdin.flush()
        except BrokenPipeError:
            pass
        for line in proc.stdout:
            log(line.rstrip())
        rc = proc.wait()
        log(f"=== exit {rc} ===")
        with JOBS_LOCK:
            JOBS[job_id]["status"] = "ok" if rc == 0 else "failed"
        if rc == 0:
            update_device_field(
                device["name"],
                last_app=app_name,
                last_deployed_at=int(deploy_started),
            )
    except Exception as exc:
        log(f"!! exception: {exc}")
        with JOBS_LOCK:
            JOBS[job_id]["status"] = "failed"


@app.route("/")
def index() -> str:
    devices = load_devices()
    for d in devices:
        if d.get("kind") == "emulator":
            d["running"] = emulator_running(d["name"])
    return render_template(
        "index.html",
        apps=list_apps(),
        devices=devices,
        ports=list_serial_ports(),
        saved_wifi=load_saved_wifi(),
    )


@app.route("/api/diagnostics")
def api_diagnostics():
    """Probe what macOS / Linux can see right now — useful when a cube isn't
    being detected on USB. Output goes into the Flash Me troubleshooting card."""
    info = {
        "serial_ports": list_serial_ports(),
        "system_serial_glob": sorted(glob.glob("/dev/cu.*"))[:20],
        "system_tty_glob": sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))[:20],
    }
    # macOS-only: ask system_profiler whether it sees a Silicon Labs CP210x device
    try:
        cp = subprocess.run(
            ["system_profiler", "SPUSBDataType", "-detailLevel", "mini"],
            capture_output=True, text=True, timeout=10,
        )
        if cp.returncode == 0:
            lines = cp.stdout.splitlines()
            relevant = []
            for i, line in enumerate(lines):
                low = line.lower()
                if any(kw in low for kw in ("silicon labs", "cp210", "ch340", "kublet", "esp32")):
                    # Grab the matching line plus 1 line of surrounding context
                    relevant.extend(lines[max(0, i - 1):i + 2])
            info["macos_usb_match"] = relevant if relevant else "no CP210x/CH340/Kublet/ESP32 device reported by macOS"
    except (FileNotFoundError, subprocess.TimeoutExpired):
        info["macos_usb_match"] = "system_profiler not available (non-macOS or sandboxed)"
    return jsonify(info)


@app.route("/api/serial-ports")
def api_serial_ports():
    return jsonify(list_serial_ports())


@app.route("/preview/<app_name>")
def preview_image(app_name: str):
    """Serve the preview.gif/png/jpg from a given app's assets/. Restricted
    to apps in APPS_REPO/apps so we don't act as an open file server."""
    safe = re.sub(r"[^a-zA-Z0-9._-]", "", app_name)
    if not safe or safe != app_name:
        return ("bad app name", 400)
    app_dir = APPS_REPO / "apps" / safe / "assets"
    for ext, mime in (("gif", "image/gif"), ("png", "image/png"), ("jpg", "image/jpeg"), ("jpeg", "image/jpeg")):
        candidate = app_dir / f"preview.{ext}"
        if candidate.exists():
            return candidate.read_bytes(), 200, {"Content-Type": mime, "Cache-Control": "public, max-age=300"}
    return ("no preview", 404)


@app.route("/api/init", methods=["POST"])
def api_init():
    body = request.get_json(force=True)
    name = (body.get("name") or "").strip()
    ssid = (body.get("ssid") or "").strip()
    pw = body.get("pw") or ""
    port = (body.get("port") or "").strip() or None
    # Sentinel: "use the password we already have on disk from a prior init"
    if pw == "__saved__":
        saved = get_saved_password()
        if not saved:
            return jsonify({"error": "no saved password on disk yet — type one in"}), 400
        pw = saved
    if not name or not ssid or not pw:
        return jsonify({"error": "name, ssid, pw required"}), 400

    job_id = uuid.uuid4().hex[:8]
    with JOBS_LOCK:
        JOBS[job_id] = {
            "status": "running",
            "kind": "init",
            "name": name,
            "started": time.time(),
            "log": "",
        }
    threading.Thread(
        target=run_init_job, args=(job_id, name, ssid, pw, port), daemon=True
    ).start()
    return jsonify({"job_id": job_id})


@app.route("/api/devices", methods=["GET", "POST"])
def api_devices():
    if request.method == "GET":
        return jsonify(load_devices())
    body = request.get_json(force=True)
    name = (body.get("name") or "").strip()
    kind = (body.get("kind") or "cube").strip()
    ip = (body.get("ip") or "").strip()
    num = int(body.get("num") or 0)
    if kind not in ("cube", "emulator"):
        return jsonify({"error": "kind must be 'cube' or 'emulator'"}), 400
    if not name or num < 1:
        return jsonify({"error": "name and num (>=1) required"}), 400
    if kind == "cube" and not ip:
        return jsonify({"error": "ip required for cube"}), 400
    if kind == "emulator":
        ip = "emulator"  # placeholder so existing UI/JSON consumers still see a value
    devices = load_devices()
    devices = [d for d in devices if d["name"] != name]
    record = {"name": name, "ip": ip, "num": num}
    if kind == "emulator":
        record["kind"] = "emulator"
    devices.append(record)
    devices.sort(key=lambda d: d["num"])
    save_devices(devices)
    return jsonify({"ok": True, "devices": devices})


@app.route("/api/devices/<name>", methods=["DELETE"])
def api_delete_device(name: str):
    stop_emulator(name)  # no-op if it wasn't an emulator or wasn't running
    devices = [d for d in load_devices() if d["name"] != name]
    save_devices(devices)
    return jsonify({"ok": True})


@app.route("/api/devices/<name>/edit", methods=["POST"])
def api_edit_device(name: str):
    """Edit a device's name, number, and/or IP. All three fields are optional
    in the body — missing/empty fields are left unchanged. Migrates the
    live-emulator bookkeeping on rename: any running SDL2 process keyed by
    the old name is re-keyed under the new name, and the on-disk frame
    dump file moves so the Simulator tab keeps showing the same window."""
    body = request.get_json(force=True)
    devices = load_devices()
    current = next((d for d in devices if d["name"] == name), None)
    if not current:
        return jsonify({"error": f"no such device '{name}'"}), 404

    new_name = (body.get("name") or "").strip() or name
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", new_name):
        return jsonify({"error": "name must be alphanumeric / _ / -"}), 400
    if new_name != name and any(d["name"] == new_name for d in devices):
        return jsonify({"error": f"a device named '{new_name}' already exists"}), 400

    new_num = current["num"]
    if body.get("num") not in (None, ""):
        try:
            new_num = int(body.get("num"))
        except (TypeError, ValueError):
            return jsonify({"error": "num must be an integer"}), 400
        if new_num < 1:
            return jsonify({"error": "num must be >= 1"}), 400

    # IP is meaningless for emulator slots — keep their placeholder so the
    # rest of the UI doesn't have to special-case missing values.
    is_emulator = current.get("kind") == "emulator"
    if is_emulator:
        new_ip = "emulator"
    else:
        new_ip = (body.get("ip") or "").strip() or current["ip"]
        if not new_ip:
            return jsonify({"error": "ip required for cube"}), 400

    if new_name != name:
        with EMULATOR_LOCK:
            proc = EMULATOR_PROCS.pop(name, None)
            if proc is not None:
                EMULATOR_PROCS[new_name] = proc
        old_frame = FRAME_DIR / f"{name}.png"
        new_frame = FRAME_DIR / f"{new_name}.png"
        if old_frame.exists():
            try:
                old_frame.rename(new_frame)
            except OSError:
                pass

    for d in devices:
        if d["name"] == name:
            d["name"] = new_name
            d["num"] = new_num
            d["ip"] = new_ip
            break

    devices.sort(key=lambda d: d["num"])
    save_devices(devices)
    return jsonify({"ok": True, "devices": devices})


@app.route("/api/emulator/<name>/stop", methods=["POST"])
def api_emulator_stop(name: str):
    stopped = stop_emulator(name)
    return jsonify({"ok": True, "stopped": stopped})


def _probe_cube(ip: str, timeout: float = 1.5) -> tuple[bool, int | None]:
    """TCP-connect to port 80 on the cube's IP. Returns (online, latency_ms).
    Used by the devices-status endpoint to render green/red indicators in
    the webui — much cheaper than an HTTP request and fast enough to do for
    every registered cube on every page poll."""
    t0 = time.monotonic()
    try:
        with socket.create_connection((ip, 80), timeout=timeout):
            pass
        return True, int((time.monotonic() - t0) * 1000)
    except (OSError, socket.timeout):
        return False, None


@app.route("/api/devices/status")
def api_devices_status():
    """Live online/offline for every registered device. Cubes are probed in
    parallel via TCP-connect to :80 with a short timeout so the whole table
    refreshes in well under 2 s even with several unreachable cubes.
    Emulator slots report online based on whether their SDL2 process is
    currently alive."""
    devices = load_devices()
    cubes = [d for d in devices if d.get("kind") != "emulator"]
    out: dict[str, dict] = {}
    if cubes:
        with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, len(cubes))) as ex:
            futs = {ex.submit(_probe_cube, d["ip"]): d for d in cubes}
            for fut in concurrent.futures.as_completed(futs):
                d = futs[fut]
                online, latency_ms = fut.result()
                out[d["name"]] = {"online": online, "latency_ms": latency_ms}
    for d in devices:
        if d.get("kind") == "emulator":
            out[d["name"]] = {
                "online": emulator_running(d["name"]),
                "latency_ms": None,
            }
    # Preserve the device list ordering so the UI maps row-by-row easily.
    return jsonify([{"name": d["name"], **out.get(d["name"], {})} for d in devices])


@app.route("/api/emulator/<name>/frame.png")
def api_emulator_frame(name: str):
    """Serve the most recently dumped emulator frame for `name`. Returns 404
    until the first frame has been written (during build, or if the emulator
    isn't running). The browser cache-busts via a `?t=…` query param."""
    safe = re.sub(r"[^a-zA-Z0-9._-]", "", name)
    if not safe or safe != name:
        return ("bad device name", 400)
    path = FRAME_DIR / f"{safe}.png"
    if not path.exists():
        return ("no frame yet", 404)
    return path.read_bytes(), 200, {
        "Content-Type": "image/png",
        # Frame changes ~5 fps; tell every cache to leave it alone
        "Cache-Control": "no-store, no-cache, must-revalidate, max-age=0",
        "Pragma": "no-cache",
    }


@app.route("/api/emulator/status")
def api_emulator_status():
    """List every emulator slot with its running state and frame freshness.
    The Simulator tab uses this to pick a default device + show staleness."""
    out = []
    for d in load_devices():
        if d.get("kind") != "emulator":
            continue
        path = FRAME_DIR / f"{d['name']}.png"
        out.append({
            "name": d["name"],
            "num": d["num"],
            "running": emulator_running(d["name"]),
            "frame_age_s": (time.time() - path.stat().st_mtime) if path.exists() else None,
            "last_app": d.get("last_app"),
        })
    return jsonify(out)


@app.route("/api/deploy", methods=["POST"])
def api_deploy():
    body = request.get_json(force=True)
    app_name = (body.get("app") or "").strip()
    device_name = (body.get("device") or "").strip()
    if not app_name or not device_name:
        return jsonify({"error": "app and device required"}), 400

    devices = load_devices()
    device = next((d for d in devices if d["name"] == device_name), None)
    if not device:
        return jsonify({"error": f"unknown device '{device_name}'"}), 404

    is_emulator = device.get("kind") == "emulator"
    usb = bool(body.get("usb"))
    port = (body.get("port") or "").strip() or None
    if usb and is_emulator:
        return jsonify({"error": "USB mode doesn't apply to emulator slots"}), 400

    job_id = uuid.uuid4().hex[:8]
    with JOBS_LOCK:
        JOBS[job_id] = {
            "status": "running",
            "app": app_name,
            "device": device_name,
            "kind": "emulator" if is_emulator else ("cube-usb" if usb else "cube-ota"),
            "started": time.time(),
            "log": "",
        }
    if is_emulator:
        threading.Thread(
            target=run_emulate_job, args=(job_id, app_name, device), daemon=True
        ).start()
    else:
        threading.Thread(
            target=run_deploy_job, args=(job_id, app_name, device, usb, port), daemon=True
        ).start()
    return jsonify({"job_id": job_id})


@app.route("/api/jobs/<job_id>")
def api_job(job_id: str):
    with JOBS_LOCK:
        job = JOBS.get(job_id)
    if not job:
        return jsonify({"error": "no such job"}), 404
    return jsonify(job)


if __name__ == "__main__":
    port = int(os.environ.get("KUBLET_WEBUI_PORT", "1666"))
    app.run(host="0.0.0.0", port=port, debug=False)
