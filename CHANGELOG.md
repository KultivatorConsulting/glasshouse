# Changelog

All notable changes to Glasshouse Viewer are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.3] - 2026-05-14

### Changed
- Default `release_hotkey` is now `Ctrl+Alt+Shift+Backspace` (was
  `Ctrl+Alt+Shift+Escape`). All four keys now sit under the natural
  two-handed Ctrl-Alt-Del shape — left hand on the modifier cluster,
  right hand on Backspace — instead of forcing both hands into the
  bottom-left corner. Existing configs with `release_hotkey:` set
  explicitly are unchanged; only the fallback when the field is
  empty or missing has moved. Update or remove the field in
  `~/.config/glasshouse/config.yaml` to pick up the new default.

## [0.1.2] - 2026-05-13

### Fixed
- High-resolution mouse wheels and Wayland touchpad scrolls were
  silently dropped: Qt delivers `angleDelta` in sub-notch increments
  (e.g. ±16 1/8-degrees per event) instead of one full ±120 notch,
  and the previous integer-divide-by-120 rounded each event to zero
  before reaching the wire. `handleWheel` now accumulates `angleDelta`
  across events and emits `mouseWheel` only when the running total
  crosses a full ±120 notch in either axis, retaining the sub-notch
  residue for the next event.

## [0.1.1] - 2026-05-12

### Known issues
- **`webrtcbin` long-running memory leak** (~8 MB/s/stream). The
  GStreamer `webrtcbin` element used by `transport: janus` leaks
  steadily while media is flowing; observed RSS reached 67 GB in
  ~3 hours with two PiKVMs. Bisected to `webrtcbin` internals
  (decoder, `videoconvert`, our Qt path, glibc arenas, and pipeline
  teardown all ruled out). **Workaround**: set `transport: mjpeg`
  for affected PiKVMs. The MJPEG path is leak-free. See
  DESIGN.md §10.5 for the full investigation.

### Changed
- Loud `WARN`-level startup log when `transport: janus` is selected,
  pointing at DESIGN.md §10.5 and the MJPEG workaround. Easy to
  miss otherwise — the leak takes hours to manifest.

### Fixed
- Shifted-symbol keys typed on the physical keyboard (`?`, `(`, `)`,
  `@`, `#`, `_`, `+`, `{`, `}`, `|`, `:`, `"`, `<`, `>`, `~`, `!`,
  `$`, `%`, `^`, `&`, `*`) were silently dropped at the eventFilter:
  Qt resolves shifted forms into different `Qt::Key` enums from the
  unshifted physical key (Shift+2 → `Qt::Key_At` rather than
  `Qt::Key_2`), so the keymap fell through to the unmapped return.
  KeyMap now routes each shifted form back to its physical-key MDN
  code (US layout); the Shift modifier event continues to fire on
  its own, and the target's keymap composes the two into the right
  character.
- `VideoPipeline::Impl` destructor now calls `malloc_trim(0)` on
  glibc to return decommitted heap pages to the OS at pipeline
  teardown. Doesn't fix the `webrtcbin` leak (those allocations
  aren't actually freed) but trims any incidental fragmentation
  from session churn.

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

[Unreleased]: https://github.com/kultivator-consulting/glasshouse/compare/v0.1.3...HEAD
[0.1.3]: https://github.com/kultivator-consulting/glasshouse/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/kultivator-consulting/glasshouse/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/kultivator-consulting/glasshouse/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/kultivator-consulting/glasshouse/releases/tag/v0.1.0
