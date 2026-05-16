# Contributing to Kublet-Asu

Thanks for stopping by. Bug reports, app ideas, and PRs are all welcome.

## Where to start

- **Setting up a dev environment**, running the webui, building an app,
  flashing a cube, troubleshooting → see [DEVELOPING.md](DEVELOPING.md).
- **Just trying to get a brand-new cube on your network** → see
  [HOWTO.md](HOWTO.md).
- **Project license + attribution** → [LICENSE](LICENSE) (Apache 2.0)
  and [NOTICE](NOTICE).

## Filing issues

When reporting a bug it helps a lot to include:

- What kind of cube + which app
- Output of `./kublet.sh status` (PID, log tail)
- Output of `./tools/dev logs -p /dev/cu.usbserial-XXXX` (serial dump
  from the cube) if it's a runtime/boot issue
- For OTA failures: `ping <cube-ip>` output

## Pull requests

- Match the existing code style — apps are kept deliberately
  copy-paste-y across the catalog so each one is self-contained and
  readable in isolation. Don't refactor common code into a shared lib
  unless we've discussed it first.
- New external dependencies (libraries or runtime APIs) require an
  entry in [NOTICE](NOTICE) crediting the upstream.
- New apps want at minimum: `platformio.ini`, `manifest.yaml`,
  `src/main.cpp`, and `assets/preview.png` (run
  `./tools/emulate <YourApp> --screenshot assets/preview.png --after 3`
  to generate the gallery tile).

## Code of conduct

Be kind. The Kublet hardware was abandoned by its vendor; everything
in this repo only works because hobbyists keep showing up. Please
don't be the reason someone stops.
