# HANDOVER — Kublet Silver fleet project

Read this first when you (or any future agent) sits down at this directory
cold. It is the single document that explains the *current state*, what's
real, what's queued, and where everything lives.

Project root: `/Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet/`

---

## 1. The thirty-second pitch

The Kublet cube is a small ESP32 + 240×240 ST7789 LCD desk display that was
funded on Kickstarter and abandoned by its company in August 2024. The owner
has ~10 of these cubes and wants a self-hosted toolchain to keep them useful.

Phase 1 (this project) ships:

- A **silver-price firmware app** that displays the live silver spot price
  (USD per troy ounce) refreshing every minute, with the cube's individual
  number `#N` in the upper-right corner. Built, compiles to 1039 KB, verified
  in the desktop emulator.
- A **Flask web UI on port 1666** to register cubes, build any app with that
  cube's `KUBLET_NUM` baked in, and push it over WiFi-OTA. Also handles the
  one-time USB Mini-B flash with a serial-port diagnostic.
- A **shareable installer** (`install.sh` + `make-bundle.sh`) that lets any
  Mac (or Linux) user set up the whole stack from a 20 KB tarball.
- A **source archive** of everything we recovered from the dead Kublet
  ecosystem (5 official repos, the active community fork, key Wayback
  Machine pages of the offline developer portal).

What's still blocked on hardware: actually flashing the first cube. The cube
showed "open mobile app" (factory firmware) — we need a working USB Mini-B
cable + SiLabs CP210x driver before `tools/dev init` will see it.

---

## 2. Directory layout

```
9-Kublet/
├── HANDOVER.md            ← this file
├── HANDOFF.md             ← shorter "what's done / what needs hardware" note
├── HOWTO.md               ← first-time user guide (shareable)
├── install.sh             ← one-shot bootstrap (idempotent)
├── make-bundle.sh         ← packages a shareable .tar.gz
│
├── apps/                  ← apps in the bundle (currently just silver/)
│   └── silver/            (this gets copied into kublet-apps/apps/ by install.sh)
│
├── patches/               ← drop-in fixes for upstream files
│   └── kublet-apps/tools/src/
│       ├── emulate/CMakeLists.txt        SDL2::SDL2 imported-target fix
│       └── kublet_dev/flash.py           non-interactive init + venv-friendly
│
├── webui/                 ← Flask app, port 1666
│   ├── app.py
│   ├── templates/index.html
│   ├── static/style.css
│   ├── devices.json       ← persistent device registry (mount as volume in Docker)
│   ├── requirements.txt
│   ├── Dockerfile
│   └── README.md
│
├── kublet-apps/           ← clone of github.com/markusos/kublet-apps (the active fork)
│   ├── apps/              27 apps incl. our silver
│   ├── tools/dev          Python CLI (build, init, deploy, logs, devices)
│   ├── tools/emulate      SDL2 desktop emulator
│   ├── kublet_env/        Python venv (Dropbox-ignored; pio, esptool, pyserial, flask)
│   └── ...                lib/KGFX, lib/OTAServer, firmware/, server/
│
└── source_archive/        ← everything we recovered from the dead ecosystem
    ├── NOTES.md
    ├── kublet-official/   all 5 github.com/kublet/* repos, cloned 2026-05-15
    ├── markusos-fork-snapshot/  frozen at commit a2ad62c
    └── wayback-developers-portal/  4 highest-value pages from web.archive.org
```

