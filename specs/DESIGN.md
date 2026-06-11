# Multi-Screen PiKVM Desktop Client — Design

**Status:** v0.1.3 (latest) — Phases 0–8 complete in v0.1.0; three
patch releases since (keymap shifted-symbol routing, mouse-wheel
sub-notch accumulation, ergonomic release-hotkey default). Document
is now a running architectural reference; future work updates it in
place rather than as a phase plan.
**Target platform:** Kubuntu 24.04+ (KDE Plasma 6.x, Wayland)
**Target laptop OS:** Ubuntu / Linux

## 1. Goal

A native Linux application that replaces the pattern of multiple Chrome
tabs pointed at multiple PiKVM web UIs. The application presents **N
independently movable and resizable windows, one per PiKVM**, each
rendering a remote target monitor live. A single physical keyboard and
mouse are shared between the windows via click-to-capture with a release
hotkey. On KDE, the windows pinned to one virtual desktop make the remote
screens feel like any other KDE workspace.

`N` is configuration-driven. Current target use cases are N=2 and N=3,
but nothing in the architecture assumes a specific count.

## 2. Setup being solved for

* One **target** laptop with its own USB HID input and multiple HDMI
  outputs (e.g. one from the laptop body, two from a dock).
* **N PiKVMs**, each receiving one of the target's HDMI outputs.
* All target HDMI outputs form a single extended logical desktop on the
  target (not mirrored, not separate X/Wayland seats).
* The **client** is a Linux workstation running Kubuntu 24.04+ with one
  or more local physical monitors (local/target layouts need not match).

Validated configurations:

| N | Target HDMI sources | PiKVMs | Use case |
|---|---|---|---|
| 2 | laptop + 1 dock output | 2 | current minimum-viable setup |
| 3 | laptop + 2 dock outputs | 3 | stated goal |

## 3. The core constraint

PiKVM emulates a standard USB HID absolute mouse. When the target OS
receives an absolute HID coordinate, it maps the full coordinate range
across its **entire logical desktop** — not per-HDMI-output, not per
monitor. This was confirmed by PiKVM issue #1070 (two PiKVMs on one
target in absolute mode cannot pass the cursor seamlessly between
monitors) and by issue #1373 (feature request to allow per-PiKVM custom
mouse resolution, still open).

Target-side remapping (Wacom-style per-device monitor mapping via
libinput calibration matrices) exists in theory on KDE Plasma but is
only exposed in the UI for devices identified by libinput as graphics
tablets. PiKVM presents as a generic USB HID mouse, not a tablet, so
the Tablet KCM doesn't apply. Hand-rolled udev rules plus libinput
configuration could achieve the mapping but are fragile across upgrades
and all PiKVMs share the same device name ("PiKVM"), requiring USB-path
matching.

**Conclusion:** the coordinate math must live in the client, not rely on
target-side per-device remapping.

## 4. Architecture — Option A: one HID master, N−1 video-only

### 4.1 Physical topology

* **PiKVM A** — HDMI in from target monitor 1, USB HID **connected**
  to target. This is the **HID master**. All input (keyboard, mouse)
  flows through this PiKVM regardless of which local window is
  captured.
* **PiKVM B..N** — HDMI in from target monitors 2..N, USB HID
  **disconnected** (either unplugged, or HID gadget disabled in
  `/etc/kvmd/override.yaml`). Video-only.

The HID master is a config choice; it's typically the PiKVM plugged
into the most stable USB port on the target (laptop body rather than
dock).

### 4.2 Why one HID master

* Only one physical USB HID means only one input device contending for
  the cursor on the target — simpler, predictable.
* All PiKVMs in absolute mode would map to the same logical desktop;
  extra HID channels buy no coordinate-space independence, only
  potential failover.
* Keyboard input has no per-monitor concept, so routing keyboard
  per-window is meaningless. One HID master handles it uniformly.

### 4.3 PiKVM-side configuration

On the N−1 video-only PiKVMs, disable the HID gadget via
`/etc/kvmd/override.yaml`:

```yaml
kvmd:
    hid:
        keyboard:
            device: ""
        mouse:
            device: ""
```

Or simply leave the OTG USB cable to the target unplugged. Same effect.

On the HID master PiKVM, leave the default absolute-mode USB HID
configuration. Verify `mouse.absolute == true` in `/api/hid` at
startup.

## 5. Coordinate model

Each local window represents one target monitor. Windows are free to be
anywhere on the local desktop, any size, including all-on-one-local-monitor
or overlapping. The transform is **window-local**, not local-screen-local.

### 5.1 Configuration inputs

* **Target logical desktop**: total width and height in pixels, e.g.
  3840 × 1080 (N=2, two 1920×1080 monitors side by side) or
  5760 × 1080 (N=3, three 1920×1080 monitors side by side).
* **Target monitor rects** within that desktop (see §8 for schema).
* **Window → target monitor mapping**: which local window represents
  which target monitor.
* **Letterbox rect** per window: the sub-region of the window that
  currently holds video (after aspect-ratio fit). Cursor is only
  mapped from inside this rect; outside, cursor sits at the nearest
  letterbox edge.

### 5.2 Transform (empirically verified)

Let the captured window be W, mapped to target monitor i. Let V be the
letterbox video rect inside W. Let (sx, sy) be the local cursor position
in local screen coordinates.

```
# 1. Local cursor relative to window origin
wx = sx - W.x
wy = sy - W.y

# 2. Clamp to letterbox (cursor at letterbox edge if outside)
vx = clamp(wx - V.x, 0, V.w)
vy = clamp(wy - V.y, 0, V.h)

# 3. Normalize within letterbox: nx, ny in [0.0, 1.0]
nx = vx / V.w
ny = vy / V.h

# 4. Map to target monitor pixel coords
tx_px = monitor[i].origin.x + nx * monitor[i].size.w
ty_px = monitor[i].origin.y + ny * monitor[i].size.h

# 5. Map target pixel coords to PiKVM API coord space.
#    PiKVM API uses signed s16 [-32768, 32767] with (0, 0) at the
#    CENTER of the target's combined logical desktop. Linear mapping,
#    verified empirically (see §10.1). Round-before-bias keeps the
#    endpoints (0, center, max) on exact integers and the mapping
#    monotonic; the equivalent-looking (f − 0.5) × 65535 form rounds
#    the *wrong* side of the bias and introduces off-by-one errors at
#    25% / 75% of each axis.
to_x = clamp(round(tx_px / desktop.w * 65535) - 32768, -32768, 32767)
to_y = clamp(round(ty_px / desktop.h * 65535) - 32768, -32768, 32767)

# 6. Send via POST /api/hid/events/send_mouse_move?to_x=...&to_y=...
#    or via the WebSocket event channel (preferred for latency).
```

