# Changelog

All notable changes to Glasshouse Viewer are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Four OCR providers behind `ocr.provider`**, so text extraction no
  longer requires a Kultivator API key:
  - `pikvm` — the PiKVM's own OCR, over a rectangle of the frame it is
    already serving. Needs nothing installed on the host or the PiKVM
    (stock firmware reports `enabled: true` with `eng` and `osd` at
    `/api/streamer/ocr`), and the image never leaves the LAN. **The new
    default**, so a fresh clone extracts text with no configuration at
    all. Keep selections small: measured on a PiKVM 3, a 300×100 region
    takes about 2 s, 890×320 about 11 s, and a whole 1920×1080 frame
    exceeds the 60 s timeout kvmd sits behind.
  - `command` — any host program, PNG on stdin, text from stdout.
    `["tesseract", "-", "-"]` is about 8 MB on noble. Nothing is linked
    against it, so other installs carry no dependency.
  - `openai` — any OpenAI-compatible `/v1/chat/completions` with
    `Authorization: Bearer`, which also covers a local Ollama, vLLM or
    LM Studio via `endpoint`.
  - `tui` — the same wire shape at Kultivator's endpoint with
    `X-API-Key`.

  `none` turns extraction off while leaving the selector usable.
  Existing configurations are unaffected: an `ocr.api_key` with no
  `provider` still resolves to `tui`, and there is a test pinning that.

### Changed
- **`TuiClient` is now `OpenAiExtractor`, behind a `TextExtractor`
  interface** shared by all four providers. The surface is the one
  `TuiClient` already had, because that is what the spinner needs
  whatever is doing the work.
- **`VideoWindow::regionCaptured` carries the source rectangle** as
  well as the cropped image. The PiKVM provider does not take an
  upload — it OCRs its own live frame and wants a bounding box — so the
  two forms of the same selection both have to reach the extractor. One
  consequence: that provider reads the screen as it is when the request
  lands, while the others read the still the user froze.

- **Watchers** (`Ctrl+Alt+Shift+W`, or `glasshouse_watcher_create` over
  MCP). Drag a rectangle, say what matters about it, and Glasshouse
  checks it on every decoded frame and records an event whenever it
  changes — which is the rate a notification toast needs, since one
  lives a few seconds and anything on a timer misses most of them.
  Detection is local, needs no OCR provider and never leaves the
  machine: a strided luma difference against the previous frame over
  the rectangle only, so a large region costs about what a small one
  does.
  - `change` fires the instant a region changes; `settle` fires once a
    region that was changing has gone quiet. A region that never goes
    quiet reports at its cooldown's rate rather than once and then
    never again.
  - The target's cursor is captured over HDMI, so a pointer crossing
    the region looks exactly like the event — detection is suppressed
    while Glasshouse's own pointer is inside, and `watcher_list` says
    when that is why nothing is firing.
  - `keep_history` keeps a rolling buffer of crops so you can look back
    *through* a change rather than only at the frame that tripped it.
    Retained on change rather than on tick, held in memory, never
    written to disk, dropped with the watcher, and refused outright
    above ~186,000 px rather than silently degraded.
  - `glasshouse_watcher_frames` reads it back as a filmstrip — many
    moments tiled into one image with wall-clock times drawn under
    them, which gets a timeseries past a four-images-per-request limit
    — or as full-resolution stills.
  - **Nothing is pushed.** Events queue until something collects them
    with `glasshouse_watcher_poll`, which takes a cursor and returns
    the next one. Polling needs no protocol feature, so any MCP client
    works and the viewer keeps no subscriber list.
  - Budgets and the hotkey are configurable under `watchers:` in
    config.yaml. See DESIGN.md §10.12 and HARNESS.md §5.
  - Every event carries a **`transience`**: `reverted` means the region
    went back to its pre-event content, so something appeared and then
    left — the shape of a notification — while `persisted` means it is
    still on screen. The comparison is against the frame from before
    the firing rather than the previous one, over a 30 s window
    (`watchers.revert_window_ms`). It ranks retention and never
    suppresses a firing: a dialog waiting to be answered does not
    revert either, and those are events a watcher exists to catch.
    When the ring is full, persistent frames are evicted before
    transient ones, because a transient's frames are the only record
    there will ever be.
  - **`glasshouse_watcher_verdict`** lets a consumer write `relevant`
    or `spurious` back onto an event — HARNESS.md §5's judging step,
    which had never been built. `spurious` drops that event's retained
    frames at once and hands the budget back; the event survives,
    carrying the verdict and an optional note. A bundled
    `watcher-triage` skill drives it, and needs no poll cursor because
    the verdict is itself the bookmark.
  - Detection samples the mapped frame in place, including the packed
    BGRA both pipelines actually emit. Verified live on a two-head
    session: a single 42×22 watcher adds no measurable CPU, where an
    earlier build that recognised only planar YUV fell back to two
    whole-frame conversions per frame and cost about +46% of a core.
    The viewer logs which path a screen takes, once, on its first frame.

