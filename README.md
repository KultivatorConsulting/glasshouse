# Glasshouse

A native Linux Qt 6 desktop client for [PiKVM](https://pikvm.org/).
Presents N PiKVMs as N independently-movable, resizable windows on a
KDE desktop, replacing the pattern of N Chrome tabs pointed at N PiKVM
web UIs. One PiKVM acts as the USB HID master for a shared target
host; the others are video-only. A single local keyboard and mouse are
shared between the windows via session-wide click-to-capture.

The full architecture, empirically-verified coordinate model, and the
list of PiKVM API quirks Glasshouse works around live in
[specs/DESIGN.md](specs/DESIGN.md). Release-by-release changes live in
[CHANGELOG.md](CHANGELOG.md).

## Status

**v0.1.0 — first release.** All Phase 0–8 work in DESIGN.md §9 is
complete. The viewer is in daily use against a live two-PiKVM (PiKVM 4
Plus + PiKVM 3) setup.

What works end-to-end today:

- One independently-movable, resizable Qt window per configured PiKVM.
- **MJPEG transport** (`/streamer/stream`) — recommended for any
  long-running session. Leak-free, works on every PiKVM model.
- WebRTC over Janus (`/janus/ws`, plugin `janus.plugin.ustreamer`)
  with hardware H.264 decode preferred (`nvh264dec` / `vah264dec`),
  `avdec_h264` fallback. **Note:** the underlying `webrtcbin` element
  has a long-running memory leak (~8 MB/s/stream); only suitable
  for short sessions or until you can restart the viewer. See
  [DESIGN.md §10.5](specs/DESIGN.md) for the investigation. MJPEG
  is the path to use in production for now.
- Session-wide click-to-capture: cursor walks continuously across
  windows, with the active coordinate transform following it.
- Mouse, keyboard, scroll wheel, and curated/custom shortcut chords
  routed through one configured HID master to the target.
- Special Keys palette (`Ctrl+Alt+K`) for chords the local compositor
  swallows (PrintScreen, Super-key combos, …) plus paste-as-keystrokes
  for picky BIOS prompts.
- Mass Storage upload / mount / eject / present-as-CDROM.
- Target ATX power short-press, long-press (force-off), reset — each
  gated by a confirmation dialog.
- Per-PiKVM window geometry / fullscreen / maximized state persists
  across sessions.
- `.deb` packaging built by GitHub Actions on tag push, linked against
  system Qt 6.4 from the noble apt archive (no bundled Qt).

Deliberately out of scope for v0.1.0: clipboard sync, audio, macro
keypad daemon. See [CHANGELOG.md](CHANGELOG.md) for the full list.

## Install (from a release `.deb`)

Tagged releases publish a `.deb` to the
[Releases page](https://github.com/kultivator-consulting/glasshouse/releases).
Download `glasshouse-viewer_<version>_amd64.deb` and install with:

```bash
sudo apt install ./glasshouse-viewer_<version>_amd64.deb
```

`apt install` (vs. `dpkg -i`) resolves Qt 6, GStreamer, and libnice
runtime dependencies from the stock Ubuntu 24.04 archive in one step.
No per-user Qt setup required.

The package drops an example config at
`/etc/xdg/glasshouse/glasshouse-viewer.example.yaml` — copy it to
`~/.config/glasshouse/config.yaml` and edit:

```bash
mkdir -p ~/.config/glasshouse
cp /etc/xdg/glasshouse/glasshouse-viewer.example.yaml \
   ~/.config/glasshouse/config.yaml
$EDITOR ~/.config/glasshouse/config.yaml
```

The schema is documented in [DESIGN.md §8](specs/DESIGN.md#8-configuration-schema-yaml);
the validator enforces the rules in §8.1 at startup.

## Configure secrets

PiKVM passwords (and optional TOTP seeds) live outside the config
file. The config refers to them by `secret://<name>` references which
resolve in this order:

1. **Environment variable** `GLASSHOUSE_SECRET_<NAME>` — uppercased,
   non-alphanumerics replaced with `_`.
   `secret://pikvm-1-passwd` → `GLASSHOUSE_SECRET_PIKVM_1_PASSWD`.
2. **`~/.config/glasshouse/secrets.yaml`** — YAML map, used when the
   env var is unset. This is the path that survives KDE-menu /
   `.desktop` launches where shell rc files don't get sourced; prefer
   it for desktop launches.

Either form works on its own. Env wins when both are set.

```bash
mkdir -p ~/.config/glasshouse
cat > ~/.config/glasshouse/secrets.yaml <<'EOF'
pikvm-71-passwd:  hunter2
pikvm-144-passwd: hunter2
# pikvm-71-totp:    JBSWY3DPEHPK3PXP   # optional base32 TOTP seed
EOF
chmod 0600 ~/.config/glasshouse/secrets.yaml
```

The viewer warns at startup if the file's permissions allow group or
other access. KWallet / libsecret integration is a future enhancement.

## Run

```bash
# Bring up every window in the config.
glasshouse-viewer

# Bring up only one window (e.g. for ad-hoc testing).
glasshouse-viewer --only 192.168.1.71

# Append all log messages to a file in addition to stderr. Honours
# QT_LOGGING_RULES — pair with one to dump verbose categories.
glasshouse-viewer --log-file /tmp/glasshouse.log
QT_LOGGING_RULES='glasshouse.video.debug=true' \
    glasshouse-viewer --log-file /tmp/glasshouse.log
```

Default hotkeys (configurable per-PiKVM in YAML):

| Hotkey | Action |
|---|---|
| `Ctrl+Alt+Shift+Escape` | Release session-wide capture |
| `F11`                   | Toggle fullscreen on this window |
| `Ctrl+Alt+K`            | Show / hide Special Keys palette |

## Prep on the PiKVMs

- **HID master** (the one named in `hid_master:` in the config): USB
  OTG cable plugged to the target host; mouse must be in absolute
  mode. Flip it via the web UI (System → Mouse) or:

    ```bash
    curl -k -X POST -u admin:PASS \
      'https://<hid-master>/api/hid/set_params?mouse_output=usb'
    ```

- **Video-only PiKVMs**: HID gadget disabled in
  `/etc/kvmd/override.yaml` (see
  [DESIGN.md §4.3](specs/DESIGN.md#43-pikvm-side-configuration)) — or
  simply leave the OTG cable unplugged. Same effect.

No PiKVM firmware modifications are required. Glasshouse works against
stock kvmd / ustreamer; cold-start quirks (encoder warmup, older-
firmware pre-IDR 503s) are handled client-side.

## Build from source

### Build dependencies (apt)

```bash
sudo apt install \
    cmake ninja-build pkg-config \
    qt6-base-dev qt6-multimedia-dev qt6-websockets-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev gstreamer1.0-plugins-bad \
    gstreamer1.0-nice gstreamer1.0-libav libnice-dev
```

| Dependency | Version | Notes |
|---|---|---|
| OS         | Ubuntu / Kubuntu 24.04+ | other distros should work; only tested on 24.04 |
| Compiler   | GCC 13.3+               | C++20 required |
| CMake      | 3.22+                   | |
| Qt         | 6.4+                    | system Qt from `qt6-*` apt packages is enough; nothing post-6.4 is in use |
| yaml-cpp   | 0.8.0                   | vendored via `FetchContent` — no system package |

### Configure and build

```bash
cmake --preset default
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Release build:

```bash
cmake --preset release
cmake --build build-release -j
```

The first configure downloads yaml-cpp 0.8.0 from GitHub via
`FetchContent`. Subsequent builds use the cached source.

### Run from the build tree

```bash
./build/bin/glasshouse-viewer
./build/bin/glasshouse-viewer --only <host>

# Phase 1 CLI harness — state-WS smoke driver, no video, no Qt windows.
./build/bin/glasshouse-cli

# Janus signalling probe — drives info → create → attach → watch and
# logs the SDP offer; useful when diagnosing transport failures.
./build/bin/glasshouse-janusprobe --host <pikvm>
```

## Project layout

```
glasshouse/
├── CMakeLists.txt              top-level build; declares yaml-cpp via FetchContent
├── CMakePresets.json           `default` = Debug in ./build; `release` in ./build-release
├── CHANGELOG.md                release-by-release changes
├── README.md                   you are here
├── LICENSES/                   LGPL-3.0 + MDI icon NOTICE
├── specs/DESIGN.md             architectural source of truth
├── pikvm_coord_verify.py       Phase 0 coord-space regression script
├── config/example.yaml         reference config matching DESIGN.md §8
├── dist/
│   ├── nfpm.yaml               .deb metadata; consumed by the Release workflow
│   ├── glasshouse-viewer.desktop
│   ├── glasshouse-viewer.svg
│   └── glasshouse-viewer.example.yaml
├── .github/workflows/          GH Actions: build + tag-driven release
├── src/core/                   auth + state WS + reconnect + secrets + config + logging
├── src/video/                  Janus client, GStreamer pipelines, VideoWindow,
│                               Special Keys & Mass Storage dialogs
├── src/input/                  CoordTransform, InputRouter, MDN keymap
├── src/cli/                    glasshouse-cli (Phase 1 harness)
├── src/viewer/                 glasshouse-viewer (the GUI; ties everything together)
└── tests/                      QTest: config validator, coord transform, keymap
```

## Troubleshooting

**`config load failed: ...`** — the validator rejected the config.
Read the error list; it names the specific §8.1 rule that was
violated.

**`login failed: Host requires authentication`** — wrong user /
password, or the secret didn't resolve. Check that
`GLASSHOUSE_SECRET_<NAME>` matches what the loader expects, or that
the entry in `~/.config/glasshouse/secrets.yaml` matches the
`secret://<name>` reference. The loader logs the expected env-var
name when a secret can't be resolved.

**Blank video, ICE reaches connected, no `pad-added`** — usually
means the PiKVM's H.264 encoder isn't running. Glasshouse opens the
state WS with `?stream=1` to wake the encoder; if you've patched a
fork to use `?stream=0`, the encoder won't spawn until something
else watches. See
[DESIGN.md §10.5](specs/DESIGN.md#105-video-transport-januswebrtc-verified-2026-04-25).

**Older PiKVM 3 returns 503 `"Haven't received SPS/PPS from memsink
yet"`** — expected on the first `watch`; `JanusClient` retries
automatically (up to 5 attempts, 2 s apart) until the encoder produces
its first IDR. If retries exhaust, the outer reconnect ladder takes
over.

**`[host] error: SSL handshake failed`** — the example config sets
`insecure_tls: true` per-PiKVM because PiKVMs ship self-signed certs.
If you disabled that opt-in, you need to install the PiKVM's CA
locally or re-enable the flag.

**Verbose logs** — every subsystem has its own
`Q_LOGGING_CATEGORY` so you can isolate noise:

```bash
QT_LOGGING_RULES='glasshouse.janus.debug=true;glasshouse.video.debug=true' \
    glasshouse-viewer
```

Categories: `glasshouse.{pikvm,ws,hid,video,janus,config}`.

## Phase 0 regression

`pikvm_coord_verify.py` walks the PiKVM absolute coordinate space with
a deterministic probe sequence so cursor placement on the target can
be observed via `libinput debug-events`. Re-run after any PiKVM
firmware upgrade:

```bash
pip install requests
python3 pikvm_coord_verify.py --host <pikvm> --user admin
```

Results are recorded in
[DESIGN.md §10.1](specs/DESIGN.md#101-coordinate-range-and-origin-verified-2026-04-25).

## Contributing

- **Empirical verifications matter.** When implementation contradicts
  the design (latency, API behaviour, Qt quirks, PiKVM firmware
  surprises), update `specs/DESIGN.md` §10 in the same commit that
  reveals it. The slots in §10 exist for exactly this.
- **Tests for pure logic.** Network-heavy behaviour (auth, WS, Janus)
  is exercised by the harnesses against real hardware. Pure-function
  code — config validation, coord transform, keymap — must have QTest
  coverage in `tests/`.
- **Don't modify the PiKVM.** Glasshouse runs against stock firmware;
  workarounds for firmware quirks live client-side. If you find
  yourself wanting to patch kvmd or ustreamer, find a client-side
  path instead.

## Design and references

- [specs/DESIGN.md](specs/DESIGN.md) — architecture, coord model,
  PiKVM API subset, empirical verifications.
- [CHANGELOG.md](CHANGELOG.md) — what changed, version by version.
- [PiKVM handbook](https://docs.pikvm.org/)
- [PiKVM HTTP API reference](https://docs.pikvm.org/api/)
- [kvmd source](https://github.com/pikvm/kvmd) (the Janus signalling
  reference is `web/share/js/kvm/stream_janus.js`)
- [ustreamer source](https://github.com/pikvm/ustreamer) (the Janus
  plugin's accepted verbs are in `janus/src/plugin.c`)

## License

Glasshouse is licensed under
[**LGPL-3.0-or-later**](LICENSES/LGPL-3.0.txt), matching the dynamic
Qt 6 linkage requirement (Qt 6 is LGPL v3 with The Qt Company's GPLv3
exception).

The MDI greenhouse glyph used as the application icon is distributed
under the Pictogrammers Free License — see
[LICENSES/material-design-icons-NOTICE.txt](LICENSES/material-design-icons-NOTICE.txt).