Worked example (N=2, desktop 3840×1080):

| Target pixel | → API coord |
|---|---|
| `(0, 0)` top-left | `(-32768, -32768)` |
| `(3840, 1080)` bottom-right | `(32767, 32767)` |
| `(1920, 540)` dead center | `(0, 0)` |
| `(2880, 540)` center of monitor 2 | `(16383, 0)` |

### 5.3 Window-switching behavior

Capture is **session-wide**, not per-window. Clicking inside any window
starts capture; that window becomes the *holder* (it owns Qt's mouse +
keyboard grab and is the anchor for event delivery). While capture is
active, the cursor is hidden across the whole app and can be moved
freely between sibling windows. On every mouse event the active
`CoordTransform` is the one belonging to whichever window the cursor is
currently over — so target-cursor motion across the windows is
**continuous**, not the per-window teleport an earlier draft of this
doc described.

That continuity is correct because every window represents a sub-region
of the same combined target desktop and every window's input goes
through the same HID master (Option A, §4.1). Walking the local cursor
from window 1's right edge into window 2's left edge maps to walking
the target cursor from monitor 1's right edge into monitor 2's left
edge — exactly the behaviour a real multi-monitor extended desktop
gives.

The release hotkey ends capture globally, regardless of which window
is the holder. Keyboard events while captured always go to the HID
master, mouse events use the cursor's current host window's transform
but still output via the HID master.

## 6. Component architecture (Qt6)

```
Main thread (QApplication)
├── KvmWindow × N  (QMainWindow subclass, one per target monitor)
│   ├── VideoSurface (QVideoWidget fed by GStreamer sink)
│   ├── Click-to-capture handler (mousePressEvent)
│   ├── Release hotkey (QShortcut, app-local)
│   └── Wayland pointer constraint on capture
├── InputRouter
│   ├── Tracks captured window / target monitor
│   └── Forwards events to CoordTransform + HidSink
├── CoordTransform
│   ├── Stateful with window rects, letterbox rects, target layout
│   └── Pure function: (local cursor, captured window) -> (to_x, to_y)
└── HidSink
    ├── Writes to HID master's outgoing WebSocket
    └── Optional REST fallback if WS disconnected

Worker threads (QThread × N, one per PiKVM)
├── PiKvmClient
│   ├── HTTPS session (QNetworkAccessManager + cookie jar)
│   ├── /api/ws state WebSocket (incoming state events, HID output)
│   └── Outgoing HID event sender (only active on HID master)
├── JanusClient
│   ├── /janus/ws signalling WebSocket (subprotocol janus-protocol)
│   ├── info → create → attach(janus.plugin.ustreamer) → watch
│   ├── SDP offer in; answer out; keepalive every 30 s
│   └── Auth via the kvmd auth_token cookie from PiKvmClient
└── VideoPipeline (GStreamer, per PiKVM)
    ├── webrtcbin (terminates the WebRTC peer, DTLS/SRTP/ICE)
    ├── rtph264depay (built dynamically on pad-added)
    ├── h264parse config-interval=-1
    ├── nvh264dec | vah264dec | avdec_h264
    ├── videoconvert
    └── qtvideosink -> KvmWindow's VideoSurface
```

Scaling from N=2 → N=3 → N=M is a config change. The only code path
that iterates on N is startup (spawn M windows + M clients + M video
pipelines, wire them into the InputRouter). All per-event logic is
parameterized.

## 7. Tech stack

| Concern | Choice | Notes |
|---|---|---|
| UI framework | Qt 6.4+ (C++) | system Qt on Ubuntu 24.04 noble; nothing post-6.4 is used |
| Video decode | GStreamer 1.24 via QtMultimedia | `vah264dec` for VA-API, auto-selected |
| Video transport | H.264 over WebRTC via Janus (`/janus/ws`, plugin `janus.plugin.ustreamer`) | The only H.264 path stock PiKVM exposes; see §10.5 |
| HTTP/WS | Qt's QNetworkAccessManager + QWebSocket | In-tree, no extra deps |
| Config | YAML via `yaml-cpp` | Hand-editable |
| Packaging | `.deb` built with `nfpm` | `dist/nfpm.yaml`; built and published by `.github/workflows/release.yml` on `v*` tag pushes |
| Build system | CMake | Qt6 convention |

### 7.1 Required Ubuntu packages

```
qt6-base-dev qt6-multimedia-dev qt6-websockets-dev
libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
gstreamer1.0-nice    # libnice wrapper — webrtcbin's ICE backend;
                     # without it, pipelines containing webrtcbin
                     # fail state-change to PLAYING
gstreamer1.0-vaapi   # or the equivalent for the `va` plugin
va-driver-all        # or intel-media-va-driver-non-free / mesa-va-drivers
libyaml-cpp-dev
```

### 7.2 VA-API verification

```bash
gst-inspect-1.0 vah264dec   # should print element details
vainfo                      # should list H.264 decode profiles
```

To force VA-API over software decode at runtime:

```bash
export GST_PLUGIN_FEATURE_RANK=vah264dec:MAX
```

## 8. Configuration schema (YAML)

The schema is list-based; N is defined implicitly by the length of
`target.monitors`, which must equal the length of the configured
`windows` (one window per target monitor). Startup validates this.