### Documentation
- **The providers are now measured against each other, not just
  described.** On identical crops of one snapshot, Tui answered 3–4×
  faster than the PiKVM's own OCR at every selection size and read
  correctly the characters Tesseract lost (`_`, `@`, `l` versus `1`, a
  git SHA). The README's provider section and DESIGN.md §10.10 carry
  the figures, so choosing between "nothing leaves the LAN" and "gets
  the characters right" is an informed choice rather than a guess.
- **A whole-screen watcher is documented as a trap, not a shortcut.**
  The change metric is a mean over the rectangle, so a toast covering
  2% of a 1080p frame moves it by about 4 against a default threshold
  of 12 — it never fires. Measured, and now said plainly in both the
  README and DESIGN.md §10.12.
- **Each provider's upper limit is now written down**, because past a
  certain selection size they fail rather than slow down: `pikvm`
  returns 504 once kvmd's nginx times out at 60 s, and `tui` returns
  500 immediately once the base64'd PNG pushes the request past its
  1 MiB body limit — about 786 KB of PNG, so a whole 1080p frame never
  fits. `openai` against OpenAI itself documents 20 MB per image and
  has no equivalent wall.

## [0.1.4] - 2026-10-12

### Added
- **A local MCP server, off by default** (`mcp.enabled`). Exposes the
  running session to any MCP client: `glasshouse_session` (screen indexes,
  each head's rectangle within the combined desktop, which PiKVM carries
  HID), `glasshouse_screenshot`, `glasshouse_type`, `glasshouse_keys`,
  `glasshouse_mouse_move`, `glasshouse_mouse_click` and
  `glasshouse_mouse_scroll`. It runs inside the viewer because the tools
  need the video sinks and the session view, neither of which exists
  elsewhere — see DESIGN.md §10.11.

  Mouse coordinates need no calibration step: the monitor layout is
  configured and verified against the streamer, so a desktop pixel maps
  straight to absolute HID coordinates. `glasshouse_mouse_move` takes
  `settle_ms` for callers that capture straight after moving, since the
  target still has to draw the cursor and the frame still has to travel.

  Loopback-only when enabled, with an optional `mcp.token` bearer token
  resolved as a `secret://` ref — a token that cannot be resolved stops
  the server rather than silently leaving the port open. Every tool call
  flashes on the status bar, because something else driving the keyboard
  should not be silent.
- **`glasshouse-mcpprobe`.** Serves a synthetic two-head session with no
  PiKVMs attached, for exercising the MCP tool surface without hardware.
- **`--version`.** Prints the compiled version string and exits. Long
  form only: `-v` is already `--verbose`. Added because "is the installed
  package the code I am looking at" had no quick answer.

### Changed
- **`VideoWindow::setCaptureContext` takes a `CaptureHotkeys` struct**
  instead of five adjacent `QString` parameters. Two transposed hotkeys
  would have bound silently and misbehaved later.
- **Quieter logging.** Categories now default to Info level, so the
  high-frequency debug output — per-frame WebSocket state dumps, the
  per-second video telemetry, and per-event HID drops during a reconnect —
  no longer floods syslog/journald. Re-enable for troubleshooting with
  `QT_LOGGING_RULES='glasshouse.*.debug=true'` (or per-category, e.g.
  `glasshouse.ws.debug=true`). The viewer also accepts `--verbose`/`-v` to
  enable debug logging without setting the env var.

### Fixed
- **The viewer aborted when an MCP tool answered after its request
  returned.** `handleRequest` captured its JSON-RPC responder by
  reference, so it lived on a stack frame that was gone by the time an
  asynchronous tool called back: `glasshouse_mouse_move` with
  `settle_ms`, and `glasshouse_type` waiting on `pasteFinished`.
  Invoking the destroyed `std::function` threw `bad_function_call`,
  which nothing caught, so the process terminated. Every synchronous
  tool was unaffected, which is why this only showed up on the first
  live pointer move. Responders are now captured by value.
