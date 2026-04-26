# Glasshouse

A native Linux Qt 6 desktop application that presents N PiKVMs as N
independently-movable KDE windows, replacing the pattern of N Chrome tabs
pointed at N PiKVM web UIs. One PiKVM acts as the USB HID master for a
shared target host; the others are video-only. A single local keyboard and
mouse are shared between the windows via click-to-capture.

See [specs/DESIGN.md](specs/DESIGN.md) for the full architecture, empirically
verified coordinate model, component breakdown, and phase plan.

## Status

**Phase 1 — PiKVM client skeleton: complete.** What works end-to-end today:

- HTTPS auth (cookie-based, optional TOTP concatenation)
- `/api/ws?stream=0` state WebSocket with initial-burst handling and
  reconnect-with-exponential-backoff
- HID event sender over the same WS (mouse move / button / wheel, key,
  synthesised shortcut)
- YAML config loader + structural validator (DESIGN.md §8.1)
- CLI harness (`glasshouse-cli`) that proves the loop against real PiKVMs

**Not yet implemented:** video decode, Qt windows, click-to-capture, input
router, letterboxing, polish — see [DESIGN.md §9](specs/DESIGN.md#9-phase-plan)
for the full phase plan.

## Install (from a release .deb)

Tagged releases publish a `.deb` to
[GitHub Releases](https://github.com/kultivator-consulting/glasshouse/releases).
Download the `glasshouse-viewer_<version>_amd64.deb` for your release and
install with:

```bash
sudo apt install ./glasshouse-viewer_<version>_amd64.deb
```

`apt install` (vs. `dpkg -i`) resolves the GStreamer / libnice apt
dependencies in one step.

> **Qt 6.7 prerequisite.** The current `.deb` does not bundle Qt and Ubuntu
> 24.04's archive ships only Qt 6.4. Install Qt 6.7 per-user via aqt before
> running the viewer (see [Installing Qt 6.7 without touching the system](#installing-qt-67-without-touching-the-system)
> below). Bundling Qt with the package is on the todo list.

The package drops an example config at
`/etc/xdg/glasshouse/glasshouse-viewer.example.yaml` — copy it to
`~/.config/glasshouse/config.yaml` and edit.

## Project layout

```
glasshouse/
├── CMakeLists.txt              top-level build; declares yaml-cpp via FetchContent
├── CMakePresets.json           `default` = Debug in ./build; `release` in ./build-release
├── LICENSES/                   LGPL-3.0 text for Qt (dynamically linked)
├── README.md                   you are here
├── specs/DESIGN.md             design doc — the architectural source of truth
├── pikvm_coord_verify.py       Phase 0 coord-space regression script
├── config/example.yaml         reference config matching DESIGN.md §8
├── src/core/                   glasshouse_core static library
│   ├── pikvmclient.{h,cpp}     auth + state WS + reconnect + HID sender
│   ├── config.{h,cpp}          YAML load + §8.1 validator
│   ├── secrets.{h,cpp}         `secret://` → env-var resolver
│   ├── pikvmevents.{h,cpp}     MouseButton enum + HidState struct
│   └── logging.{h,cpp}         Q_LOGGING_CATEGORY per subsystem
├── src/cli/main.cpp            glasshouse-cli: Phase 1 harness
└── tests/test_config.cpp       QTest: config-validator coverage
```

## Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| OS         | Ubuntu / Kubuntu 24.04+        | Other distros should work; only tested on 24.04 |
| Compiler   | GCC 13.3+                      | C++20 required                                  |
| CMake      | 3.25+                          | tested on 4.3                                   |
| Ninja      | any recent                     | used by the `default` preset                    |
| Qt         | 6.7+ (Core, Network, WebSockets, Test) | installed per-user (see below)          |
| yaml-cpp   | 0.8.0                          | vendored via `FetchContent` — no system package |

### Installing Qt 6.7 without touching the system

Ubuntu 24.04's archive ships Qt 6.4, which is what KDE Plasma itself is
linked against on Kubuntu. **Do not install a Qt backports PPA** — it
replaces system `libqt6*` under a running desktop and has historically been
a good way to end up at a tty after a reboot. Install Qt 6.7 into your home
directory instead:

```bash
pipx install aqtinstall
mkdir -p ~/Qt
aqt install-qt linux desktop 6.7.3 linux_gcc_64 -m qtwebsockets -O ~/Qt
```

`CMakePresets.json` points `CMAKE_PREFIX_PATH` at `$HOME/Qt/6.7.3/gcc_64`.
If you install a different 6.7.x patch, either update the preset or pass
`-DCMAKE_PREFIX_PATH=...` on the command line.

To uninstall: `rm -rf ~/Qt`. Nothing else is affected.

## Build

```bash
cmake --preset default
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Release:

```bash
cmake --preset release
cmake --build build-release -j
```

The first configure downloads yaml-cpp 0.8.0 from GitHub via
`FetchContent`. Subsequent builds use the cached source.

## Configuration

The CLI loads `$XDG_CONFIG_HOME/glasshouse/config.yaml` by default
(`~/.config/glasshouse/config.yaml` on most setups). Start from the
reference:

```bash
mkdir -p ~/.config/glasshouse
cp config/example.yaml ~/.config/glasshouse/config.yaml
$EDITOR ~/.config/glasshouse/config.yaml
```

The schema is documented in [DESIGN.md §8](specs/DESIGN.md#8-configuration-schema-yaml);
the validator enforces the five rules in §8.1.

### Secrets

Passwords and TOTP seeds live outside the config file. The config uses
`secret://<name>` references that resolve to env vars of the form
`GLASSHOUSE_SECRET_<NAME>` (uppercased, non-alphanumerics replaced with
`_`). For example `secret://pikvm-1-passwd` → `GLASSHOUSE_SECRET_PIKVM_1_PASSWD`.

```bash
export GLASSHOUSE_SECRET_PIKVM_1_PASSWD='...'
export GLASSHOUSE_SECRET_PIKVM_2_PASSWD='...'
# optional, if totp_secret_ref is set in the config (base32 seed):
export GLASSHOUSE_SECRET_PIKVM_1_TOTP='...'
```

KWallet / libsecret integration is a Phase 6/7 concern.

## Running the CLI harness

`glasshouse-cli` is the Phase 1 smoke driver. It loads the config, opens a
`PiKvmClient` per configured PiKVM, and streams state events to stdout. No
video, no Qt windows.

```bash
# Connect to everything in the config, stream state events until Ctrl+C.
./build/bin/glasshouse-cli

# Filter to a single PiKVM in the config.
./build/bin/glasshouse-cli --host 192.168.1.71

# On the HID master only: send the DESIGN.md §10.1 5-point coord sweep
# (center → four corners → center) with 1s gaps, then exit. Watch the
# target screen to confirm the cursor moves.
./build/bin/glasshouse-cli --send-test-sequence

# Surface every received WS frame (debug-level logging is silent by default).
QT_LOGGING_RULES='glasshouse.ws.debug=true' ./build/bin/glasshouse-cli
```

### Prep on the PiKVMs

- **HID master** (whichever IP is named in `hid_master:`): USB OTG cable
  plugged to the target host; mouse must be in absolute mode. Flip it
  via the web UI (System → Mouse) or:

    ```bash
    curl -k -X POST -u admin:PASS \
      'https://<hid-master>/api/hid/set_params?mouse_output=usb'
    ```

- **Video-only PiKVMs**: HID gadget disabled in `/etc/kvmd/override.yaml`
  (see [DESIGN.md §4.3](specs/DESIGN.md#43-pikvm-side-configuration)) — or
  simply leave the OTG cable unplugged.

## Phase 0 regression

`pikvm_coord_verify.py` walks the PiKVM absolute coordinate space with a
deterministic probe sequence so you can observe cursor placement on the
target via `libinput debug-events`. Re-run after any PiKVM firmware upgrade:

```bash
pip install requests
python3 pikvm_coord_verify.py --host <pikvm> --user admin
```

Results are recorded in [DESIGN.md §10.1](specs/DESIGN.md#101-coordinate-range-and-origin-verified-2026-04-25).

## Troubleshooting

**`config load failed: ...`** — the validator rejected the config.
Read the error list; it names the specific §8.1 rule that was violated.

**`login failed: Host requires authentication`** or similar on startup**
— wrong user/password, or the secret env var isn't exported. Double-check
the `GLASSHOUSE_SECRET_*` name matches what the loader expects; the loader
logs the expected env-var name when a secret ref can't be resolved.

**WS connects but `initial state` never prints** — `loop` is the
burst-terminator marker (see [DESIGN.md §10.4](specs/DESIGN.md#104-websocket-event-formats-verified-2026-04-25)).
If it never arrives, run with
`QT_LOGGING_RULES='glasshouse.ws.debug=true'` and inspect the raw frame
stream.

**`--send-test-sequence` errors with "HID master is not in absolute mode"**
— flip the PiKVM HID mode with the curl command above, then re-run.

**`[host] error: SSL handshake failed`** — the default config sets
`insecure_tls: true` per-PiKVM because PiKVMs ship self-signed certs. If
you disabled this, you'll need to install the PiKVM's certificate locally
or re-enable the opt-in.

## Design & specs

- [specs/DESIGN.md](specs/DESIGN.md) — architecture, coord model, PiKVM
  API subset, phase plan, empirical verifications
- [PiKVM handbook](https://docs.pikvm.org/)
- [PiKVM HTTP API reference](https://docs.pikvm.org/api/)
- [kvmd source](https://github.com/pikvm/kvmd)

## Contributing

- **Empirical verifications matter.** When implementation contradicts the
  design (latency, API behavior, Qt quirks), update `specs/DESIGN.md` §10
  in the same commit that reveals it. The slots in §10 exist for exactly
  this.
- **Scope by phase.** Don't start Phase N+1 work in a Phase N PR. The phase
  plan in §9 keeps increments reviewable.
- **Tests for pure logic.** Network-heavy behavior (auth, WS) is exercised
  by the CLI harness against real hardware. Config validation, coordinate
  transform, and other pure-function code should have QTest coverage.

## License

The Glasshouse source code is licensed under
[**LGPL-3.0-or-later**](LICENSES/LGPL-3.0.txt), matching the dynamic Qt 6
linkage requirement (Qt 6 is LGPL v3 with The Qt Company's GPLv3
exception). LGPL-3.0 incorporates GPL-3.0 by reference — see
<https://www.gnu.org/licenses/gpl-3.0.html>.