```yaml
# ~/.config/pikvm-desktop/config.yaml

target:
  # Total combined logical desktop.
  logical_desktop:
    width: 3840           # example for N=2
    height: 1080

  # One entry per target monitor. Length defines N.
  monitors:
    - id: 1
      origin: [0, 0]
      size: [1920, 1080]
      pikvm: 192.168.1.71
    - id: 2
      origin: [1920, 0]
      size: [1920, 1080]
      pikvm: 192.168.1.72
    # For N=3, add:
    # - id: 3
    #   origin: [3840, 0]
    #   size: [1920, 1080]
    #   pikvm: 192.168.1.73

# Exactly one PiKVM in target.monitors must also be hid_master.
hid_master: 192.168.1.71

auth:
  # Per-PiKVM auth. TOTP optional; if configured, the 2FA code is
  # concatenated onto the password at request time.
  192.168.1.71:
    user: admin
    password_ref: secret://pikvm-1-passwd
    totp_secret_ref: secret://pikvm-1-totp   # optional
  192.168.1.72:
    user: admin
    password_ref: secret://pikvm-2-passwd

release_hotkey:      "Ctrl+Alt+Shift+Backspace" # ends session-wide capture
fullscreen_hotkey:   "F11"                     # toggles per-window fullscreen
special_keys_hotkey: "Ctrl+Alt+K"              # shows/hides Special Keys palette

# Optional. Each entry produces a button on the palette's Custom tab,
# and the same shape will be consumed by the macro-pad daemon when it
# lands. `keys` is a list of MDN KeyboardEvent.code values — same as
# PiKVM's send_shortcut endpoint expects (see kvmd/keymap.csv).
shortcuts:
  - label: "Open terminal"
    keys: [ControlLeft, AltLeft, KeyT]
  - label: "Lock session"
    keys: [MetaLeft, KeyL]

video:
  prefer_hw_decode: true    # VA-API; software fallback on init failure
  target_fps: 30            # MJPEG only: client-side cap via a pre-decode
                            # videorate (§10.6). 0 = uncapped; no effect on Janus.

# One entry per target monitor; target_monitor must reference an id in
# target.monitors. Length must equal length of target.monitors.
windows:
  - target_monitor: 1
    geometry: { x: 0, y: 0, w: 960, h: 540 }
  - target_monitor: 2
    geometry: { x: 960, y: 0, w: 960, h: 540 }
```

Password and TOTP secrets should be loaded via KWallet or libsecret,
not inlined in the config. `secret://` indirections are placeholders
for that lookup.

### 8.1 Startup validation

The application rejects a config with:

* `len(target.monitors) != len(windows)`
* any `window.target_monitor` not referencing an existing
  `target.monitors[].id`
* `hid_master` not matching any `target.monitors[].pikvm`
* any `target.monitors[]` rect extending outside `logical_desktop`
* overlapping `target.monitors[]` rects (probably user error)

## 9. Phase plan

### Phase 0 — Empirical verification ✓ COMPLETE
Verified coordinate range and mapping via `pikvm_coord_verify.py`.
Results recorded in §10.

### Phase 1 — PiKVM client skeleton — done
One `PiKvmClient` QThread class. HTTPS auth, state WebSocket, basic
request/response for HID events. No video yet. Validates auth,
session cookie flow, 2FA concatenation, WebSocket keepalive.

### Phase 2 — GStreamer video pipeline — done
H.264 over WebSocket into `appsrc`, through
`h264parse ! vah264dec ! videoconvert ! qtvideosink`, rendered into
a `QVideoWidget`. Measure end-to-end latency (target displays a
clock, compare frame to wall clock).

### Phase 3 — Single-window capture & input — done
One window, click-to-capture, release hotkey, mouse + keyboard routing
via the coord transform. Prove the full end-to-end loop with the HID
master.

### Phase 4 — N windows + config — done
Load YAML config, spawn N windows, InputRouter routes through the
HID-master client. Validate with N=2, then test N=3 expansion as a
pure config change (no code changes).

### Phase 5 — Letterboxing + aspect ratio (half a day) — done
`computeLetterbox(widgetSize, videoSize)` in `src/input/coord_transform.cpp`
returns the rendered video rect inside a `Qt::KeepAspectRatio` widget;
`VideoWindow::transformFor` queries `QVideoSink::videoSize()` on every
event and feeds that rect (in frame-local coords) as the
`CoordTransform` letterbox. Cursor in a letterbox/pillarbox bar clamps
to the nearest video edge, matching how the video is actually painted.
Falls back to the full widget rect before the first frame arrives.

### Phase 6 — Reliability (1 day) — done

Two-tier reconnect strategy:

* **Short loop (transport-internal).** `JanusClient` retries its own WS
  open with exponential backoff (1 s → 2 s → 4 s → 8 s, cap 30 s) up
  to four attempts before escalating. Per-attempt state (sessionId,
  handleId, transaction map) is reset on each try; the auth cookie is
  re-used. The retry budget resets to zero on `sdpOfferReceived` —
  signalling reaching that point is the success criterion. `PiKvmClient`
  has had this style of reconnect since Phase 1 for the state WS.
* **Long loop (orchestrator-driven).** When `JanusClient::sessionFailed`
  fires (terminal — Janus give-up after the short loop, or pipeline
  bus-error from `VideoPipeline` / `MjpegPipeline`), `viewer/main.cpp`
  schedules `PiKvmClient::start()` after a 2 s breather. That triggers
  a fresh `/api/auth/login`, so the next pass sees a current cookie —
  this is the auth-token refresh path. The `authenticated` handler is
  idempotent: it tears down stale `JanusClient` / pipeline state at
  the top before rebuilding.

Stable sessions stay stable: the `sdpOfferReceived` reset means Phase 6
doesn't penalise long-running sessions for accumulated retry counts.

A 120 Hz mouse-move coalescer in `VideoWindow` flushes the latest
captured cursor position once per ~8 ms instead of once per Wayland
event. Wayland delivers up to 1 kHz on a high-poll mouse; kvmd's USB
HID gadget drains 125–500 Hz, so the flow used to queue server-side
and surface as cursor lag. Button presses force-flush so the
"move-then-click" order stays tight.

Graceful degradation: status-bar messages already cover "reconnecting…"
without an in-video overlay; left as-is until field testing surfaces a
specific gap.

### Phase 7 — Polish — done

**Window-position persistence — done.** `VideoWindow::closeEvent`
saves `saveGeometry()` (position, size, maximized / fullscreen flag —
one QByteArray) into `QSettings` under `windows/<host>/geometry`. On
startup main.cpp picks geometry by precedence:
explicit YAML `windows[].geometry` (override) > saved QSettings slot
(last-known) > Qt default. The QSettings file lives at
`~/.config/glasshouse/glasshouse-viewer.conf`.