- **Every link to the repository was a 404.** The URLs said
  `kultivator-consulting`; the organisation is `KultivatorConsulting`.
  Nine occurrences, including the About box link, the `homepage` field
  shipped in the package metadata, the README Releases link and the
  CONTRIBUTING issues link.
- **The About box reported a different version from the package.** The
  version string was compiled from `project(... VERSION)` in
  `CMakeLists.txt`, which had never been bumped past `0.1.0`, while the
  `.deb` took its version from the git tag. CI now determines the version
  before configuring and passes it in as `GLASSHOUSE_VERSION`, so a
  packaged binary reports exactly what the package says; `project()`
  remains the fallback for a local build with no override.
- **Pastes over 1024 characters could arrive scrambled.** `pasteText`
  split the text into chunks at the server's per-request limit but posted
  them all from a tight loop; Qt opens up to six connections per host, so
  the chunks raced their TLS handshakes against each other and reached
  kvmd in whatever order they finished. Chunks are now posted strictly one
  at a time, each from the previous one's completion, which also makes the
  paste abortable and reportable. Only reachable from the Special Keys
  Paste tab before now, where the text is usually short.
- **Mouse wheel only scrolled on the window that started the capture.**
  Qt routes `QWheelEvent` without consulting `QWidget::mouseGrabber()` —
  unlike mouse moves and buttons, which do honour the grab — so a scroll
  over a sibling window was delivered either to the holder's `QMainWindow`
  root (where no event filter was installed) or to the sibling's own video
  widget (where `handleWheel` early-returned because that window didn't own
  the grab). Either way the notch was dropped. The filter now also watches
  the window itself, and `handleWheel` is gated on a session-wide capture
  rather than on this window holding the grab; the notch carries no
  position, so the target scrolls wherever its cursor already is. The Qt
  grab is deliberately *not* moved between windows — see DESIGN.md §10.7.
- **Cursor confinement during capture (X11).** While capture is held, the
  pointer is now penned inside the bounding rectangle of the session windows
  using XFixes pointer barriers, so it can't drift onto the local desktop or
  another monitor and misroute keystrokes into the wrong window. Barriers are
  server-side (no pointer grab, no cursor warping), so — unlike the briefly
  attempted `QCursor::setPos` approach — they can't feed events back and
  freeze input; verified to confine and release cleanly on a live session.
  X11 only; Wayland would need the compositor pointer-constraints protocol
  (not yet implemented), where confinement is a no-op.
