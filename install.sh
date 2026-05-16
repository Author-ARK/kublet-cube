#!/usr/bin/env bash
# Kublet Silver — one-shot bootstrap for macOS.
#
# Sets up everything you need to flash a Kublet cube with the silver-price
# app: clones the community fork, creates a Python venv, installs PlatformIO
# + ESP-IDF tooling, patches the desktop emulator's SDL2 hookup, and runs a
# smoke build to download and cache the ESP32 toolchain.
#
# Usage:
#   ./install.sh                # install in current directory (creates ./kublet-silver/)
#   ./install.sh ~/projects     # install at ~/projects/kublet-silver/
#
# Expected bundle layout (sibling to install.sh):
#   install.sh
#   HOWTO.md
#   HANDOVER.md            ← cold-start resume document for future you / agents
#   apps/silver/           ← the silver-price app source (copied into kublet-apps/apps/)
#   webui/                 ← the fleet web UI (used as-is, parallel to kublet-apps/)
#   patches/kublet-apps/   ← drop-in replacements for files in the cloned upstream:
#                            tools/src/emulate/CMakeLists.txt  (SDL2 fix)
#                            tools/src/kublet_dev/flash.py     (non-interactive init)
#
# Idempotent: re-running upgrades pieces that are missing, leaves the rest.

set -euo pipefail

BUNDLE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="${1:-$PWD}"
KUBLET_DIR="$TARGET_DIR/kublet-silver"
APPS_REPO_URL="https://github.com/markusos/kublet-apps.git"

# ---------- pretty printing ----------
bold()  { printf "\033[1m%s\033[0m\n" "$*"; }
say()   { printf "  %s\n" "$*"; }
ok()    { printf "  \033[32m✓\033[0m %s\n" "$*"; }
warn()  { printf "  \033[33m⚠\033[0m %s\n" "$*"; }
die()   { printf "  \033[31m✗\033[0m %s\n" "$*" >&2; exit 1; }

bold "Kublet Silver bootstrap"
echo "  Target: $KUBLET_DIR"
echo

# ---------- 0. host check ----------
bold "0. Host check"
[[ "$(uname -s)" == "Darwin" ]] || die "This installer targets macOS. For Linux, see HOWTO.md."
ok "macOS detected"

if ! command -v brew >/dev/null 2>&1; then
  die "Homebrew not found. Install from https://brew.sh first, then re-run this script."
fi
ok "Homebrew installed"

if ! command -v git >/dev/null 2>&1; then
  die "git not found (should ship with macOS Command Line Tools — run 'xcode-select --install')."
fi
ok "git installed"

PYTHON_BIN=""
for candidate in python3.13 python3.14 python3.12 python3; do
  if command -v "$candidate" >/dev/null 2>&1; then
    if "$candidate" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)' 2>/dev/null; then
      PYTHON_BIN="$candidate"
      break
    fi
  fi
done
[[ -n "$PYTHON_BIN" ]] || die "Python ≥3.12 required. Install with: brew install python@3.13"
ok "Python: $($PYTHON_BIN --version)"
echo

# ---------- 1. brew deps ----------
bold "1. Homebrew packages (for desktop emulator)"
NEEDED=()
for pkg in cmake sdl2 ffmpeg; do
  if brew list "$pkg" >/dev/null 2>&1; then
    ok "$pkg"
  else
    NEEDED+=("$pkg")
  fi