**Menubar + Target ATX actions — done.** Each `VideoWindow` now hosts a
`QMenuBar` with File (Quit), View (Toggle Fullscreen, Show Special Keys),
Target (Power short press, Power long press / force off, Reset — each
gated by a `QMessageBox` confirmation), and Help (About). Target
actions emit `atxClickRequested(button)` which `main.cpp` routes
through `InputRouter` to the HID-master `PiKvmClient::atxClick`
(`POST /api/atx/click_button?button=…`). The menubar hides alongside
the status bar in fullscreen.

**File logging — done.** `glasshouse-viewer --log-file PATH` (`-L`) installs
a custom `QtMessageHandler` that delegates to the default stderr handler
(so `QT_LOGGING_RULES` filtering still works) and additionally appends
ISO-timestamped lines to PATH. Append mode, no rotation — pair with
`logrotate(8)` for long-running sessions.

**Mass Storage menu item — done.** `Target → Mass Storage…` opens
`MsdDialog`, a modal that subscribes to `PiKvmClient::rawStateEvent`
filtering for `msd_state` frames. Surfaces enabled/online/busy, free /
total storage, the active drive image, and the list of stored images.
Buttons drive the kvmd HTTP API:

- *Upload New Image…* — `QFileDialog` → `POST /api/msd/write?image=…`
  with the file streamed via QNAM (multi-GB ISOs don't materialise in
  RAM). `QProgressDialog` tracks bytes via `uploadProgress`.
- *Use Selected* — `POST /api/msd/set_params?image=…&cdrom=…&rw=…`.
- *Delete Selected* — `POST /api/msd/remove?image=…` after confirmation.
- *Connect / Disconnect* — `POST /api/msd/set_connected?connected=0|1`.
- *Present as CD-ROM* — toggles `set_params` `cdrom`/`rw` together.

