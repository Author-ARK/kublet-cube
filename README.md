# Kublet-Asu

Custom firmware, a Flask fleet web-UI, and a growing app catalog for
the [Kublet cube](https://thekublet.com) — the small ESP32 desk display
whose vendor went silent in mid-2024. This repository keeps the hardware
useful: it adds OTA-based deploys, a desktop SDL2 emulator, a USB-fallback
flasher, and a set of finance/weather/clock apps tuned to the author's
home base in Asunción, Paraguay.

The dev-firmware, OTA listener, build wrapper, and SDL2 emulator are all
descended from the open-source community fork by
[@markusos](https://github.com/markusos/kublet-apps) and ship under the
Apache 2.0 license. Apps and the fleet web-UI in this repository are new.

> Copyright © A.R.K · [schlaptop@protonmail.com](mailto:schlaptop@protonmail.com)
> Licensed under the Apache License, Version 2.0 — see [LICENSE](LICENSE)
> and [NOTICE](NOTICE).

---

## A note from the author

This is my first public project. I wanted to update my Kublet and
found out that my Android app only had a few apps working — and that
Kublet itself is no more. So I went searching on GitHub and turned up
a few people keeping the cubes alive.

From there I started letting Claude help me build this little
web-server app so I could push whatever app I want onto my cubes, and
I made a few of my own apps with Claude along the way. Run
`install.sh` and enjoy — please check the code first and then apply
those apps to your Kublet, or change them however you like (with
Claude or any other tool / idea) and OTA them onto your cubes.

It's been fun. Maybe it helps you too.

— A.R.K.

---

## What's here

```
.
├── HOWTO.md                ← step-by-step install + first-cube setup
├── HANDOFF.md              ← state-of-the-world for hand-overs
├── install.sh              ← one-shot bootstrap (brew + venv + pio + smoke build)
├── kublet.sh               ← start/stop/status/logs wrapper for the webui
├── kublet-apps/            ← cloned community fork + our apps under apps/
│   ├── apps/               ← per-app firmware (see catalog below)
│   ├── firmware/           ← dev_firmware.bin, partitions.bin (binary backups)
│   ├── lib/                ← KGFX + OTAServer libraries
│   └── tools/              ← `dev` CLI and the SDL2 emulator
├── webui/                  ← Flask fleet-manager (port 1666)
└── source_archive/         ← upstream snapshots + Wayback-Machine docs
```

## Quick start

```bash
git clone https://github.com/<your-fork>/kublet-asu.git
cd kublet-asu
bash install.sh                # brew, venv, pio toolchain, smoke build
./kublet.sh start              # webui on http://localhost:1666
```

First time per cube:

1. Plug it into a USB-Mini-B data cable.
2. Open the **⚡ Flash me** tab in the webui → enter SSID + WiFi password → flash.
3. Once the cube reports its LAN IP in the serial log, it shows up in
   the **📡 Devices** table and you can deploy any app via 🚀 Deploy
   (OTA over WiFi) or fall back to 🔌 USB if WiFi is flaky.

## Documentation

| Doc                                  | What's in it                                                            |
| ------------------------------------ | ----------------------------------------------------------------------- |
| [HOWTO.md](HOWTO.md)                 | First-time install + getting a brand-new cube on your network          |
| [DEVELOPING.md](DEVELOPING.md)       | Day-to-day workflow: server, emulator, deploy, adding an app, troubleshooting |
| [CONTRIBUTING.md](CONTRIBUTING.md)   | Issue / PR conventions                                                  |
| [NOTICE](NOTICE)                     | Upstream attributions for every library and API the project uses        |
| [LICENSE](LICENSE)                   | Apache License 2.0 (full text)                                          |

## Apps catalog

All apps here render at 240×240. Most poll a public API on a 1-minute
or 15-minute cadence; the cube's bundled OTA listener accepts further
deploys at port 80.

### Solo finance apps (single-asset)

| App            | What it shows                                                   | Data source     |
| -------------- | --------------------------------------------------------------- | --------------- |
| `Crypto-BTC`   | Bitcoin spot · 1h chart · 1h delta % · 1-min direction tint     | Binance klines  |
| `Crypto-ETH`   | Ethereum · same layout                                          | Binance klines  |
| `Crypto-XRP`   | XRP · same                                                      | Binance klines  |
| `Crypto-SOL`   | Solana · same                                                   | Binance klines  |
| `Metal-Au`     | Gold spot · same layout, 60-min in-RAM ring buffer for chart    | Stooq CSV       |
| `Metal-Ag`     | Silver spot · same                                              | Stooq CSV       |

### Multi-asset boards

| App              | Layout                                                              | Source        |
| ---------------- | ------------------------------------------------------------------- | ------------- |
| `Precious-Metal` | Au + Ag split-screen with 1-min direction arrows                    | Stooq CSV     |
| `cryptoBTC_ETH`  | BTC + ETH split-screen                                              | Binance ticker|
| `cryptoXRP_SOL`  | XRP + SOL split-screen                                              | Binance ticker|
| `Crypto_XLM_BAR` | XLM + BAR split-screen                                              | Binance ticker|
| `Dex-Tokens`     | Up to 4 Dexscreener tokens; button pages between pairs              | Dexscreener   |
| `6-crypto`       | Six-row board: BTC ETH XRP SOL XLM HBAR with 1h % and tick arrows   | Binance batch |

### Clocks & weather

| App             | What it shows                                                    |
| --------------- | ---------------------------------------------------------------- |
| `World-Times`   | 5 zones at once (ASU/TPA/FRA/COL/UTC) with ±1d when dates differ |
| `Digital-Clock` | Single huge clock that rotates ASU/FRA/COL every 30 s            |
| `Asu-Weather`   | Asunción weather, sunrise + sunset, moonrise + moonset, 3-day    |

## Fleet web-UI ([webui/](webui/))

Flask app on port 1666 with tabbed nav:

- **⚡ Flash me** — first-time USB flash + WiFi credential burn
- **📡 Devices** — per-cube name / number / IP, live online dot, edit & remove
- **🚀 Deploy** — build + OTA-push (or 🔌 USB fallback) to a registered cube
- **🖥 Simulator** — live SDL2 emulator framebuffer streamed into the browser
- **📱 Apps catalog** — auto-rendered preview tiles, sort by last-added / A→Z / Z→A
- **ℹ About** — credits, hardware specs, recovered documentation

The webui process is managed with `./kublet.sh start | stop | restart | status | logs`.

## Emulator

The SDL2 desktop emulator under [`kublet-apps/tools/src/emulate/`](kublet-apps/tools/src/emulate/)
builds any app for the host so you can iterate on layout without
re-flashing the cube. The fleet web-UI exposes it as a 💻 device kind
("sim slot") and the live framebuffer is streamed into the **🖥 Simulator**
tab via atomically-rewritten frame.png files.

```bash
cd kublet-apps && source kublet_env/bin/activate
./tools/emulate Crypto-BTC                       # interactive SDL window
./tools/emulate Metal-Au --screenshot out.png    # headless, single capture
```

## Credits

This project depends heavily on the work of others. The full list and
its license attribution lives in [NOTICE](NOTICE); a few that earn
particular thanks:

- **Markus Östberg** — author of the [community fork](https://github.com/markusos/kublet-apps)
  whose dev firmware, OTA listener, and SDL2 emulator make any of this
  possible after the original vendor's developer portal went dark.
- **bodmer/TFT_eSPI**, **bblanchon/ArduinoJson**, **nothings/stb** — load-bearing libraries.
- **PlatformIO** & the **ESP32 Arduino core** — the toolchain.
- **Open-Meteo**, **MET Norway**, **Binance**, **Stooq**, **Dexscreener** — public
  APIs the apps poll at runtime (no source incorporated; rate-limit-friendly use).

If you spot code in this repo descended from a project not listed in
[NOTICE](NOTICE), please open an issue and we'll add the attribution.

## License

Apache License, Version 2.0. See [LICENSE](LICENSE) and the per-component
attributions in [NOTICE](NOTICE).
