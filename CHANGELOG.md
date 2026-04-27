# Changelog

All notable changes to Glasshouse Viewer are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-04-27

First public release. A native Linux Qt 6 / C++ KVM client that
presents N PiKVMs as N independently-movable windows on a KDE
desktop, replacing the "multiple Chrome tabs pointed at multiple
PiKVM web UIs" pattern for managing remote target machines.

### Added

#### Multi-PiKVM viewer
- One window per PiKVM, each backed by its own auth, signalling, and
  decode pipeline. Windows can sit anywhere on the local desktop, any
  size, fullscreen or tiled.
- Single configured HID master routes all keyboard and mouse input
  through one PiKVM's USB OTG link to the target. The other PiKVMs
  are video-only.
- **Click-to-capture, session-wide.** Clicking any window starts
  capture; the cursor moves freely between sibling windows with the
  active coordinate transform following it. Default release hotkey
  `Ctrl+Alt+Shift+Escape`, configurable.

#### Video transports
- **WebRTC over Janus** (`/janus/ws`, plugin `janus.plugin.ustreamer`)
  for stock PiKVM 4 Plus. Hardware H.264 decode preferred
  (`nvh264dec` / `vah264dec`), `avdec_h264` fallback.
- **MJPEG** (`/streamer/stream`) for older PiKVM 3 boards or when the
  Janus path is unavailable.
- Per-PiKVM transport selection in YAML.

#### Reliability
- Exponential-backoff reconnect for both auth and Janus signalling,
  with a shared retry budget so a single failure can't fan out into
  a storm.
- `JanusClient` retries the `watch` handshake on the older firmware's
  pre-IDR `503 "Haven't received SPS/PPS from memsink yet"`,
  mirroring what kvmd's web UI does. Bounded retry budget before
  escalating to the outer reconnect ladder.
- Cold-start works without a browser priming the encoder: the kvmd
  state WS is opened with `?stream=1`, which is what tells kvmd's
  stream controller to spawn `ustreamer`.

#### Input
- PiKVM signed-s16 absolute-mouse API space, with the off-by-one
  artefact at 25 % / 75 % corrected
  (`round(f × 65535) − 32768`, verified against `evtest` on real
  hardware).
- ~120 Hz mouse-move coalescer to keep the kvmd HID gadget from
  queueing frames behind a Wayland 1 kHz event stream.
- MDN `KeyboardEvent.code` mapping (`KeyA`, `ShiftLeft`, …)
  end-to-end.

#### UI
- **Special Keys palette** (`Ctrl+Alt+K`) with three tabs: on-screen
  keyboard with sticky modifier toggles for keys the local
  compositor would otherwise swallow (PrintScreen, Super-chords),
  curated and YAML-defined custom chord buttons, and a paste-as-
  keystrokes panel via `/api/hid/print` for picky BIOS prompts.
- **Mass Storage dialog** — upload, mount, eject, set-as-CD-ROM,
  delete; multi-GB uploads are streamed with `QProgressDialog`.
- **Target ATX actions** — power short-press, power long-press
  (force-off), reset; each gated by a confirmation dialog.
- **Per-PiKVM window persistence** via `QSettings` — geometry,
  fullscreen, and maximized state survive across sessions.
- **Menubar** with File / View / Target / Help; auto-hides
  alongside the status bar in fullscreen.

#### Packaging and ops
- `.deb` for Ubuntu 24.04+ produced by GitHub Actions on tag push,
  uploaded to the GitHub Release. Linked against system Qt 6.4 — no
  bundled Qt, no `LD_LIBRARY_PATH` shim. Runtime deps are stock
  `libqt6*t64` / GStreamer / libnice from the noble archive.
- MDI greenhouse glyph as the app icon.
- `--log-file` flag installs a custom `QtMessageHandler` that
  delegates to Qt's stderr handler (so `QT_LOGGING_RULES` keeps
  working) and additionally appends ISO-timestamped lines to a log
  file.
- Secrets resolution from environment first
  (`GLASSHOUSE_SECRET_<NAME>`), then
  `~/.config/glasshouse/secrets.yaml` (chmod 0600).
- `secrets.yaml` fallback so KDE-menu / `.desktop` launches don't
  fail when the user's shell rc files aren't sourced.

### Verified configurations

- PiKVM 4 Plus, kvmd 3.199 (WebRTC).
- PiKVM 3, kvmd 3.106 (WebRTC with cold-start retry).
- Two-monitor 3840×1080 target desktop, three-monitor configurations.

### Out of scope for this release

- Clipboard sync between local and target.
- Audio (microphone-to-target, speaker-from-target).
- Macro keypad integration. The YAML `shortcuts` schema is the
  shared source of truth between the Special Keys palette and a
  future keypad daemon, but the daemon itself is separate work.

[Unreleased]: https://github.com/kultivator-consulting/glasshouse/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/kultivator-consulting/glasshouse/releases/tag/v0.1.0