MSD always targets the HID master (the assumption being that whichever
PiKVM drives the target's HID also has the MSD hardware wired) — for
mixed topologies a per-PiKVM selector is a follow-up.

**Qt distribution — decided.** The codebase audited clean against Qt 6.4
(nothing post-6.4 is in use), so the `find_package(Qt6 6.4 REQUIRED)`
pin matches what Ubuntu 24.04 noble ships in apt. CI and the .deb both
target system Qt 6.4: build deps are `qt6-base-dev / qt6-multimedia-dev
/ qt6-websockets-dev`, runtime deps are the noble `libqt6*t64` /
`libqt6multimedia6` / `libqt6websockets6` set. No bundling, no
side-by-side aqt install, no LD_LIBRARY_PATH dance. Local development
can still use a newer Qt via aqt — 6.4 is the *minimum*, not a pin.

**Systemd user-service unit — deliberately skipped.** Glasshouse Viewer
is an interactive multi-window viewer, not a daemon. KDE's
*System Settings → Startup and Shutdown → Autostart* picks up the
`.desktop` entry the .deb already installs, which is the right path
for users who actually want autostart. Re-evaluate if a "headless
always-on" deployment use case ever materialises.

### Phase 8 — Special-keys palette + clipboard paste — done
A floating, stays-on-top "Special Keys" dialog (`SpecialKeysDialog`)
that solves the cases where the local compositor swallows a keypress
before our captured window sees it (KDE Spectacle on Print Screen,
Super-key combos, etc.) and gives the user clipboard-paste-as-
keystrokes via PiKVM's `POST /api/hid/print` endpoint. Toggled by a
configurable hotkey (default `Ctrl+Alt+K`). Three tabs:

* **Keyboard** — full US-QWERTY on-screen keyboard with sticky-toggle
  modifiers (Ctrl, Shift, Alt, Win, Caps). Click a non-modifier key →
  `send_shortcut(active_modifiers + key)`.
* **Shortcuts** — curated chord buttons (Ctrl+Alt+Del, Win+L, …) plus
  a "Custom" section populated from `cfg.shortcuts` (label + keys[]).
  The same shortcut-spec schema is reused by the future macro-pad
  daemon, so a chord defined in YAML drives both the dialog button and
  whichever physical key the keypad maps to it.
* **Paste** — multiline editor + a "Type N chars" button that sends
  the buffer through `POST /api/hid/print`, with optional slow / delay
  knobs for picky BIOS prompts. Server-side keymap conversion applies,
  so the *target's* layout is what determines what gets typed.

Routing goes through the same `InputRouter` as the rest of HID, so
the dialog targets the configured HID master regardless of which
window had focus when it was opened. Opening the palette releases
the parent window's keyboard grab (Qt focus semantics) — the user
re-clicks a window to re-capture.

### Out of scope for v1
* Clipboard sync between local and target.
* Audio (microphone to target, speaker from target).
* MSD / virtual media upload (trivial to add later via existing API).
* ATX power control (trivial to add later).
* Macro keypad integration — the daemon itself is separate (planned
  to ride a local Unix domain socket). Phase 8's `cfg.shortcuts`
  schema is the shared source of truth between the dialog and a
  future keypad daemon.

## 10. Empirical verifications

### 10.1 Coordinate range and origin (verified 2026-04-25)

PiKVM used: `192.168.1.71`. Target: laptop with 2 HDMI outputs, combined
logical desktop 3840 × 1080. Observed via `evtest` on the target and
visual confirmation of cursor position.

| API call | Wire `ABS_X, ABS_Y` | Visual result |
|---|---|---|
| `(0, 0)` | `16383, 16383` | center of combined desktop |
| `(32767, 0)` | `32767, 16383` | right edge, vertical center |
| `(-32768, 0)` | `0, 16383` | left edge, vertical center |
| `(0, 32767)` | `16383, 32767` | horizontal center, bottom edge |
| `(0, -32768)` | `16383, 0` | horizontal center, top edge |
| `(-32768, -32768)` | `0, 0` | top-left of monitor 1 |
| `(32767, 32767)` | `32767, 32767` | bottom-right of combined desktop (off-screen in visible windows) |
| `(16384, 16384)` | `24575, 24575` | 75% across, 75% down (linearity confirmed) |
| `(-16384, -16384)` | `8191, 8191` | 25% across, 25% down (linearity confirmed) |
| `(65535, 0)` | `32767, 16383` | clamped to `(32767, 0)` |
| `(-65536, 0)` | `0, 16383` | clamped to `(-32768, 0)` |

**Wire relationship**: `wire = (api + 32768) / 2`. The HID descriptor
uses a 15-bit unsigned logical range, effectively halving the API's
signed s16 range on the wire. For client purposes this is transparent
— we work entirely in API coords.

**Conclusions**:
* API range: signed s16 `[-32768, 32767]` on both axes.
* `(0, 0)` is the **center** of the target's combined logical desktop.
* Mapping is linear and spans the full multi-monitor desktop.
* Values outside `[-32768, 32767]` are silently clamped by the API.
* Step `(16384, 0)` separately verified to land at the center of the
  right-hand monitor, confirming full-desktop spanning.

### 10.2 Video latency — TBD (Phase 2)
* H.264 over WebSocket end-to-end latency (clock-on-screen test):
  _____________________ ms
* VA-API decode confirmed active (via `GST_DEBUG=4` or `gst-inspect`):
  _____________________
* Software decode latency (for comparison):
  _____________________ ms

### 10.3 Qt Wayland pointer constraint — TBD (Phase 3)
* Capture/release cycle reliable on Plasma 6.x:
  _____________________
* Compositor spontaneously releases grab under any condition?
  _____________________

### 10.4 WebSocket event formats (verified 2026-04-25)

Established by two tiers of evidence: (a) a read of kvmd HEAD
(`kvmd/apps/kvmd/api/hid.py`, `kvmd/validators/hid.py`, `kvmd/mouse.py`,
`kvmd/htserver.py`, `web/share/js/kvm/{session,mouse,keyboard,hid}.js`),
and (b) empirical capture from a live Glasshouse CLI session against
two real PiKVMs (kvmd 3.106 and 3.199) logging every received text
frame. Tier (b) overrode tier (a) wherever they disagreed — `session.js`
has an abstraction layer above the wire format that normalised event
names, which misled the initial source-only investigation.

Full schemas are recorded in §13.4 (outgoing HID events) and §13.2
(incoming state events). Findings worth calling out:

* `mouse_move` event wraps coords in a nested `to` object —
  `{"event": {"to": {"x": X, "y": Y}}}`. It does **not** use flat
  `to_x`/`to_y` keys as the REST query-string form suggests.
* State-event `event_type` strings carry the `_state` suffix on the
  wire (`hid_state`, `atx_state`, `streamer_state`, etc.), plus
  `gpio_model_state`, `hid_keymaps_state`, `streamer_ocr_state`, and
  per-subsystem `info_*_state` splits (`info_auth_state`,
  `info_extras_state`, `info_fan_state`, `info_hw_state`,
  `info_meta_state`, `info_system_state`). `session.js` dispatches on
  normalised *bare* names, so reading only the JS gave a wrong picture.
* The initial burst terminates with a `loop` event (empty `event`
  object). This is the correct ready-for-use marker for a new session.
* Server periodic broadcasts (`info_fan_state`, etc.) are sent to all
  connected clients and can arrive on a freshly-opened WS before its
  own initial burst starts, so the first event received is **not**
  reliably the start of this session's burst. Only `loop` is reliable.
* There is **no `shortcut` event on the WS**. Shortcuts must be
  synthesised client-side as an ordered `key` press/release sequence
  (matches the reference web UI).
* Malformed or out-of-schema WS frames are **silently dropped** by the
  server — no ack, no error response. Plan for "send and hope" on the
  HID path. Numeric out-of-range is clamped (same as REST).
* The server accepts `{"event_type": "ping", "event": {}}` and replies
  with `{"event_type": "pong", "event": {}}`. Independent of the WS
  protocol ping; proves the aiohttp JSON handler is alive. The client
  pings every 15 s while the WS is open.

### 10.5 Video transport: Janus/WebRTC (verified 2026-04-25)

The previous draft of this section described a raw-H.264-over-WebSocket
transport via `kvmd-media` at `/api/media/ws`. That turned out to be
wrong in practice: stock PiKVM firmware does not install `kvmd-media`,
so `/api/media/ws` returns HTTP 404 on the upgrade. The only H.264 path
exposed by a stock PiKVM is the Janus WebRTC gateway at `/janus/ws`,
which is also what the reference web UI uses. This client now targets
that path exclusively.

* Endpoint: `WSS /janus/ws`. WebSocket subprotocol **`janus-protocol`**
  — must be offered on the upgrade via `Sec-WebSocket-Protocol`, or the
  server rejects the handshake. Auth: the same `auth_token` cookie kvmd
  issues for `/api/ws`; shared across both sockets from a single login.
* Signalling is the standard Janus JSON-RPC flow:
  `info → create → attach(janus.plugin.ustreamer) → message{request:"watch"}`.
  The plugin replies with an async `event` carrying a JSEP SDP offer.
  Client answers with `message{request:"start"}` and the JSEP answer.
  `{"janus":"keepalive", session_id:S}` every 30 s (Janus defaults to a
  60 s session-inactivity timeout).
* Plugin name is `janus.plugin.ustreamer` on stock PiKVM. A single
  `{"janus":"info"}` request returns the plugin registry, so the probe
  (`bin/glasshouse-janusprobe`) logs the full list — if a variant image
  ships a different name, it's trivial to adjust.
* ICE is **vanilla** (not trickle): the offer includes all candidates
  inline with `a=end-of-candidates`. On our side, we defer emitting the
  answer until webrtcbin's `ice-gathering-state` transitions to
  `COMPLETE`, then send the final SDP (with our gathered candidates) in
  a single `start` message.
* Media arrives as a single RTP H.264 track (BUNDLE-max-bundle). The
  decode chain hangs off `webrtcbin`'s `pad-added` signal:
  `queue ! rtph264depay ! h264parse config-interval=-1 ! <decoder> !
  videoconvert ! video/x-raw,format=BGRA ! appsink`. `config-interval=-1`
  re-inserts SPS/PPS before every IDR so decoders that lose their
  stream header on concealment still recover.
* If you see "probe received SDP offer then the server closed" during
  the signalling probe: normal. The ustreamer plugin drops handles that
  never answer. The viewer path answers promptly, so the session stays
  live.
* **Encoder warmup via the kvmd state WS (verified 2026-04-27).** The
  Janus plugin only emits H.264 RTP when the `ustreamer` process is
  actually running. On a stock PiKVM with `stream_forever=false`
  (the default), kvmd spawns `ustreamer` lazily: its stream-controller
  spawns the encoder on the False→True edge of "at least one client
  has the state WS open with `?stream=true`" (kvmd
  `apps/kvmd/server.py` `_on_ws_added` → `__stream_controller`). If
  no client is watching, attaching `janus.plugin.ustreamer` and
  sending `watch` succeeds at the signalling layer — Janus completes
  ICE+DTLS, emits `webrtcup` — but no SPS/PPS ever shows up at the
  memsink, so no RTP is forwarded and `webrtcbin` never fires
  `pad-added`. Older ustreamer surfaces this as
  `{"error_code":503,"error":"Haven't received SPS/PPS from memsink yet"}`
  on the watch reply (seen on PiKVM 3 / kvmd 3.106). Newer ustreamer
  silently drops the SDP-build SPS/PPS check, so the failure mode on
  a current PiKVM 4 Plus is "signalling completes, video stays
  black". **Mitigation:** open the kvmd state WS at
  `wss://<host>/api/ws?stream=1` and keep it open for the lifetime
  of the session. That single bit is what wakes the encoder; with it
  set, the cold-start Janus handshake just works on either firmware.