- **Video latency: >1 s of glass-to-glass lag from unbounded client-side
  buffering.** Three reservoirs, all client-side, none a codec change:
  - Janus/H.264: the pre-decode `queue` inherited GStreamer's 1 s,
    non-leaky default and filled on any decode stall. Now bounded to a
    few buffers (non-leaky — dropping encoded H.264 mid-GOP would corrupt
    decode until the next IDR; loss is left to webrtcbin's jitterbuffer).
  - MJPEG: added a `queue leaky=downstream` after `multipartdemux` as a
    deterministic stale-frame dropper (MJPEG is all-intra, so dropping is
    safe). Previously nothing shed frames ahead of the software decoder,
    so a slow client backed frames up in `souphttpsrc` + the socket.
  - Both transports: decoded frames were posted to the GUI thread on an
    unbounded queued connection; a latest-frame mailbox now coalesces to
    the newest frame so a slow render can't accumulate lag.
  See DESIGN.md §10.2.

### Added
- **Region capture to text (`Ctrl+Alt+Shift+C`, configurable via
  `region_hotkey`).** Freezes the video frame of the window *under the
  pointer* — not the focused one, which while captured is rarely the one
  you're looking at — dims it, and lets you drag out a rectangle. The crop
  goes to Tui and the extracted text lands on the host clipboard. Esc,
  right-click, or a drag under 8px cancels. The selector replaces the
  video widget rather than overlaying it, so there's no compositing
  question and the target can't shift under you mid-drag; the capture
  survives the round trip, with only the Qt grabs standing down (pointer
  confinement stays up) so the drag can reach the selector. Configure
  under `ocr:`; `api_key` takes a `secret://` reference so the key stays
  out of `config.yaml`. Without a key the selector still works and says
  what's missing. New `glasshouse.ocr` logging category. See DESIGN.md
  §10.10.
- **Progress indication for text extraction.** The round trip to the model
  can take many seconds, where a static status line is indistinguishable
  from a hang. The requesting window now shows an animated spinner with an
  elapsed-seconds count, as a permanent status-bar widget so an unrelated
  status update can't wipe it out, plus a busy cursor under the pointer.
  Pressing the region hotkey again abandons the request instead of
  queueing a second one.
- **Paste the host clipboard onto the target (`Ctrl+Alt+Shift+V`,
  configurable via `paste_hotkey`).** Types the clipboard through the HID
  master's `POST /api/hid/print`, so the target's own keymap decides what
  gets typed — the same path the Special Keys palette's Paste tab already
  used, now without opening a dialog (and so without dropping the capture
  to do it). Text only: an image or empty clipboard is reported in the
  status bar and dropped. Clipboard text is folded to what the server-side
  `en_US` keymap can actually reach first — curly quotes, en/em dashes,
  ellipses and non-break spaces become their ASCII equivalents, CRLF
  collapses to LF (a surviving CR types a second Enter), and anything
  still unreachable is dropped with a count shown to the user, because
  kvmd discards it silently otherwise. Pressing the hotkey again while a
  long paste is still running aborts the remainder. See DESIGN.md §10.9.
- **Target layout drift warning.** The coord transform is purely
  proportional — only each monitor's share of the logical desktop reaches
  the wire — so a resolution change on the target is harmless when every
  monitor scales by the same factor, and breaks the mouse mapping when they
  don't. The status bar now shows each PiKVM's live capture resolution, and
  the viewer compares the real per-monitor scale factors against
  `target.monitors` on every resolution change: a uniform change logs at
  INFO ("mapping unaffected"), a non-uniform one logs a WARNING naming each
  host's actual vs configured size and flags the status bar with `⚠`. A
  left/right reordering on the target is invisible to the client and the
  warning says so. See DESIGN.md §10.8.
- **Capture badge follows the cursor.** With a session capture active, the
  "capture ON" status now shows on whichever window the cursor is over
  rather than on the one that started it; other windows read "capture ON —
  cursor on another window". State only — the Qt grab does not move.
- **Local cursor marker during capture.** Instead of hiding the pointer,
  capture draws a zero-lag local marker (a haloed ring + centre dot by
  default) at the true pointer position. Because PiKVM is absolute-positioned
  and the video is a 1:1 letterboxed view, the guest cursor converges to it,
  so pointing feels instant despite video latency; clicks ripple the marker.
  Configurable via `video.cursor_marker` (`ring` | `crosshair` | `hidden`),
  `cursor_marker_color` (`#RRGGBB`), and `cursor_marker_size` (px).
- **Configurable WebRTC jitterbuffer latency.** `video.webrtc_latency_ms`
  (default 100 ms, down from webrtcbin's 200) tunes the Janus/H.264
  jitterbuffer dwell — lower trims glass-to-glass latency on a LAN at the
  cost of jitter tolerance. No effect on MJPEG.
- **Steady-state video telemetry.** The status bar shows effective render
  FPS and the GUI-thread coalesced-drop count (`video: N fps · M
  coalesced`), polled at 1 Hz; the same line logs under
  `glasshouse.video.debug`. See DESIGN.md §10.2.
- **MJPEG path tuned for low latency.** Pre-decode buffering keeps only the
  single freshest JPEG (`queue leaky=downstream max-size-buffers=1`) and one
  decoded frame at the sink (`appsink max-buffers=1`), so the picture tracks
  input as tightly as software decode allows. The earlier `jpegparse !
  videorate max-rate=N` fps cap was removed — on ustreamer's untimestamped
  stream it never capped (GStreamer #720104), only added latency, and froze
  the video outright with `drop-only=true`. `video.target_fps` is no longer
  enforced on MJPEG.
  Hardware JPEG decode was evaluated and ruled out: ustreamer emits 4:2:2
  baseline JPEG, which NVIDIA's `nvjpegdec` rejects with not-negotiated
  (and `vajpegdec` is absent on NVIDIA), so software `jpegdec` remains the
  only viable decoder for the real stream. See DESIGN.md §10.6.

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

[Unreleased]: https://github.com/KultivatorConsulting/glasshouse/compare/v0.1.4...HEAD
[0.1.4]: https://github.com/KultivatorConsulting/glasshouse/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/KultivatorConsulting/glasshouse/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/KultivatorConsulting/glasshouse/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/KultivatorConsulting/glasshouse/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/KultivatorConsulting/glasshouse/releases/tag/v0.1.0
