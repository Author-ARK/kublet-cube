# Kublet Device — Technical Reference

Documentation on the Kublet hardware, firmware, Bluetooth OTA protocol, and the process for deploying apps without the (now-defunct) iOS app.

> **Disclaimer:** This is a community best-effort document. The Kublet company has shut down and there is no official technical documentation available. Everything here was reverse-engineered or gathered from community sources (Discord, the open-source `krate` CLI, etc.). Some behavior may be incorrect, incomplete, or vary across hardware revisions. This project has no affiliation with the original manufacturer.

---

## Table of Contents

- [Hardware](#hardware)
- [Display (TFT_eSPI)](#display-tft_espi)
- [Flash Layout](#flash-layout)
- [Firmware Types](#firmware-types)
- [Bluetooth Low Energy (BLE)](#bluetooth-low-energy-ble)
  - [BLE Services & Characteristics](#ble-services--characteristics)
  - [BLE Config Protocol](#ble-config-protocol)
  - [BLE OTA Protocol](#ble-ota-protocol)
  - [BLE OTA Limitations (Secure Boot)](#ble-ota-limitations-secure-boot)
- [WiFi OTA](#wifi-ota)
- [NVS (Non-Volatile Storage)](#nvs-non-volatile-storage)
- [Tools](#tools)
  - [tools/dev](#toolsdev)
  - [esptool.py](#esptoolpy)
  - [nvs_partition_gen.py](#nvs_partition_genpy)
- [Workflow: Deploying Apps Without the iOS App](#workflow-deploying-apps-without-the-ios-app)
- [Recovery Procedure](#recovery-procedure)
- [Known Issues & Gotchas](#known-issues--gotchas)

---

## Hardware

| Property         | Value                                      |
|------------------|--------------------------------------------|
| SoC              | ESP32-D0WDQ6 (revision v1.0)              |
| Cores            | Dual-core Xtensa LX6, 240 MHz             |
| Flash            | 4 MB                                       |
| RAM              | 320 KB SRAM                                |
| Connectivity     | WiFi 802.11 b/g/n + Bluetooth 4.2 (BLE)   |
| Crystal          | 40 MHz                                     |
| Display          | 240×240 ST7789 LCD (SPI)                   |
| USB Serial Chip  | CP2102 or similar (macOS: `/dev/cu.usbserial-XXXX`) |
| Input            | Single button (GPIO 19, active LOW)        |
| Flash Encryption | **Enabled** (hardware-level)               |
| Secure Boot      | **Likely enabled** (rejects unsigned OTA)  |

---

## Display (TFT_eSPI)

The Kublet uses a 240×240 ST7789 LCD driven by TFT_eSPI. The **exact pin configuration** was found in the [kublet/krate](https://github.com/kublet/krate) source code:

```c
#define USER_SETUP_INFO "User_Setup"
#define ST7789_DRIVER
#define TFT_RGB_ORDER TFT_BGR
#define TFT_WIDTH  240
#define TFT_HEIGHT 240
#define TFT_MISO -1
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS    5
#define TFT_DC    2
#define TFT_RST   4
#define TFT_BL   15
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
#define SPI_FREQUENCY       1000000
#define SPI_READ_FREQUENCY 27000000
#define SPI_TOUCH_FREQUENCY 2500000
```

The `./tools/dev build` command automatically writes this to `.pio/libdeps/esp32dev/TFT_eSPI/User_Setup.h` before compiling (with SPI at 40 MHz instead of the original 1 MHz). **Building without this config will crash the device** — the display won't initialize and the app will fail.

---

## Flash Layout

Partition table (from dev firmware / community apps):

| Name     | Type    | Offset     | Size      |
|----------|---------|------------|-----------|
| nvs      | data    | `0x9000`   | 20 KB     |
| otadata  | data    | `0xE000`   | 8 KB      |
| app0     | ota_0   | `0x10000`  | 1280 KB   |
| app1     | ota_1   | `0x150000` | 1280 KB   |
| spiffs   | data    | `0x290000` | 1408 KB   |
| coredump | data    | `0x3F0000` | 64 KB     |

The bootloader lives at `0x1000` and the partition table at `0x8000`.

---

## Firmware Types

There are two types of firmware binaries:

### Factory Firmware (encrypted on disk)
- **`firmware/firmware.bin`** (699 KB) — Original manufacturer firmware with display support, BLE config, and the Kublet UI.
- **`firmware/bootloader.bin`** (18 KB) — Factory bootloader.
- **`firmware/partitions.bin`** (3 KB) — Factory partition table.
- These are **encrypted at rest** (flash encryption enabled). They don't have the standard ESP32 `0xE9` magic byte. However, `esptool.py` can still flash them — the ESP32 handles decryption/encryption transparently.

### Dev Firmware
- **`firmware/dev_firmware.bin`** (882 KB) — A minimal OTA server firmware. Connects to WiFi (reads creds from NVS) and exposes an HTTP OTA endpoint.
- **`firmware/dev_partitions.bin`** (3 KB) — Partition table for dev firmware.
- This firmware's sole purpose is to provide a WiFi OTA endpoint (`POST /update`) so you can wirelessly flash community apps.

### Community App Firmware (built from source)
- Built with `./tools/dev build <app-name>` from the project root.
- Output `.bin` in `.pio/build/esp32dev/firmware.bin`.
- Includes OTAServer library so subsequent flashes can be done wirelessly.
- Must be built with the correct TFT_eSPI pin config (handled by `./tools/dev build`).

---

## Bluetooth Low Energy (BLE)

The Kublet advertises two BLE services when running the factory firmware.

### BLE Services & Characteristics

**Config Service** — `7cfc0ee9-ac1a-4a82-8590-c1b6190a4d36`

| Characteristic | UUID | Properties | Description |
|---|---|---|---|
| Config Write   | `d5f58b50-023c-409b-846e-45397f7da674` | Write | Send config JSON to device |
| Config R/W     | `69cd6e40-10b4-4034-adcd-b51ab2b41cc6` | Read, Write | Config exchange |
| MAC Address    | `5dc9fed9-48ff-4cd7-92e8-0818da432a4b` | Read | Device WiFi MAC |
| FW Version     | `a1f91110-4973-4314-a1c7-a2e00a9410bb` | Read | Firmware version string |
| App Name       | `2e461e1e-4f92-4210-96a3-d7669486fcb9` | Read | Currently running app name |
| Status         | `17833482-c9fa-4c62-b5cb-fd1d1fadd8f1` | Read | Status byte |

**OTA Service** — `68035479-e5e7-482a-966e-717617f1b99a`

| Characteristic | UUID | Properties | Description |
|---|---|---|---|
| OTA TX (notify) | `9a425c95-0cf4-4bd0-804f-ef8069ac9ef4` | Notify | ACKs from ESP32 during upload |
| OTA Write       | `8a215016-d4cd-4efd-9bea-de89675d7409` | Write No Response | Firmware data chunks |

### BLE Config Protocol

The factory firmware accepts JSON written to the `Config Write` characteristic, but **does not persist WiFi credentials to NVS**. The credentials are silently ignored — the `core` NVS namespace remains empty.

> ⚠️ BLE config is not a viable way to provision WiFi. Use NVS partition flashing via USB instead (see [NVS](#nvs-non-volatile-storage)).

### BLE OTA Protocol

The OTA upload protocol was reverse-engineered:

1. Subscribe to notifications on `CHARACTERISTIC_TX_UUID`
2. Write the **file size as a 4-byte little-endian integer** to `CHARACTERISTIC_OTA_UUID`
3. Wait ~3 seconds (ESP32 runs `esp_ota_begin()` which erases flash)
4. Send firmware data in **512-byte chunks** (non-final chunks must be ≥ 510 bytes)
5. After each chunk batch, wait for an ACK notification
6. The final chunk (< 510 bytes) signals end of transfer
7. ESP32 calls `esp_ota_end()` to validate and commit

### BLE OTA Limitations (Secure Boot)

BLE OTA uploads complete successfully, but `esp_ota_end()` **rejects the firmware** and the device rolls back. This is caused by **Secure Boot** — the device only accepts firmware signed with the manufacturer's key. Since the manufacturer (Kublet) has shut down, we cannot obtain this key.

**Bottom line: BLE OTA is not viable for flashing custom firmware.** Use USB serial + WiFi OTA instead (see workflow below).

---

## WiFi OTA

Community apps (and the dev firmware) include the **OTAServer** library (`kublet/OTAServer@^1.0.4`), which:

1. Reads WiFi credentials from NVS namespace `core` (keys: `ssid`, `pw`)
2. Connects to WiFi
3. Starts an HTTP server on port 80
4. Registers mDNS as `esp32.local`
5. Exposes `POST /update` endpoint for multipart firmware upload

### Upload Format

The OTA endpoint expects a **multipart form upload** with the field name **`filedata`**:

```bash
# Using curl:
curl -X POST -F "filedata=@firmware.bin" http://<device-ip>/update

# Using tools/dev (preferred):
./tools/dev deploy <app-name>
```

> ⚠️ The form field must be `filedata`, NOT `firmware`. Using the wrong field name will appear to succeed but won't actually flash.

The device IP is assigned by DHCP. You can also try `esp32.local` if mDNS is working on your network.

---

## NVS (Non-Volatile Storage)

WiFi credentials are stored in NVS at partition offset `0x9000` (20 KB):

| Namespace | Key    | Type   | Value            |
|-----------|--------|--------|------------------|
| `core`    | `ssid` | string | WiFi SSID        |
| `core`    | `pw`   | string | WiFi password    |

App-specific configuration uses the `app` namespace.

### Generating NVS Binaries

Since BLE config write doesn't persist to NVS, we generate NVS partition images directly:

1. Create a CSV file (`tools/nvs_wifi.csv`):
   ```csv
   key,type,encoding,value
   core,namespace,,
   ssid,data,string,YourSSID
   pw,data,string,YourPassword
   ```

2. Generate the binary (from project root):
   ```bash
   uv run python ~/.platformio/packages/framework-espidf/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
     generate tools/nvs_wifi.csv tools/nvs_wifi.bin 0x5000
   ```
   (`esp-idf-nvs-partition-gen` is listed in `pyproject.toml`; run `uv sync` if not installed.)

3. Flash to `0x9000` (from project root):
   ```bash
   uv run esptool.py -p /dev/cu.usbserial-XXXX -b 460800 --chip esp32 \
     write_flash 0x9000 tools/nvs_wifi.bin
   ```

---

## Tools

### tools/dev

All-in-one development tool for building, deploying, and managing the Kublet device. Replaces the original `krate` Go CLI.

**Key commands:**
| Command | Description |
|---|---|
| `./tools/dev build [app]` | Compile firmware (auto-configures TFT_eSPI pins with 40 MHz SPI) |
| `./tools/dev deploy <app>` | Build and send firmware over WiFi OTA |
| `./tools/dev deploy <app> --skip-build` | Send without rebuilding |
| `./tools/dev logs [app]` | Stream serial logs via USB |
| `./tools/dev init` | Flash dev firmware + WiFi credentials via USB |
| `./tools/dev init --factory` | Restore factory firmware |

**What `build` does internally:**
1. Writes the correct `User_Setup.h` to `.pio/libdeps/esp32dev/TFT_eSPI/User_Setup.h` (40 MHz SPI)
2. Runs `pio run` to compile

**What `deploy` does internally:**
1. Runs the build step (unless `--skip-build`)
2. Reads `.pio/build/esp32dev/firmware.bin`
3. POSTs it as multipart form (`filedata` field) to `http://<ip>/update`

Configuration via `tools/.env` (auto-created on first run):
```
KUBLET_SSID=<WiFi SSID>
KUBLET_PW=<WiFi password>
```

> Generated files (`nvs_wifi.bin`, `nvs_wifi.csv`) are automatically cleaned up after flashing since they contain WiFi secrets. They are also in `.gitignore`.

### esptool.py

Used for USB serial flashing. Available via `uv run` from the project root (`esptool` is listed in `pyproject.toml`).

```bash
uv run esptool.py -p /dev/cu.usbserial-XXXX -b 460800 \
  --before default_reset --after hard_reset --chip esp32 \
  write_flash --flash_mode dio --flash_size detect --flash_freq 40m \
  <offset> <file> [<offset> <file> ...]
```

**Serial port:** `/dev/cu.usbserial-XXXX` (check macOS System Information or `ls /dev/cu.*` to find yours)  
**Baud rate:** 460800

### nvs_partition_gen.py

ESP-IDF tool for generating NVS partition binaries from CSV. Located at:
```
~/.platformio/packages/framework-espidf/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py
```

`esp-idf-nvs-partition-gen` is listed in `pyproject.toml`; run `uv sync` from the project root to install.

---

## Workflow: Deploying Apps Without the iOS App

### Prerequisites

- USB cable connected to Kublet
- PlatformIO installed (VS Code extension or CLI)
- `uv` installed (for running Python tools)

### Step 1: Flash Dev Firmware + WiFi Creds (USB, one-time)

```bash
./tools/dev init
```

This will:
1. Prompt for WiFi credentials (or read from `tools/.env`)
2. Generate the NVS partition with WiFi creds
3. Flash dev firmware + NVS via USB serial (auto-detected)
4. Read the device IP from serial boot output
5. Verify connectivity (ping + OTA endpoint)
6. Clean up generated NVS files (contain secrets)

To restore factory firmware instead: `./tools/dev init --factory`

### Step 2: Build and Deploy

```bash
./tools/dev deploy <app-name>
```

This builds the firmware and sends it over WiFi OTA. The device reboots into the new app. Since every community app includes OTAServer, you can repeat this step to flash different apps wirelessly.

### Changing WiFi Networks

If you move the device to a different WiFi network, just re-run init — it will prompt for new credentials:
```bash
./tools/dev init
```

---

## Recovery Procedure

If a bad flash bricks the device or the app crashes, re-flash dev firmware via USB to restore WiFi OTA:

```bash
./tools/dev init
```

To restore the original manufacturer UI (with BLE) instead:

```bash
./tools/dev init --factory
```

Note: the factory firmware does NOT include WiFi OTA, so you'll need to run `./tools/dev init` (without `--factory`) again before deploying apps wirelessly.

---

## Known Issues & Gotchas

1. **BLE config write doesn't persist WiFi to NVS.** The config characteristic accepts JSON without error, but credentials are not stored. Use NVS partition flashing via USB instead.

2. **BLE OTA is blocked by Secure Boot.** Upload completes 100% but the device rejects unsigned firmware at `esp_ota_end()` and rolls back. There is no workaround without the manufacturer's signing key.

3. **Building without TFT_eSPI config crashes the device.** Always use `./tools/dev build` (which writes the correct `User_Setup.h`) rather than raw `pio run`. If you must use `pio run` directly, add the pin defines as `build_flags` in `platformio.ini` (see [Display section](#display-tft_espi)).

4. **WiFi OTA form field is `filedata`, not `firmware`.** If using `curl` directly instead of `./tools/dev deploy`, use `-F "filedata=@firmware.bin"`.

5. **Factory firmware binaries are encrypted on disk.** They can still be flashed via `esptool.py` because the ESP32 handles encryption transparently. But you cannot inspect their contents or modify them.

6. **The KGFX library.** `kublet/KGFX` is a Kublet-specific graphics helper library. Community apps may or may not use it. The badgers app uses `TFT_eSPI` and `AnimatedGIF` directly.

7. **Button is on GPIO 19.** Active LOW with internal pull-up. Not documented by the manufacturer; determined via GPIO scanning.