* **Cold-start retry on 503 (older firmware).** If the watch reply
  carries `error_code:503` (older ustreamer pre-warming), the kvmd web
  UI re-issues `stop`+`watch` after a 2 s timer until the plugin
  accepts it. Glasshouse should do the same; the state-WS wake covers
  the common case but the encoder may take a beat to produce its
  first IDR after spawning.
* **`webrtcbin` long-running leak (open 2026-04-27).** GStreamer's
  `webrtcbin` (1.24.2) leaks at a steady **~8 MB/s/stream** as long
  as media is flowing. With two PiKVMs at 1080p the process's RSS
  climbed past 67 GB in ~3 hours of normal use, swap-thrashed the
  host, and pushed load average past 200. Bisection (2026-04-27):
  * Replacing the decode chain with `fakesink` immediately on
    `pad-added` → leak persists at the same rate, so the decoder,
    `videoconvert`, and our Qt-side `QVideoFrame` path are
    exonerated.
  * `MALLOC_ARENA_MAX=2` → no change, so it's not glibc arena
    fragmentation.
  * `webrtcbin` `latency=50` (down from default 200) → no change.
  * Periodic `JanusClient + VideoPipeline` teardown+rebuild +
    `malloc_trim(0)` in the `Impl` destructor → **no change**.
    The bin's destructor runs and unrefs as expected, but the
    leaked allocations are *true unfreed pointers* that survive
    `gst_object_unref` on the bin. Process RSS is the only thing
    that can recover them.
  Conclusion: the leak is inside `webrtcbin` / its embedded
  `rtpbin` / `libnice` / SRTP / DTLS subgraph and cannot be
  mitigated at the GstElement teardown layer. Needs an upstream
  bug report against `gst-plugins-bad`. Until then:
  * **Recommended workaround**: switch the affected PiKVM to
    `transport: mjpeg`. The MJPEG path uses
    `souphttpsrc → multipartdemux → jpegdec` and never
    instantiates `webrtcbin`, so it doesn't leak. Costs more
    bandwidth and slightly more CPU than H.264 but is otherwise
    equivalent for KVM use.
  * **Process-restart mitigation** (out of scope for this client):
    a watchdog that respawns `glasshouse-viewer` every few hours
    is the only currently-known way to bound RSS while keeping
    the Janus transport.
* Sources of truth (both checked 2026-04-25, no docs.pikvm.org coverage):
  * `pikvm/ustreamer` — `janus/src/plugin.c` for the plugin's accepted
    request verbs (`watch`, `start`, `stop`).
  * `pikvm/kvmd` — `web/share/js/kvm/stream_janus.js` for the UI's
    signalling order.
  Both are private-but-stable protocols; check those files first if
  the session starts refusing the configured plugin or the offer shape
  changes.

### 10.6 MJPEG transport CPU profile (verified 2026-06-12)

Two 1080p PiKVMs on `transport: mjpeg` were observed at ~150% CPU in a
single `glasshouse-viewer` process. Investigated against the live PiKVM 4
Plus (`192.168.1.71`, kvmd 3.199, ustreamer 5.37) on an NVIDIA RTX 3070 Ti
client.

* **Where the CPU goes** (per-thread, streams flowing): ~70% main thread
  (QVideoWidget render of two 1080p BGRA frames), ~40% across the
  `souphttpsrc` streaming threads (HTTPS receive + TLS + multipart demux),
  ~30% software `jpegdec` + `videoconvert`.
* **ustreamer emits 4:2:2 baseline JPEG** (`2x1,1x1,1x1` sampling, SOF0),
  1920×1080 — confirmed by carving a frame off `/streamer/stream`.
* **Hardware JPEG decode is not viable for this stream.** NVIDIA's
  `nvjpegdec` decodes a synthetic 4:2:0 JPEG but rejects ustreamer's real
  4:2:2 frames with `not-negotiated (-4)` (pipeline reaches PLAYING, dies
  on the first buffer) — with or without `jpegparse` / `cudadownload`.
  `vajpegdec` / `v4l2jpegdec` aren't present on NVIDIA at all. `jpegdec`
  (software) is the only decoder that handles the real stream. **Do not
  re-add `nvjpegdec` to the MJPEG path** expecting it to work on stock
  PiKVM output.
* **Client-side frame-rate cap.** `video.target_fps` (previously parsed but
  unused) now inserts `jpegparse ! videorate max-rate=N` *before* jpegdec.
  Dropping surplus frames while still encoded makes the cap cut software
  decode, colour-convert, and main-thread render together. Measured linear:
  a synthetic 1080p 4:2:2 stream costs 18% CPU at 60fps vs 9% at a 30fps
  cap (decode+convert; render scales the same way). `max-rate` implies
  drop-only, so a static screen (ustreamer `drop-same-frames`) costs
  nothing.
