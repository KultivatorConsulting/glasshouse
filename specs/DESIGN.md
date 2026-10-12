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

# NOTE: steps 4–6 only ever use each monitor's *share* of the logical
# desktop — the target's real pixel resolution never enters the maths.
# A uniform resolution change across all monitors is therefore harmless;
# a non-uniform one, or a reordering, breaks the mapping. See §10.8.
#
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

A configurable hotkey (`paste_hotkey`, default `Ctrl+Alt+Shift+V`)
drives the same endpoint straight from the host clipboard, which is
the common case and avoids dropping the capture to visit a dialog.
See §10.9 for why the modifiers have to be released first and why
the chunks have to be sequenced.

Routing goes through the same `InputRouter` as the rest of HID, so
the dialog targets the configured HID master regardless of which
window had focus when it was opened. Opening the palette releases
the parent window's keyboard grab (Qt focus semantics) — the user
re-clicks a window to re-capture.

### Phase 9 — Region capture to text — done
A hotkey (`region_hotkey`, default `Ctrl+Alt+Shift+C`) freezes the
current video frame of the window **under the pointer**, lets the user
drag out a rectangle, has it turned into text by the configured
provider, and puts the result on the host clipboard. `RegionSelector`
owns the drag; a `TextExtractor` owns the request; `main.cpp` joins them
to the clipboard.

The window under the pointer is deliberately not the focused window:
while captured, the hotkey is delivered to whichever window holds the
grabs, which is rarely the one the user is looking at.

Configuration lives under `ocr:` — endpoint, model, `max_tokens`,
`reasoning_effort`, timeout and the prompt are all overridable, and
`api_key` is a `secret://` reference so the key stays out of
`config.yaml`. The default prompt asks for a verbatim transcription,
because the result goes straight to the clipboard and a description of
the screen is not something you can paste. Point `prompt` at something
else to get something else. Without a resolvable key the selector still
works and says what is missing.

Extraction is a model round trip and can run for many seconds, so the
window shows a spinner with an elapsed-seconds count and puts a busy
cursor under the pointer for the duration; pressing the hotkey again
abandons the request. See §10.10.

### Phase 10 — Kahu harness — started (MCP server built)
An assistant that watches the target over the PiKVMs, judges what it
sees against conversation context, and reports in a chat window the
user opens when they want it — proposing rather than acting, with
word-described screen watchers and an optional host notification for
the few events worth chasing the user over.

Phases 8 and 9 turn out to be its primitives: `/api/hid/print` is the
act path, and region capture is the observe path.

**First increment is in** (§10.11): a local MCP server exposing the
session, screenshots, typing, key chords and pointer control. That is the
tier-2 consumer path from HARNESS.md §8.1 — point any MCP client at it and
the surface is usable before the harness exists. Still unbuilt: the watcher
engine (§5 and §5.4 of HARNESS.md) and everything behind it.

**Only part of it belongs to this repository.** The watcher engine and
the MCP server must live in the viewer — detection runs on every decoded
frame, so a process boundary would mean moving frames across IPC — and
neither depends on Kahu. The Tauri shell and the proprietary Kahu
backend ship as a separate private package, `glasshouse-harness`,
depending on this one. HARNESS.md §9 has the split and why the licence
alone would force it.

Design captured separately in **[HARNESS.md](HARNESS.md)** — it is
architecture for unbuilt work, and this document records what exists.

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

### 10.2 Video latency (investigated 2026-06-16)

The viewer accumulated >1 s of glass-to-glass lag. Root cause was
**unbounded client-side buffering**, not the codec or the network — and
it stayed invisible because nothing measured steady-state latency (only
auth-to-first-frame). Three reservoirs, all client-side:

