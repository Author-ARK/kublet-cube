# Kublet Source Archive

Snapshot of everything we recovered after Kublet Inc. went silent in
August 2024. Goal: be able to rebuild this whole stack from the contents
of this folder even if every external source disappears tomorrow.

## What's in here

```
source_archive/
├── NOTES.md                            ← this file
├── kublet-official/                    ← all 5 repos from github.com/kublet, cloned 2026-05-15
│   ├── community/                      ← app-store template, ~10 stars (the original "fork-and-add-apps" repo)
│   ├── krate/                          ← Go CLI for build/flash/deploy (depends on infrastructure that died)
│   ├── kgfx/                           ← Graphics library wrapping TFT_eSPI; chart sprites, fonts
│   ├── otaserver/                      ← C++ OTA server library (HTTP POST /update on port 80, no auth)
│   └── homebrew-tools/                 ← brew tap for installing krate
├── markusos-fork-snapshot/             ← active community fork at commit a2ad62c (2026-05-15)
│   └── SNAPSHOT_COMMIT.txt             ← exact commit + date stamped
└── wayback-developers-portal/          ← HTML scraped from Internet Archive
    ├── installation.html               ← krate install (brew tap kublet/tools && brew install krate)
    ├── apps-server.html                ← documented HTTP /update endpoint spec
    ├── http-client.html                ← canonical HTTPClient + ArduinoJson example
    └── display-api.html                ← full KGFX API reference (more complete than GitHub README)
```

## Status of every external source

| Source | Status | Mirror |
|---|---|---|
| github.com/kublet/* (5 repos) | **Frozen** since Aug 2024 — no commits, no responses to issues. | `kublet-official/` |
| developers.thekublet.com | **Offline** (connection refused). | `wayback-developers-portal/` (4 most-useful pages; full 29-page set archived at web.archive.org) |
| thekublet.com | Marketing site only, no dev resources. | (not mirrored) |
| github.com/markusos/kublet-apps | **Active** — only meaningful fork. 26 apps + Python dev tooling + SDL2 emulator. | `markusos-fork-snapshot/` |
| Other forks of kublet/* | All zero commits ahead of upstream. Nothing to mirror. | (none) |
| iOS companion app (App Store ID 6476105211) | Live but functionally degraded. Only used for first-time WiFi setup; markusos fork bypasses it. | (Apple-binary, not mirrored) |
| `bordeapi.com` | **Was a community typo** — meant `boredapi.com`, a third-party "bored activity" API used by one example app. NOT a Kublet-owned backend. The OTA system is fully on-device, no server backend exists. | n/a |

## Recovered hardware reference (authoritative)

ESP32 + ST7789 240x240 LCD. No other peripherals: no IMU, no temperature
sensor, no microphone — just display + button. Pin map (from
`markusos-fork-snapshot/tools/src/kublet_dev/config.py`):

| Function | GPIO |
|---|---|
| Button | 19 (active LOW, INPUT_PULLUP) |
| Backlight (PWM via ledc) | 15 |
| TFT_MOSI | 23 |
| TFT_SCLK | 18 |
| TFT_CS | 5 |
| TFT_DC | 2 |
| TFT_RST | 4 |
| TFT_MISO | -1 (not connected) |

SPI bus at 40 MHz. Driver: `ST7789_DRIVER`, RGB order `TFT_BGR`.

Flash parameters: `--chip esp32 --flash_mode dio --flash_size detect
--flash_freq 40m --baud 460800`. Partition layout:

| Offset | Contents |
|---|---|
| 0x1000 | bootloader.bin |
| 0x8000 | partitions.bin |
| 0x9000 | NVS (WiFi creds + app config) |
| 0x10000 | firmware.bin (the app) |
| 0xE000 | OTA data (zero this on first init) |

NVS namespace `core` holds keys `ssid` (string) and `pw` (string).

## OTA protocol (recovered from otaserver source)

The "dev firmware" markusos provides is just a bare-bones HTTP server
that listens on port 80 of the cube's LAN IP. To update:

```
POST /update HTTP/1.1
Host: <cube-ip>
Content-Type: multipart/form-data; boundary=----BOUNDARY

------BOUNDARY
Content-Disposition: form-data; name="filedata"; filename="firmware.bin"
Content-Type: application/octet-stream

<binary firmware bytes>
------BOUNDARY--
```

**No authentication. No encryption.** Anyone on the LAN with the IP can
flash anything. If you care, put the cube on a VLAN.

Device reboots automatically into the new firmware. The whole upload +
reboot takes ~30 seconds for a typical 1 MB image.

## Apps inventory (markusos fork, 27 apps incl. our `silver`)

| App | What it does |
|---|---|
| aquarium | Animated aquarium |
| astro | Astro / space display |
| badgers | Badger animation (the "badgers badgers badgers" meme) |
| bored | Random activity from boredapi.com |
| boredd | Variant of bored |
| claude-usage | Anthropic Claude API usage monitor |
| clock | Clock face |
| color | Random color display |
| dvd | DVD-screensaver bouncing logo |
| hello | Hello-world template |
| hn | Hacker News headlines |
| icinga | Icinga host + service status (sysadmin dashboard) |
| lava | Lava-lamp animation |
| life | Conway's Game of Life |
| matrix | Matrix-style falling characters |
| maze | Maze rendering |
| music | Now-playing music display |
| notice | Push notification display |
| pomodoro | Pomodoro timer |
| **silver** | **Silver spot price (USD/oz) — ours** |
| snake | Snake game (button-controlled) |
| speed | Speedometer / network speedtest |
| stock | S&P 500 / VOO intraday chart |
| this-is-fine | "This is fine" meme animation |
| time | Time / clock variant |
| weather | Weather forecast |
| yule | Yule log animation |

To install any app from the source archive: copy `apps/<name>/` from
`markusos-fork-snapshot/apps/<name>/` into `kublet-apps/apps/`, then
`./tools/dev deploy <name> <kublet-name>` (or use the webui).

## How to rebuild from this archive (zero external dependencies)

If markusos goes dark *and* GitHub goes dark:

```bash
# 1. Restore the dev tool
cp -R source_archive/markusos-fork-snapshot ~/restored-kublet-apps
cd ~/restored-kublet-apps

# 2. Install Python deps (still need pip / PyPI for these)
python3 -m venv kublet_env && source kublet_env/bin/activate
pip install -e . platformio flask

# 3. From here, the standard install.sh / HOWTO.md flow works as
#    documented — apps/, tools/dev, tools/emulate are all in the
#    snapshot.
```

The five `kublet-official/` repos are **not strictly needed** for normal
operation — markusos forked the relevant pieces (kgfx, otaserver) into
its own `lib/` tree. They're kept here for historical reference and in
case anyone wants to verify the OTA protocol against the original C++
source.