* **The `souphttpsrc` receive cost (~40%) is a floor** the client can't
  shrink: every frame the server sends is received and TLS-decrypted before
  any client-side drop point. kvmd here advertises `desired_fps: 40`, so a
  30fps cap trims a flowing stream ~25%; lower the cap for more.
* **For genuinely low decode CPU the answer is H.264, not HW JPEG.**
  `transport: janus` with `nvh264dec` is hardware-decoded and ~10× less
  bandwidth, but carries the §10.5 `webrtcbin` leak. The MJPEG fps cap is
  the leak-free middle ground.

## 11. Risks and mitigations

| Risk | Status | Mitigation |
|---|---|---|
| PiKVM coord range differs from assumed s16 | **resolved** | Verified empirically in §10.1 |
| WebRTC negotiation latency | open | Measure auth-to-first-frame; if ICE gathering dominates, set `stun-server=NULL` on webrtcbin for LAN-only deployments |
| KDE Wayland grab quirks | open | App-local hotkey avoids global-shortcut issues; test on real hardware in Phase 3 |
| HID master PiKVM fails | open | Manual failover (re-cable, update config). Auto-failover out of scope v1 |
| Dock disconnect re-orders target monitors | open | Calibration flow or manual reassignment in config |
| Growing from N=2 to N=3 later | **resolved** | Config-only change; architecture is list-parameterized |
| `webrtcbin` long-running leak (~8 MB/s/stream) | open (worked around) | Use `transport: mjpeg` for affected PiKVMs — the leak is genuinely inside `webrtcbin`/`libnice` and survives pipeline teardown. Needs upstream bug. See §10.5 |

## 12. References

* PiKVM handbook: <https://docs.pikvm.org/>
* PiKVM HTTP API: <https://docs.pikvm.org/api/>
* kvmd source: <https://github.com/pikvm/kvmd>
* ustreamer: <https://github.com/pikvm/ustreamer>
* Relevant issues:
  * #1070 — two PiKVMs, absolute mode across monitors
  * #1373 — custom mouse resolution request
  * #1017 — multi-monitor consolidation
* GStreamer `va` plugin:
  <https://gstreamer.freedesktop.org/documentation/va/>
* Qt6 QtMultimedia GStreamer backend:
  <https://doc.qt.io/qt-6/qtmultimedia-gstreamer.html>

## 13. Appendix — PiKVM API subset

Only the handles this client depends on:

### 13.1 Authentication
* `POST /api/auth/login` — form body `user=...&passwd=...`; returns
  `Set-Cookie: auth_token=...`.
* Header alternative: `X-KVMD-User`, `X-KVMD-Passwd` on every request.
* 2FA: concatenate TOTP onto password (no separator).

### 13.2 State
* `GET /api/info?fields=hw,system,meta` — platform identification.
* `GET /api/hid` — current HID state; verify `mouse.absolute == true`
  on the HID master at startup.
* `WSS /api/ws?stream=0` — state event stream; `stream=0` means "do
  not count me toward the video client count". The initial burst emits
  one broadcast per subsystem, all with `_state`-suffixed event types
  — `hid_state`, `atx_state`, `msd_state`, `streamer_state`,
  `gpio_state`, `gpio_model_state`, `hid_keymaps_state`,
  `streamer_ocr_state`, `info_auth_state`, `info_extras_state`,
  `info_fan_state`, `info_hw_state`, `info_meta_state`,
  `info_system_state` — and terminates with a `loop` event (empty
  `event` object). Subsequent events reuse the same event-type strings
  and carry deltas. Server periodic broadcasts (e.g. `info_fan_state`
  every few seconds) may arrive interleaved with a fresh session's
  initial burst, so do not assume a session's first event on the stream
  is the start of *its* burst. Verified empirically 2026-04-25 against
  kvmd 3.106 and 3.199; see §10.4.

### 13.3 HID input
* `POST /api/hid/events/send_mouse_move?to_x=X&to_y=Y` — absolute
  move. Accepts signed s16 `[-32768, 32767]` for both axes; `(0, 0)`
  is the center of the target's combined logical desktop. Values
  outside range are silently clamped.
* `POST /api/hid/events/send_mouse_button?button=left&state=1` —
  button state change. Buttons: `left`, `middle`, `right`, `up`,
  `down`.
* `POST /api/hid/events/send_mouse_wheel?delta_x=0&delta_y=N` —
  wheel scroll.
* `POST /api/hid/events/send_key?key=KeyA&state=1&finish=1` — single
  key with auto-release safety via `finish=1`.
* `POST /api/hid/events/send_shortcut?keys=ControlLeft,KeyC` — chord.
* Key names come from the `web_name` column of `kvmd/keymap.csv`,
  matching MDN `KeyboardEvent.code` values.

### 13.4 WebSocket input (lower latency than REST)

Sent as **TEXT** frames on the same `/api/ws?stream=0` connection used
for state. (The server also accepts BINARY frames with per-event
opcodes — the PiKVM web UI uses binary; we use TEXT for readability.)
Validated at HEAD of `pikvm/kvmd` on 2026-04-25 against
`kvmd/apps/kvmd/api/hid.py`, `kvmd/validators/hid.py`, `kvmd/mouse.py`,
`kvmd/htserver.py`, and `web/share/js/kvm/{session,mouse,keyboard}.js`.

All frames share the envelope `{"event_type": <str>, "event": <obj>}`.
The server **silently drops** malformed, out-of-schema, or unknown
events — there is no ack or error response on the WS path. Numeric
values outside the accepted ranges are **clamped**, not rejected
(matching the REST endpoints).

#### `key`

```json
{"event_type": "key", "event": {"key": "Enter", "state": true, "finish": false}}
```

* `key` — string, case-sensitive DOM `KeyboardEvent.code` value
  (`"KeyA"`, `"ControlLeft"`, `"F1"`, ...). Source of truth is the
  `WEB_TO_EVDEV` table in `kvmd/keyboard/mappings.py`.
* `state` — bool; `true` = pressed, `false` = released.
* `finish` — bool, optional (default `false`). Bad-link safety: when
  `true`, the HID plugin auto-releases non-modifier keys immediately
  after the press. Same semantics as `finish=1` on the REST endpoint.

#### `mouse_button`