* **Janus/H.264** — the pad-added `queue` was untuned, inheriting
  GStreamer's defaults (`max-size-time=1s`, non-leaky). A decode stall
  filled it to a full second that never drained. Now bounded to a few
  buffers, non-leaky (`videopipeline.cpp`). Dropping *encoded* H.264
  mid-GOP would corrupt decode until the next IDR, so loss is left to
  webrtcbin's jitterbuffer — whose dwell is now configurable via
  `video.webrtc_latency_ms` (default 100 ms, down from webrtcbin's 200).
* **MJPEG** — ustreamer coalesces to the latest frame server-side
  (`ustreamer/http/server.c`), so a slow client backs frames up in
  `souphttpsrc` + the kernel socket; lag grows until TCP backpressure
  throttles the server. A `queue leaky=downstream max-size-buffers=2`
  after `multipartdemux` is now the deterministic dropper (MJPEG is
  all-intra, so dropping any frame is safe). `videorate` is *not* a
  reliable dropper here — see §10.6.
* **Both** — every decoded frame was posted to the GUI thread via a
  queued connection (unbounded Qt event queue). A `FrameMailbox` now
  keeps one delivery in flight and coalesces to the newest frame.

**Instrumentation.** The status bar shows effective render FPS and the
GUI-thread coalesced-drop count (`video: 24.0 fps · N coalesced`), polled
at 1 Hz from per-pipeline counters; the same line logs under
`glasshouse.video.debug`. A rising coalesced count under load is the
direct signal that shedding is holding latency bounded instead of letting
it grow.

**True glass-to-glass — manual clock-on-screen procedure** (the only
honest way to a millisecond number; not automatable client-side because
ustreamer exposes no per-frame capture timestamp the GStreamer path can
read):
1. Run a millisecond stopwatch full-screen on the *target* (an online ms
   timer, or `while :; do date +%H:%M:%S.%3N; done`).
2. Photograph the target's real HDMI output and the Glasshouse window in
   one shot (phone camera).
3. Latency = displayed-target-time − displayed-viewer-time; average a few
   shots. Measure on the real LAN — Wi-Fi between client and PiKVM
   dominates everything else.

* H.264/Janus glass-to-glass: _____________________ ms
* MJPEG glass-to-glass:       _____________________ ms
* HW decode active (`GST_DEBUG=4` / status-bar decoder label):
  _____________________

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
  stream header on concealment still recover. The `queue` is bounded
  (non-leaky, a few buffers) rather than left at its 1 s default — see
  §10.2 for why dropping *encoded* H.264 here is unsafe and loss is left
  to the jitterbuffer instead.
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
* **Client-side frame-rate cap — removed (2026-06-17).** `video.target_fps`
  used to insert `jpegparse ! videorate max-rate=N` before jpegdec, but on
  ustreamer's untimestamped multipartdemux output `videorate` never actually
  capped (it decides drop/dup from buffer PTS — GStreamer #720104), only
  added a frame of latency, and froze the stream outright when given
  `drop-only=true` (passed the first frame, dropped the rest — caught via the
  delivered/coalesced telemetry). It's gone; jpegdec decodes the demuxed
  JPEGs directly and the `queue leaky=downstream max-size-buffers=1` ahead of
  it governs frames (dropping stale ones under load). `video.target_fps` is
  no longer enforced on MJPEG; a real CPU-oriented fps cap would need a
  timestamp-independent dropper — separate work.
* **The `souphttpsrc` receive cost (~40%) is a floor** the client can't
  shrink: every frame the server sends is received and TLS-decrypted before
  any client-side drop point. kvmd here advertises `desired_fps: 40`, so a
  30fps cap trims a flowing stream ~25%; lower the cap for more.
* **For genuinely low decode CPU the answer is H.264, not HW JPEG.**
  `transport: janus` with `nvh264dec` is hardware-decoded and ~10× less
  bandwidth, but carries the §10.5 `webrtcbin` leak. The MJPEG fps cap is
  the leak-free middle ground.

### 10.7 Qt delivers wheel events without consulting the mouse grabber (verified 2026-10-11)

Scrolling only reached the target when the cursor was over the window that
had started the capture. Mouse moves, buttons and keys all crossed windows
correctly; the wheel was the lone exception.

**Cause.** Qt routes `QWheelEvent` through
`QWidgetWindow::handleWheelEvent`, which delivers to
`rootWidget->childAt(pos)` — falling back to the root widget when `childAt`
returns null — and **never consults `QWidget::mouseGrabber()`**. Mouse
moves and buttons go through `QApplicationPrivate::pickMouseReceiver`
instead, which does honour the grab. That asymmetry is the whole bug.

Verified with a two-window Qt 6.7.3 probe driving
`QWindowSystemInterface::handleWheelEvent` directly, with the event filter
installed on the child widget exactly as `VideoWindow` had it:

| Wheel delivered to | Position | Lands on |
|---|---|---|
| holder's `QWindow` | inside the holder | holder's video widget ✓ |
| holder's `QWindow` | outside the holder (what an X11 pointer grab produces when the cursor is over a sibling) | holder's **QMainWindow root** — the video-widget filter never fires |
| sibling's `QWindow` | inside the sibling | **sibling's** video widget — *not* redirected to the grabber |

So the scroll was lost either way: the holder's filter wasn't reached, or
the sibling's `handleWheel` early-returned on its own `m_captured == false`.

**Fix.** Two changes in `videowindow.cpp`, both tiny:
* install the event filter on the `QMainWindow` as well as the video widget,
  and accept `QEvent::Wheel` from either — non-wheel events stay scoped to
  `watched == m_video`, because `handleMouseButton` tests against
  `m_video->rect()`, which is 0-based, and a menubar click would otherwise
  pass that test and start capture;
* gate `handleWheel` on the **session** (`m_captured || m_sessionCaptured`)
  rather than on this window owning the grab. The wheel notch carries no
  position — the target scrolls wherever its cursor already is, put there
  by the move stream — and every window's `mouseWheel` signal already fans
  into the same HID master, so any session window may emit it.

Exactly one of the two filters fires per event (`handleWheel` returns true
while captured, which stops propagation), so there is no double-scroll.

**Deliberately not done: moving the Qt grab to the window under the
cursor.** It would also work, but it means `releaseMouse`/`grabMouse` churn
on every window crossing, and grab manipulation during capture is what
froze input twice during the §10.2 confinement work. It buys nothing
either — moves, clicks and keys were already session-wide. The capture
badge follows the cursor via plain state (`captureStatusChanged`), with no
grab movement.

Verified end-to-end against the real `VideoWindow` class (two windows,
capture started by a synthetic click, wheel delivered all three ways):
one `mouseWheel` emission per event in every case, zero before capture and
zero after release, and the badge tracking the cursor onto the sibling.

### 10.8 Target layout drift (verified 2026-10-11)

**The coord transform is purely proportional.** Step 4–6 of §5.2 reduce to
each monitor's *share* of the logical desktop:
`origin.x / desktop.w` and `size.w / desktop.w`. The target's real pixel
resolution never enters the maths — only the config's ratios do.

Consequences, worth internalising before debugging a "mouse is on the wrong
screen" report:

* **Uniform rescaling is harmless.** Two 1920×1080 monitors becoming two
  1280×720 ones leaves every ratio unchanged, so the mapping stays correct
  with a stale config. Same for a width scale that differs from the height
  scale, as long as all monitors share it.
* **Non-uniform rescaling breaks it.** One monitor changing while another
  doesn't moves the shares: a 1920×1080 + 1280×720 pair really splits the
  desktop 60/40, but a config still claiming 50/50 maps the left part of
  window 2 onto monitor 1.
* **Reordering breaks it and cannot be detected.** PiKVM has no visibility
  into how the target composes its desktop, so a left/right swap is
  invisible from the client. (Observed 2026-10-11: the target's GNOME
  session reverted to defaults, putting the right-hand PiKVM's monitor on
  the left at 1280×720.)

**Detection shipped.** Each PiKVM's capture resolution is the decoded video
size (`QVideoSink::videoSize()`, already used for the letterbox). `main.cpp`
watches `videoSizeChanged` on every window, puts the live resolution in the
status bar, and compares the per-monitor scale factors:

* all monitors share one width scale and one height scale → `INFO`, mapping
  unaffected, status bar shows `res: 1280×720 (config 1920×1080)`;
* scales disagree → `WARNING` naming every host's actual vs configured size,
  and a `⚠` on each status bar.

The warning also points out that a reordering looks identical from here, so
the target's arrangement is worth checking too.

**Not done: auto-adapting the layout.** Recomputing origins and
`logical_desktop` from the actual resolutions (keeping the configured
left-to-right order) would fix the non-uniform case without user action, but
it is a heuristic — it can't reproduce gaps, vertical alignment when heights
differ, or any reordering. Deferred in favour of saying clearly what
changed.

### 10.9 Clipboard paste by hotkey (verified 2026-10-11)

`POST /api/hid/print` already did the hard part — kvmd types a UTF-8 body
through the target's own server-side keymap, so the client needs no
character-to-scancode table and no pacing loop. Driving it from a hotkey
instead of a dialog surfaced three things that the dialog had hidden.

**The chord's own modifiers are held down on the target.** Pressing
`Ctrl+Alt+Shift+V` while captured means those three key-downs have already
been forwarded, and kvmd has no idea the client is about to ask it to type.
Left alone, the whole clipboard arrives as `Ctrl+Alt+Shift` chords. The fix
is the blanket modifier release `stopCapture()` already used for the
mid-chord case (eight `…Left`/`…Right` key-ups, no-ops server-side if the
key wasn't down), now shared as `releaseHeldModifiers()`.

**Which route delivers the hotkey depends on whether the grab took.** Two
paths can reach it: the `QShortcut` installed in `setCaptureContext`, and
`handleKey` via the event filter. The existing hotkeys tolerate either, and
tolerate *both*, because toggling fullscreen or a dialog twice is a no-op.
Paste is not idempotent — a second request is the abort — so route matters.
Measured with a harness driving the real `VideoWindow` through
`QWindowSystemInterface`: exactly one of the two fires in each state, the
`QShortcut` while uncaptured and `handleKey` once `grabKeyboard` is active.
But the harness runs offscreen, where the grab is a silent no-op, and there
the `QShortcut` won *while captured* — skipping the modifier release. Rather
than depend on the grab having taken, both routes now call
`requestClipboardPaste()`, which owns the release. Verified order while
captured:

| | emitted |
|---|---|
| chord down | `ControlLeft↓ AltLeft↓ ShiftLeft↓` |
| hotkey recognised | `ShiftLeft↑ ShiftRight↑ ControlLeft↑ ControlRight↑ AltLeft↑ AltRight↑ MetaLeft↑ MetaRight↑` |
| then | paste requested |

The `V` itself is swallowed, never forwarded.

**The chunks raced.** The server caps a request at 1024 characters, so longer
text is split — but the original loop posted every chunk immediately. Qt
opens up to six connections per host, so the chunks ran their TLS handshakes
in parallel and reached kvmd in completion order, not send order. Sequencing
each chunk from the previous one's `finished` handler fixes the ordering and
incidentally provides the cancel point, so a second hotkey press can abort a
long paste. Note that abort stops the remaining chunks; whatever already
reached the HID stays typed.

**Two transports, no ordering guarantee between them.** The modifier key-ups
go out on the state WebSocket; the paste is an HTTPS POST. Nothing orders a
message on one socket against a request on the other, and losing that race
reintroduces the chord problem. The WebSocket is long-established and
realistically always wins, but the paste is deferred by 50 ms anyway —
imperceptible against a human keypress, and cheaper than the failure.

**Not done: a confirmation dialog.** A large accidental paste is
irreversible, which argues for confirming first. A modal dialog is the wrong
instrument here: raised while the video widget holds `grabMouse`/
`grabKeyboard`, it cannot be clicked or dismissed, which is a worse failure
than the one it prevents. Abort-on-second-press plus a character count in the
status bar covers the same ground without raising a dialog into an active
grab at all. (For the record, what froze input twice during the
confinement work was `QCursor::setPos` *during* a grab feeding synthetic
motion back through `grabMouse` — not grab churn as such; Qt transfers the
grab between windows routinely here.)

### 10.10 Region capture to text (verified 2026-10-11)

**The selector replaces the video rather than covering it.** Overlaying a
child widget on a `QVideoWidget` means compositing against whatever surface
the video backend is using, which is not a property worth depending on.
Instead both are pages of a `QStackedWidget` and the selector is shown in the
video's place, painting a still with the same `computeLetterbox` maths at the
same geometry — so the swap is visually seamless and a selection lands where
it was drawn. Selecting from a still is better behaviour anyway: the target
cannot move under the user mid-drag, and the pixels they get are the ones
they were looking at when they pressed the key.

Verified first that the frame is actually there to freeze: `QVideoSink`
retains the last frame, `videoFrame().toImage()` returns it at full
resolution, and it is the *latest* frame, not the first.

**The grabs stand down, the confinement stays up.** The drag and Esc have to
reach the selector, which cannot happen while the video widget holds
`grabMouse`/`grabKeyboard`. `suspendCaptureGrabs()` releases just the two Qt
grabs and the cursor marker; `m_captured`, the session-capture state, the
badge and the XFixes pointer barriers are all left alone, so the pointer
still cannot stray off the session windows mid-drag and the capture survives
the round trip. (This is a grab release and re-acquire, which is routine —
what froze input during the confinement work was `QCursor::setPos` *during* a
grab, not grab transfer. See §10.3.)

**Edge pixels.** Mapping the drag with rounded inclusive edges loses the
fraction of an image pixel the last widget column covers: selecting an entire
1920×1080 frame yielded 1919×1079. Widget pixel N covers image pixels
`[N·s, (N+1)·s)`, so the rect is built from a floored left edge and a ceiled
exclusive right edge, taking every image pixel the selection touches.

| case | drag | result |
|---|---|---|
| exact half scale | 201×151 widget px | 402×302 image px |
| whole frame, letterboxed widget | full widget | 1920×1080 |
| drag running off the frame | past the edge | clamped inside the image |
| click, or a nudge under 8 px | — | cancelled, nothing sent |

**Four providers, behind one interface (built 2026-10-12).** `ocr.provider`
selects between `pikvm`, `command`, `openai` and `tui`; `TextExtractor`
is the interface, and it keeps the surface `TuiClient` already had —
`extract`, `busy`, `cancel`, `finished(ok, text, error)` — because that
is what the spinner needs regardless of what does the work. Defaulting
preserves every config written before providers existed: unset resolves
to `tui` when an `api_key` is present and `pikvm` otherwise.

**`ExtractRequest` carries the selection twice, and has to.** The
model-backed providers and `command` read the cropped PNG. The PiKVM
does not take an upload at all: `GET /api/streamer/snapshot?ocr=1` plus
`ocr_left/top/right/bottom` OCRs the frame *it* is serving, so it needs
a rectangle in capture pixels. The consequence is worth stating — the
`pikvm` provider reads the screen as it is when the request lands, while
the others read the still the user froze. On a screen changing under the
selection they can differ.

**PiKVM OCR is on in stock firmware, and the cost is area (verified
2026-10-12 against kvmd on a PiKVM 4 Plus and a PiKVM 3).**
`/api/streamer/ocr` reports `enabled: true` with `eng` and `osd` on both.
Timings on the PiKVM 3, measured:

| selection | wall time |
|---|---|
| 300×100 | 2.0 s |
| 890×320 | 11.0 s |
| 1920×1080 (whole frame) | **504 at 60 s** — nginx in front of kvmd gives up |

So this provider is viable precisely because the feature is region-based,
and `ocr.timeout_ms` defaults to 60 s to match the server's own ceiling.
A bad `ocr_langs` is a clean 400 with a kvmd `ValidatorError`, which is
worth surfacing verbatim rather than translating.

Quality is Tesseract's: solid on prose, unreliable on hashes and serial
numbers — in the verification run it read a git SHA with two characters
wrong and `+1300` as `+1360`. That is the trade for an image that never
leaves the LAN, and the reason the model-backed providers stay.

**Tui and OpenAI specifics.** PNG rather than JPEG, so text edges stay crisp.
A 1920×1080 frame is 2.07 MP, just under the ~2.1 MP above which Tui downscales
server-side, so even a full-frame selection survives at native resolution.
The two differ only in endpoint and auth header — `X-API-Key` for Tui,
`Authorization: Bearer` for OpenAI and the clones — which is why one class
serves both, and why pointing `endpoint` at a local Ollama or vLLM works
without new code.
`reasoning_effort: none` — reasoning is pure latency for transcription, and
it competes for the token budget: when it exhausts `max_tokens` the server
answers **200 with empty content and no explanation**, so an empty reply is
reported as a failure rather than quietly copied onto the clipboard. The
documented statuses (401 key, 429 quota, 422 validation, 400 context) are
named in the status bar, because "Error transferring…" says nothing about
which knob to reach for.

**Showing that it is still working.** Extraction routinely takes several
seconds and a static status line cannot be told apart from a hang, so the
requesting window shows an animated spinner *and* an elapsed-seconds
count — "still going" and "going badly" look identical without the
number. The spinner is a permanent status-bar widget, not a transient
`showMessage`, which an unrelated status update would wipe out halfway
through. A busy cursor goes up at the same time, because the pointer is
where the user is actually looking; while captured it stacks on top of
the capture marker, which returns when the work ends.

That cursor is the thing to be careful with: `setOverrideCursor` is
application-wide and stack-based, so a `setBusy` without its matching
`clearBusy` strands every window on a busy cursor. It is pushed exactly
once, popped exactly once on every completion path (success, failure,
cancel, timeout), `clearBusy` is a no-op when idle so it cannot pop
someone else's cursor, and `closeEvent` clears it in case the window goes
away mid-request. Verified by asserting the stack returns to the capture
marker afterwards, including on a redundant clear.

A second hotkey press while the spinner is up cancels rather than queueing
another request — after several seconds of waiting, pressing it again is
impatience, not a new request. `TextExtractor::cancel()` abandons the
request — aborting the reply, or killing the subprocess — and still
reports `finished`, so the spinner is torn down on exactly one path.

### 10.11 Local MCP server (verified 2026-10-12)

`mcp.enabled: true` starts an MCP server inside the viewer, exposing the
session to any MCP client. This is the first increment of Phase 10 and the
only part of the harness that belongs in this repository — see HARNESS.md
§9 for the public/private split, and §8.1 for why a generic client comes
before the Kahu harness rather than after it.

**Why it is in-process.** The tools need the video sinks for frames and the
session view — which head sits where on the combined desktop, which host
carries HID — and neither exists outside the viewer. A separate server
would have to re-authenticate every PiKVM and would still not know the
monitors are adjacent, because no single PiKVM can know the others exist
(§4). That is also the answer to "why not reuse the PiKVM MCP server":
it speaks per-PiKVM pixels, so a client using it cannot reason about the
desktop as a whole.

**Transport: HTTP, POST only, no SSE.** `McpHttpServer` is a minimal
HTTP/1.1 front end over `QTcpServer` — Qt has no HTTP server and pulling
one in for one POST endpoint would be disproportionate. `GET` returns
**405**, deliberately: HARNESS.md §9.1 settles on the client polling a
tool rather than the server pushing, so there is no stream to open and
leaving a GET hanging would make a client wait for push that never comes.
No `Mcp-Session-Id` is issued; the spec permits a stateless server and
every tool call here is self-contained. Chunked request bodies are
rejected with 411 rather than mis-parsed.

Responses are asynchronous. A tool that waits — a paste sequencing its
chunks, a settle delay before a screenshot — holds its socket and answers
when it completes, guarded by a `QPointer` so a client that disconnects
mid-call drops the response instead of writing to a dead descriptor.

**Verified 2026-10-12** against `curl` and against Claude Code itself
(`claude mcp add --transport http`, which reported `✔ Connected`), plus 11
assertions in `tests/test_mcpserver.cpp` covering protocol echo and
fallback, the tool schemas, notification-gets-202, parse error, unknown
method, unknown tool, the no-HID refusals, GET→405, and bearer-token
enforcement. `glasshouse-mcpprobe` serves a synthetic two-head session so
the surface can be exercised with no PiKVMs attached.

**Mouse coordinates need no calibration, and that is the point.** The
PiKVM MCP server calibrates by visual cursor detection: it moves a known
delta, diffs two screenshots, finds the two cursor-sized clusters and
medians the commanded/detected ratios. It has to, because it cannot know
the geometry. Glasshouse is configured with the monitor layout and
verifies it against the streamer (§10.8), so `desktopPixelToApi` maps a
desktop pixel straight to s16 — steps 4–6 of §5.2 with no cursor, window
or letterbox in the picture. A measured correction factor would only add
error.

What *was* worth taking from that calibration loop is the settle delay.
It waits 300 ms between a move and the screenshot it measures, and for
good reason: the target's compositor has to draw the cursor and the
capture pipeline has to carry the frame. `glasshouse_mouse_move` therefore
takes `settle_ms`, because a caller that moves and immediately captures
otherwise sees the pointer in its old position.

Both transform paths are asserted to agree exactly:
`desktopPixelMatchesCursorPathAtSamePoint` drives five points through the
cursor path and the pixel path and compares the wire values, so a
tool-driven click cannot land somewhere other than where the same
coordinate lands by hand.

**Tools.** `glasshouse_session` (screens, desktop, HID routing — call it
first), `glasshouse_screenshot` (live frame as PNG, `max_width` default
1280 since base64 in a JSON body is not free, 0 for native),
`glasshouse_type` (through `/api/hid/print` with the §10.9 sanitiser, so
unmappable characters are reported rather than silently lost),
`glasshouse_keys` (chord: pressed in order, released in reverse),
`glasshouse_mouse_move`, `glasshouse_mouse_click`,
`glasshouse_mouse_scroll`.

Every input tool checks three things before sending — a HID master exists,
it is connected, and kvmd reports the target's HID online — and names
which one failed. A keystroke that goes nowhere is worse than a refusal,
because the model cannot tell the difference without being told.

**Posture.** Off by default, `127.0.0.1` when on. This is a remote-control
surface for the target: anything that reaches the port can type on and
click it. `mcp.token` sets a required bearer token, resolved as a
`secret://` ref like any password — and a token that is configured but
cannot be resolved stops the server rather than silently downgrading to an
open port. Tool activity flashes on every window's status bar, because
something else driving the keyboard should be visible rather than silent.

## 11. Risks and mitigations

| Risk | Status | Mitigation |
|---|---|---|
| PiKVM coord range differs from assumed s16 | **resolved** | Verified empirically in §10.1 |
| WebRTC negotiation latency | open | Measure auth-to-first-frame; if ICE gathering dominates, set `stun-server=NULL` on webrtcbin for LAN-only deployments |
| KDE Wayland grab quirks | open | App-local hotkey avoids global-shortcut issues; test on real hardware in Phase 3 |
| HID master PiKVM fails | open | Manual failover (re-cable, update config). Auto-failover out of scope v1 |
| Dock disconnect re-orders target monitors | open (detected indirectly) | A reordering is invisible to the client (§10.8); the resolution-mismatch warning is the nearest signal. Manual reassignment in config |
| Target display settings change resolution | **resolved (detected)** | §10.8 — status-bar resolution readout plus an INFO/WARNING split on whether the mapping is actually affected |
| Clipboard text the target keymap can't type | **resolved (mitigated)** | §10.9 — common Unicode punctuation folded to ASCII, CRLF normalised, remainder dropped with a count shown; kvmd discards it silently otherwise |
| Runaway clipboard paste into a live target | open (bounded) | §10.9 — chunks are sequenced so a second hotkey press aborts the remainder; what already reached the HID stays typed |
| Tui API key leaking into config or logs | **resolved** | `ocr.api_key` is a `secret://` ref resolved via SecretResolver (env or `secrets.yaml`, 0600); the key is never logged |
| Region capture sends target screen content to an external service | open (by design) | The user chooses the rectangle each time and nothing is sent without a deliberate drag; endpoint is configurable |
| Local MCP server lets any local process drive the target | open (bounded) | §10.11 — off by default, loopback-only when on, optional bearer token that fails closed when unresolvable, and every tool call flashes on the status bar so it is not silent |
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

### 13.6 Capability surface of the deployed units (verified 2026-10-11)

Enumerated read-only over `GET` against both PiKVMs, to ground what is
worth mapping into the client. **The two units are not the same**, and
anything per-head must read `features` / `limits` per host rather than
assume:

| | 192.168.1.71 | 192.168.1.144 |
|---|---|---|
| hardware | v4 (`__v4_locator__` GPIO) | v3 |
| kvmd | 3.199 | 3.106 |
| ustreamer | 5.37 | 5.9 |
| `desired_fps` ceiling | 70 | 60 |
| MSD features | cdrom, multi, **rw** | cdrom, multi |

Common to both: `extras` has janus and webterm enabled, vnc and ipmi
disabled. `/api/switch` returns **404** — no multi-port switch support
on this firmware. `/api/redfish/v1` is present but redundant with ATX
here. GPIO exposes only the built-in outputs (`__v3_usb_breaker__` on
both, `__v4_locator__` on the v4) with no user-defined pins and an empty
view table.

Streamer `features` reports `h264: true`, `quality: true`,
**`resolution: false`** — the capture resolution is driven by the source
EDID and *cannot* be set through the API. That closes the obvious door
on §10.8: detection really is the only option for layout drift.

`params` are tunable via `set_params` and currently unmapped by the
client: `quality`, `desired_fps`, `h264_bitrate`, `h264_gop`. Both boxes
were asking for more than they achieve (40 and 30 desired, 27 captured),
so a per-window quality/fps control is the most direct lever available
on the viewer CPU cost noted in §10.6.

Also unmapped: the GPIO USB breaker, which is a remote "unplug the USB
from the target" — a HID-recovery tool that otherwise needs hands on
hardware.