Dropbox-ignored (won't sync, are big): `kublet-apps/.git`,
`kublet-apps/kublet_env`, every `kublet-apps/apps/*/.pio`,
`source_archive/*/.git`. Marked via `xattr -w com.dropbox.ignored 1`.

---

## 3. Current state — what works

| Component | Status |
|---|---|
| Silver firmware build | ✅ Builds clean (1039 KB / 80 % flash) via `python tools/dev build silver` |
| Silver in desktop emulator | ✅ Renders correctly, screenshot at `/tmp/silver_screenshot.png` |
| Webui on port 1666 | ✅ Running. All endpoints respond. See section 5. |
| Webui Flash Me form | ✅ End-to-end through to USB-detect step (fails gracefully without hardware) |
| Webui Apps Catalog | ✅ All 27 apps, 20 with preview thumbnails (preview.gif from each app's `assets/`) |
| Webui USB diagnostic | ✅ `/api/diagnostics` reports serial ports + `system_profiler` CP210x match |
| Saved WiFi creds reload | ✅ Webui reads `kublet-apps/tools/.env`, pre-fills SSID, pw-saved hint |
| Last-deployed-app tracking | ✅ `run_deploy_job` writes `last_app` + `last_deployed_at` into `devices.json` |
| Installer (`install.sh`) | ✅ Tested syntax-only. Idempotent, applies `patches/` to cloned tree. |
| Bundle (`make-bundle.sh`) | ✅ Produces ~20 KB shareable tarball |
| Source archive | ✅ All 5 official repos + markusos snapshot + 4 wayback pages, ~2.5 MB |

What is *not* done:
- Actual hardware flash of a real kublet (user blocked on USB Mini-B cable / CP210x driver)
- Phase 2 apps: gold, currencies, crypto, weather
- NVS-stored kublet number (currently baked in at build time via `-DKUBLET_NUM=N`)
- iOS / Android companion apps — **deferred by user**

---

## 4. What you (or future agent) likely got asked to do next

Resume points, in approximate priority:

1. **The user just plugged in a working cable and the cube appears at `/dev/cu.usbserial-XXXX`.**
   They want to flash kublet1 with WiFi creds and deploy silver.
   - From terminal: `cd kublet-apps && source kublet_env/bin/activate && python tools/dev init --name kublet1`
   - From webui: just fill the Flash Me form. Saved creds (if any) are pre-filled.
   - After init succeeds, `python tools/dev deploy silver kublet1` or use the Deploy section.

2. **Add a new price-style app** (gold, currencies, crypto).
   - Copy `kublet-apps/apps/silver/` to `apps/<name>/`.
   - Replace the Yahoo URL in `fetchSilverPrice()` with the relevant endpoint.
   - Update `manifest.yaml` summary, header text, unit label.
   - Build with `python tools/dev build <name>`, test in emulator with `./tools/emulate <name>`.
   - Add an `assets/http_fixtures.json` + sample response for emulator testing.

3. **Add a weather app** (more involved — multiple metrics).
   - The community fork already has `apps/weather/` — start by reading what it does.
   - User wants: temperature, pressure, rain forecast, sun, moon phases.
   - Free data source candidates: Open-Meteo (no auth), OpenWeatherMap (free tier with key).

4. **Persist kublet number to NVS** so the same firmware binary works on every
   cube without per-device rebuild.
   - Read `KUBLET_NUM` from NVS via `Preferences.h` namespace `app`, key `num`.
   - Have `tools/dev init` write the number to NVS alongside ssid/pw (in
     `generate_nvs` — see `flash.py:108`).
   - Webui then doesn't need `PLATFORMIO_BUILD_FLAGS=-DKUBLET_NUM=N` anymore;
     the same prebuilt firmware deploys to any cube.

5. **Yahoo Finance starts rate-limiting silver fetches.**
   - Swap to Stooq CSV: `https://stooq.com/q/l/?s=xagusd&f=sd2t2ohlc&h&e=csv`.
   - `fetchSilverPrice()` in `apps/silver/src/main.cpp` is isolated; ~10 lines.

---

## 5. How to operate the webui

### Run locally

```bash
cd /Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet/webui
/Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet/kublet-apps/kublet_env/bin/python app.py
# → http://localhost:1666
```

### Run in Docker

```bash
cd /Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet
docker build -t kublet-fleet -f webui/Dockerfile .
docker run -d --restart unless-stopped --network host \
    -v "$PWD/webui/devices.json:/srv/webui/devices.json" \
    kublet-fleet
```

`--network host` lets the container reach kublets on the LAN. For USB
flashing through Docker, also pass `--device /dev/cu.usbserial-XXXX:/dev/ttyUSB0`.

### Endpoints

| Method | Path | Body | What it does |
|---|---|---|---|
| GET | `/` | — | Render the page |
| GET | `/api/devices` | — | List registered cubes |
| POST | `/api/devices` | `{name, ip, num}` | Register/replace a cube |
| DELETE | `/api/devices/<name>` | — | Remove a cube |
| GET | `/api/serial-ports` | — | List `/dev/cu.usbserial-*` etc. |
| GET | `/api/diagnostics` | — | Serial ports + `system_profiler` CP210x match |
| POST | `/api/init` | `{name, ssid, pw, port?}` | Flash dev firmware + WiFi over USB. `pw="__saved__"` reuses on-disk pw. |
| POST | `/api/deploy` | `{app, device}` | Build firmware with `-DKUBLET_NUM=N`, OTA push, mark `last_app` on success |
| GET | `/api/jobs/<id>` | — | Status + streamed log of a job |
| GET | `/preview/<app>` | — | Serve `apps/<app>/assets/preview.{gif,png,jpg}` |

### Jobs

Both `/api/init` and `/api/deploy` run subprocesses asynchronously and
return a `job_id`. Poll `/api/jobs/<id>` once per second; the page already
does this. `status` is `running` → `ok` or `failed`.

---

## 6. Three local patches to know about

`patches/kublet-apps/tools/src/kublet_dev/flash.py` overlays the upstream
file with these changes:

1. **`get_wifi_credentials()`** also reads `KUBLET_SSID` / `KUBLET_PW` from
   `os.environ`, and skips the `[Y/n]` confirmation prompt when
   `KUBLET_NONINTERACTIVE=1`. Required for webui to invoke `init`
   without a terminal.
2. **`find_nvs_gen()`** first checks the pip-installed
   `esp_idf_nvs_partition_gen` package (the one we install in
   `kublet_env/`), then falls back to the PlatformIO-bundled copy.
3. **Removed the `["uv", "run", ...]` wrappers** around `nvs_partition_gen.py`
   and `esptool.py`. Required because the user's `~/.config/uv/uv.toml`
   contains `exclude-newer = "7 days"` which is invalid syntax, making
   every `uv` invocation fail.

`patches/kublet-apps/tools/src/emulate/CMakeLists.txt` overlays with:

4. **SDL2::SDL2 imported-target fix** — when pkg-config finds SDL2 (which
   it does on Apple Silicon brew), it didn't create the
   `SDL2::SDL2` IMPORTED target the file then tries to link against.
   Patch adds `IMPORTED_TARGET` and creates an alias.

Both files are applied by `install.sh` after cloning. They're NOT
upstreamed.

---

## 7. Hardware reference (copied from source_archive/NOTES.md for convenience)

ESP32 + ST7789 240×240 LCD. No other peripherals.

| Function | GPIO |
|---|---|
| Button | 19 (active LOW, INPUT_PULLUP) |
| Backlight (PWM, ledc channel 0) | 15 |
| TFT_MOSI | 23 |
| TFT_SCLK | 18 |
| TFT_CS | 5 |
| TFT_DC | 2 |
| TFT_RST | 4 |
| TFT_MISO | -1 (not connected) |

USB port on the cube housing: **USB Mini-B** (trapezoid), Silicon Labs
CP210x USB-serial chip. Mac must have the SiLabs CP210x VCP driver
installed; without it the cube powers up but no `/dev/cu.*` node appears.

Persistence: WiFi creds in NVS namespace `core` keys `ssid`/`pw`. Survives
power cycles and firmware swaps.

OTA: cube exposes `POST /update` on port 80, multipart field `filedata`,
**no auth**. Anyone on the LAN can flash anything. Put kublets on a VLAN
if you care.

Boot sequence (~3 seconds): power on → flash boot → app firmware reads
WiFi creds from NVS → joins WiFi → silver fetches price → screen updates.
No manual intervention ever needed once flashed.

---

## 8. Operating the dev tool (terminal)

```bash
cd /Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet/kublet-apps
source kublet_env/bin/activate   # MUST source — find_pio() uses shutil.which("pio")

python tools/dev devices                     # list known cubes (.devices.yml)
python tools/dev init --name <name>          # USB flash WiFi creds, register
python tools/dev build <app>                 # compile firmware
python tools/dev deploy <app> <name>         # build + OTA push
python tools/dev deploy <app> --ip 192.168.1.50   # OTA push to explicit IP
python tools/dev logs                        # stream serial logs over USB

./tools/emulate <app> --screenshot /tmp/x.png --after 3
./tools/emulate <app> --gif /tmp/x.gif --gif-duration 5
./tools/emulate <app>                        # interactive SDL window
```

Note: `./tools/dev` directly is `#!/usr/bin/env -S uv run --script`. The
user's `uv.toml` is broken, so don't invoke it that way — always use the
venv-activated `python tools/dev ...` form.

---

## 9. Building & sharing

```bash
cd /Users/schlaptop/Dropbox/0_multiplatform_Install/0_Codeing_Vorgabe/9-Kublet
./make-bundle.sh
# → kublet-silver-bundle.tar.gz (~25 KB now with patches + handover)
```

Recipient (on any Mac with Homebrew):

```bash
tar -xzf kublet-silver-bundle.tar.gz
cd kublet-silver-bundle
./install.sh
# Then follow HOWTO.md.
```

---

## 10. Open issues & known quirks

- **The 1-second clock tick redraw flickers slightly** because we re-fill
  a small rect. Acceptable but could be smoothed with a sprite. Low priority.
- **Yahoo Finance silver futures (SI=F)** tracks spot to within a few cents.
  If you want true spot, swap to Stooq's `xagusd`.
- **The dev tool's `tools/.env`** stores WiFi password in plaintext, gitignored.
  Webui leverages this for re-fill but the file is on disk. Fine on personal
  Macs, not great on shared machines.
- **No CI / tests for our silver app code** — markusos has pytest infra at
  `tools/tests/` but we haven't added a silver smoke test there yet. Easy
  follow-up.
- **iOS / Android apps** were originally Phase-2 scope; user **deferred** them.

---

## 11. Memory files (for Claude Code agents)

Persistent context lives at `/Users/schlaptop/.claude/projects/-Users-schlaptop/memory/`:

- `MEMORY.md` — index
- `project_kublet_silver_app.md` — Phase 1 + Phase 2 scope, user owns ~10 cubes
- `reference_kublet_fork_tooling.md` — how to build/deploy, venv quirks, the
  three flash.py patches and *why* they're needed, OTA protocol spec

Future-you: read these before doing anything in this directory.