done
if (( ${#NEEDED[@]} > 0 )); then
  say "Installing: ${NEEDED[*]}"
  brew install "${NEEDED[@]}"
fi
echo

# ---------- 2. clone the kublet-apps fork ----------
bold "2. kublet-apps source"
mkdir -p "$KUBLET_DIR"
cd "$KUBLET_DIR"
if [[ -d kublet-apps/.git ]]; then
  ok "kublet-apps repo already present — fetching latest"
  ( cd kublet-apps && git pull --ff-only --quiet || warn "pull skipped (local changes?)" )
else
  say "Cloning $APPS_REPO_URL"
  git clone --quiet "$APPS_REPO_URL" kublet-apps
  ok "cloned to $KUBLET_DIR/kublet-apps"
fi

# Excise build artefacts from cloud sync if we're inside Dropbox/iCloud
if [[ "$KUBLET_DIR" == *"/Dropbox/"* || "$KUBLET_DIR" == *"/iCloud"* ]]; then
  for d in kublet-apps/.git kublet-apps/.pio; do
    [[ -e "$d" ]] && xattr -w com.dropbox.ignored 1 "$d" 2>/dev/null || true
  done
  ok "marked .git and .pio as Dropbox-ignored (they'd otherwise sync 90+ MB per build)"
fi
echo

# ---------- 3. python venv + deps ----------
bold "3. Python venv at kublet-apps/kublet_env/"
cd "$KUBLET_DIR/kublet-apps"
if [[ ! -d kublet_env ]]; then
  "$PYTHON_BIN" -m venv kublet_env
  ok "created kublet_env"
  if [[ "$KUBLET_DIR" == *"/Dropbox/"* ]]; then
    xattr -w com.dropbox.ignored 1 kublet_env 2>/dev/null || true
  fi
fi
# shellcheck source=/dev/null
source kublet_env/bin/activate
pip install --upgrade --quiet pip
pip install --quiet -e . platformio flask
ok "installed: pyserial, esptool, esp-idf-nvs-partition-gen, platformio, flask"
echo

# ---------- 4. apply patches/ over the freshly-cloned tree ----------
bold "4. Apply patches/ over kublet-apps/"
PATCHES_SRC="$BUNDLE_DIR/patches/kublet-apps"
if [[ -d "$PATCHES_SRC" ]]; then
  # The bundle contains pre-patched copies of files in the same directory layout.
  # Overlay them on the cloned tree. Three files matter today:
  #   tools/src/emulate/CMakeLists.txt   — SDL2::SDL2 imported-target fix
  #   tools/src/kublet_dev/flash.py      — env-var creds, no-uv, pip nvs_partition_gen
  count=0
  while IFS= read -r -d '' f; do
    rel="${f#$PATCHES_SRC/}"
    dst="$KUBLET_DIR/kublet-apps/$rel"
    if [[ -f "$dst" ]]; then
      /bin/cp "$f" "$dst"
      say "patched $rel"
      ((count++))
    else
      warn "patch target missing — $rel (skipped)"
    fi
  done < <(/usr/bin/find "$PATCHES_SRC" -type f -print0)
  ok "applied $count patch file(s)"
else
  warn "no patches/ in bundle — emulator may not build, dev tool may need manual fixes"
fi
echo

# ---------- 5. seed the silver app from the bundle ----------
bold "5. Silver app at apps/silver/"
SILVER_DIR="$KUBLET_DIR/kublet-apps/apps/silver"
SILVER_SRC="$BUNDLE_DIR/apps/silver"
if [[ -d "$SILVER_SRC" ]]; then
  if [[ -d "$SILVER_DIR" ]]; then
    say "refreshing apps/silver/ from bundle"
  else
    say "installing apps/silver/ from bundle"
  fi
  mkdir -p "$KUBLET_DIR/kublet-apps/apps"
  rm -rf "$SILVER_DIR"
  cp -R "$SILVER_SRC" "$SILVER_DIR"
  ok "apps/silver/ in place"
else
  warn "no bundled apps/silver/ found at $SILVER_SRC"
  if [[ -d "$SILVER_DIR" ]]; then
    say "existing apps/silver/ left as-is"
  else
    warn "and no existing apps/silver/ — the smoke build will be skipped"
  fi
fi
echo

# ---------- 6. smoke build (downloads ESP32 toolchain on first run) ----------
bold "6. Smoke build (this is slow on first run — downloads ESP32 toolchain)"
if [[ -d "$SILVER_DIR" ]]; then
  if python tools/dev build silver >/tmp/kublet_build.log 2>&1; then
    SIZE=$(grep -oE 'Build succeeded \([0-9]+ KB\)' /tmp/kublet_build.log | tail -1)
    ok "silver firmware: $SIZE"
  else
    warn "build failed — last 20 lines:"
    tail -20 /tmp/kublet_build.log | sed 's/^/    /'
    die "fix the build before continuing"
  fi
else
  warn "skipped — apps/silver/ missing"
fi
echo

# ---------- 7. install webui (sibling dir) from the bundle ----------
bold "7. WebUI at $KUBLET_DIR/webui/"
WEBUI_SRC="$BUNDLE_DIR/webui"
WEBUI_DST="$KUBLET_DIR/webui"
if [[ -d "$WEBUI_SRC" && "$WEBUI_SRC" != "$WEBUI_DST" ]]; then
  say "installing webui from bundle"
  mkdir -p "$WEBUI_DST"
  # Preserve any existing devices.json so re-running doesn't wipe it
  EXISTING_DEVICES=""
  [[ -f "$WEBUI_DST/devices.json" ]] && EXISTING_DEVICES="$(cat "$WEBUI_DST/devices.json")"
  rsync -a --delete --exclude='__pycache__' --exclude='*.pyc' "$WEBUI_SRC/" "$WEBUI_DST/"
  if [[ -n "$EXISTING_DEVICES" ]]; then
    printf '%s' "$EXISTING_DEVICES" > "$WEBUI_DST/devices.json"
    say "preserved existing devices.json"
  fi
  ok "webui in place at $WEBUI_DST"
elif [[ -f "$WEBUI_DST/app.py" ]]; then
  ok "webui already in place"
else
  warn "no bundled webui/ found at $WEBUI_SRC — skipping"
fi
echo

# ---------- done ----------
bold "All done."
cat <<EOF

Next steps (see HOWTO.md for the long version):

  1. Plug your kublet into a data-capable USB Mini-B cable (the trapezoid connector on the cube). Install the SiLabs CP210x driver if you haven't.
  2. cd $KUBLET_DIR/kublet-apps
     source kublet_env/bin/activate
     python tools/dev init --name kublet1
     # enter your WiFi SSID + password when prompted
  3. python tools/dev deploy silver kublet1
     # → cube reboots into the silver app
  4. (optional) Start the fleet webui:
     cd $KUBLET_DIR/webui && python app.py
     # browse http://localhost:1666

Have fun.
EOF
