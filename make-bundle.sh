#!/usr/bin/env bash
# Build a shareable kublet-silver-bundle.tar.gz containing the installer,
# docs, the silver app source, and the webui — minus build artefacts and
# venvs. The recipient unpacks it, runs ./install.sh, and is good to go.
#
# Usage:
#   ./make-bundle.sh                  # writes ./kublet-silver-bundle.tar.gz
#   ./make-bundle.sh /tmp/foo.tar.gz  # writes to a specific path

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/kublet-silver-bundle.tar.gz}"

if [[ ! -d "$HERE/kublet-apps/apps/silver" ]]; then
  echo "✗ apps/silver source not found at $HERE/kublet-apps/apps/silver" >&2
  echo "  Run this from the same dir that contains kublet-apps/." >&2
  exit 1
fi
if [[ ! -d "$HERE/webui" ]]; then
  echo "✗ webui/ not found at $HERE/webui" >&2
  exit 1
fi

# Stage the bundle in a temp dir so we control exactly what ships
STAGE="$(mktemp -d -t kublet-silver-bundle.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

ROOT="$STAGE/kublet-silver-bundle"
mkdir -p "$ROOT/apps"

# Top-level files: installer + docs
cp "$HERE/install.sh" "$ROOT/"
cp "$HERE/HOWTO.md"   "$ROOT/"
[[ -f "$HERE/HANDOFF.md" ]]  && cp "$HERE/HANDOFF.md"  "$ROOT/"
[[ -f "$HERE/HANDOVER.md" ]] && cp "$HERE/HANDOVER.md" "$ROOT/"

# Silver app — clean copy without .pio leftovers
rsync -a --exclude='.pio' --exclude='__pycache__' \
  "$HERE/kublet-apps/apps/silver/" "$ROOT/apps/silver/"

# Patches: drop-in replacements for upstream files (SDL2 fix, non-interactive init)
if [[ -d "$HERE/patches" ]]; then
  rsync -a --exclude='__pycache__' --exclude='*.pyc' \
    "$HERE/patches/" "$ROOT/patches/"
fi

# Webui — clean copy without venv/cache/devices state
rsync -a --exclude='__pycache__' --exclude='*.pyc' --exclude='kublet_env' \
  "$HERE/webui/" "$ROOT/webui/"
# Reset devices.json so the recipient starts fresh
cat > "$ROOT/webui/devices.json" <<'JSON'
{
  "devices": []
}
JSON

# Drop a minimal README at the root so the recipient knows what to do
cat > "$ROOT/README.txt" <<'TXT'
Kublet Silver — shareable bundle.

1. Unpack this tarball anywhere (e.g. ~/Documents/).
2. Open a Terminal in the unpacked folder.
3. Run:    ./install.sh
4. Read HOWTO.md for first-time setup (USB-C cable, WiFi flash, deploy).

Requires macOS with Homebrew. Linux works too — see HOWTO.md.
TXT

chmod +x "$ROOT/install.sh"

# Pack it
mkdir -p "$(dirname "$OUT")"
tar -C "$STAGE" -czf "$OUT" kublet-silver-bundle

SIZE=$(du -sh "$OUT" | awk '{print $1}')
echo "✓ Bundle written: $OUT ($SIZE)"
echo
echo "Contents:"
tar -tzf "$OUT" | sed 's/^/    /'
echo
echo "Share this file. Recipient runs:"
echo "    tar -xzf $(basename "$OUT")"
echo "    cd kublet-silver-bundle"
echo "    ./install.sh"