```json
{"event_type": "mouse_button", "event": {"button": "left", "state": true}}
```

* `button` — one of `"left"`, `"right"`, `"middle"`, `"up"`, `"down"`.
  `"up"` / `"down"` are the 4th/5th buttons (browser back/forward),
  **not** wheel directions.
* `state` — bool. Press and release are two separate events; the WS
  has no auto-release helper (unlike the REST endpoint, which
  synthesises click-and-release when `state` is omitted).

#### `mouse_move` (absolute)

```json
{"event_type": "mouse_move", "event": {"to": {"x": 0, "y": 0}}}
```

* `to.x`, `to.y` — signed s16 `[-32768, 32767]`. Same coord space as
  the REST `send_mouse_move` endpoint (§5.2, §10.1). Note the nested
  `to` object — this differs from the REST query-string form
  `?to_x=X&to_y=Y`.
* Independent of `mouse_button`; no prior press required.
* If the active HID output is not absolute-mode, the move is silently
  ignored by the plugin layer (no WS-level check).

#### `mouse_relative`

```json
{"event_type": "mouse_relative",
 "event": {"delta": {"x": 3, "y": -2}, "squash": false}}
```

Batched form (one gadget report per axis, not one per delta):

```json
{"event_type": "mouse_relative",
 "event": {"delta": [{"x": 3, "y": -2}, {"x": 1, "y": 0}], "squash": true}}
```

* `delta` — either an object `{x, y}` or a list of them. Each
  component is signed s8 `[-127, 127]`, clamped.
* `squash` — bool, optional (default `false`). WS-only batching hint.

#### `mouse_wheel`

Same payload shape as `mouse_relative`; only `event_type` differs and
the server routes it to a different HID method.

```json
{"event_type": "mouse_wheel", "event": {"delta": {"x": 0, "y": -5}}}
```

Sign convention follows the reference UI: scrolling the wheel away
from the user yields **positive** Y, scrolling left yields positive X.

#### No `shortcut` event on the WS

The REST `POST /api/hid/events/send_shortcut?keys=...` has no WS
equivalent. This client (and the reference UI) synthesises shortcuts
by emitting `key` events: press all keys in order, then release in
reverse. See `PiKvmClient::sendShortcut`.

#### kvmd-level keepalive

```json
// client -> server
{"event_type": "ping", "event": {}}
// server -> client
{"event_type": "pong", "event": {}}
```

Independent of the WS-protocol ping frame. Exercises the aiohttp JSON
handler, so a pong proves end-to-end kvmd liveness, not just TCP/WS
framing. The client sends one every 15 s while the WS is open.

### 13.5 Video — Janus/WebRTC

PiKVM ships a Janus Gateway fronted by nginx at `/janus/ws`, with the
`janus.plugin.ustreamer` plugin bridging ustreamer's H.264 output into
an RTP+SRTP WebRTC track. This is the only H.264 transport exposed by
stock firmware. The handbook at docs.pikvm.org does not document it;
sources of truth are `pikvm/ustreamer/janus/src/plugin.c` and
`pikvm/kvmd/web/share/js/kvm/stream_janus.js` (both checked 2026-04-25).

* Endpoint: `WSS /janus/ws`.
* Subprotocol: `Sec-WebSocket-Protocol: janus-protocol` is **required**
  on the upgrade. QWebSocket exposes this via
  `QWebSocketHandshakeOptions::setSubprotocols`.
* Authentication: the kvmd `auth_token` cookie (same cookie `/api/ws`
  uses). No separate Janus `api_secret` is required on stock PiKVM —
  nginx gates the path before Janus sees it.

Signalling transactions used (all JSON over the same WS):

1. Client → server, TEXT (discovery; optional but cheap):

   ```json
   {"janus":"info","transaction":"t1"}
   ```

   Server replies with `{"janus":"server_info", plugins:{…}}`.
   Useful for logging the plugin registry if an image variant renames
   `janus.plugin.ustreamer`.

2. Client → server, TEXT:

   ```json
   {"janus":"create","transaction":"t2"}
   ```

   Reply `{"janus":"success","data":{"id":S}}` — keep `S` as
   `session_id` for all subsequent frames.

3. Client → server, TEXT:

   ```json
   {"janus":"attach","plugin":"janus.plugin.ustreamer",
    "session_id":S,"transaction":"t3"}
   ```

   Reply `{"janus":"success","data":{"id":H}}` — keep `H` as
   `handle_id`.

4. Client → server, TEXT:

   ```json
   {"janus":"message","body":{"request":"watch"},
    "session_id":S,"handle_id":H,"transaction":"t4"}
   ```

   Two replies: a synchronous `{"janus":"ack"}` and, asynchronously,
   `{"janus":"event", plugindata:{…}, jsep:{"type":"offer","sdp":"…"}}`.
   The SDP offer is vanilla ICE — all `a=candidate` lines are already
   inline and the block terminates with `a=end-of-candidates`, so no
   trickle path is needed from the server side.

5. Client → server, TEXT:

   ```json
   {"janus":"message","body":{"request":"start"},
    "jsep":{"type":"answer","sdp":"…"},
    "session_id":S,"handle_id":H,"transaction":"t5"}
   ```

   Our answer. Emit it only once `webrtcbin`'s `ice-gathering-state`
   has reached `COMPLETE`, so the SDP carries all local candidates and
   Janus has no reason to wait on trickle.

6. Client → server, TEXT, every 30 s:

   ```json
   {"janus":"keepalive","session_id":S,"transaction":"t…"}
   ```

   Without it Janus tears the session down after ~60 s of silence.

After step 5, media flows as SRTP inside the WebRTC transport that
`webrtcbin` terminates — nothing further rides the signalling WS for
video frames.

Alternatives on the same PiKVM (not used by this client):

* `GET /streamer/stream?key=…` — MJPEG via `multipart/x-mixed-replace`.
  Higher bandwidth, higher latency, simpler plumbing. Retained as a
  potential debug path if WebRTC negotiation fails.
* `WSS /api/media/ws` — the raw-H.264-over-WS path served by
  `kvmd-media`. Does not exist on stock PiKVM (the package is not
  installed); a 404 on the upgrade is the standard symptom and is not
  worth investigating further on a stock box.
