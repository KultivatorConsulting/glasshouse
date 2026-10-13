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

**v0.1.4 — current.** First public release was v0.1.0 in April 2026;
the patch releases since landed input fixes surfaced by daily use:
shifted-symbol keys (`?`, `(`, `)`, …) routed via the physical-key
MDN code, mouse-wheel sub-notch accumulation for high-resolution
wheels and Wayland touchpads, and an ergonomic release-hotkey
default (`Ctrl+Alt+Shift+Backspace`). v0.1.4 adds an opt-in
[MCP server](#mcp-server) so other tools can drive the session.
All Phase 0–8 work in [DESIGN.md §9](specs/DESIGN.md) is complete.
The viewer is in daily use against a live two-PiKVM (PiKVM 4 Plus +
PiKVM 3) setup.

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
- **[Clipboard paste and text extraction](#clipboard-paste-and-text-extraction)**
  — `Ctrl+Alt+Shift+V` types the host clipboard onto the target;
  `Ctrl+Alt+Shift+C` drags out a region, extracts its text and puts it
  back on the host clipboard. The two directions a KVM otherwise
  can't cross. Extraction has four backends; the default uses the
  PiKVM's own OCR, so it works with nothing installed and nothing
  sent off your network.
- **[Watchers](#watchers)** — `Ctrl+Alt+Shift+W` drags out a region to
  watch; anything that changes in it is recorded, with an optional
  rolling buffer of crops so you can see what a change looked like
  before it happened. Detection is local, per decoded frame, and needs
  no OCR provider.
- Mass Storage upload / mount / eject / present-as-CDROM.
- Target ATX power short-press, long-press (force-off), reset — each
  gated by a confirmation dialog.
- Per-PiKVM window geometry / fullscreen / maximized state persists
  across sessions.
- **An optional local [MCP server](#mcp-server)** exposing the session
  — screen layout, screenshots, typing, key chords and pointer control
  — to any MCP client. Off by default.
- `.deb` packaging built by GitHub Actions on tag push, linked against
  system Qt 6.4 from the noble apt archive (no bundled Qt).

Deliberately out of scope for the 0.1.x line: *continuous* clipboard
sync (the two one-shot paths above are supported; a background
bidirectional sync is not), audio, macro keypad daemon. See
[CHANGELOG.md](CHANGELOG.md) for the full list.

## Install (from a release `.deb`)

Tagged releases publish a `.deb` to the
[Releases page](https://github.com/KultivatorConsulting/glasshouse/releases).
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

# Report the compiled version and exit — the quick way to tell whether
# the installed package is the code you are looking at.
glasshouse-viewer --version

# Append all log messages to a file in addition to stderr. Honours
# QT_LOGGING_RULES — pair with one to dump verbose categories.
glasshouse-viewer --log-file /tmp/glasshouse.log
QT_LOGGING_RULES='glasshouse.video.debug=true' \
    glasshouse-viewer --log-file /tmp/glasshouse.log
```

Default hotkeys (configurable per-PiKVM in YAML):

| Hotkey | Config key | Action |
|---|---|---|
| `Ctrl+Alt+Shift+Backspace` | `release_hotkey` | Release session-wide capture |
| `F11` | `fullscreen_hotkey` | Toggle fullscreen on this window |
| `Ctrl+Alt+K` | `special_keys_hotkey` | Show / hide Special Keys palette |
| `Ctrl+Alt+Shift+V` | `paste_hotkey` | [Type the host clipboard onto the target](#clipboard-paste-and-text-extraction) |
| `Ctrl+Alt+Shift+C` | `region_hotkey` | [Extract text from a dragged region to the host clipboard](#clipboard-paste-and-text-extraction) |
| `Ctrl+Alt+Shift+W` | `watch_hotkey` | [Watch a dragged region and record what changes](#watchers) |

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

## Clipboard paste and text extraction

A KVM gives you pixels in and keystrokes out, so the clipboard does not
cross on its own. Two hotkeys bridge it, one in each direction. Both
work while capture is held, and both report back on the window the
pointer is over — which is not necessarily the one with keyboard
focus.

### Paste the host clipboard onto the target — `Ctrl+Alt+Shift+V`

Types the host clipboard at the target as keystrokes, through the HID
master's server-side keymap. No configuration needed.

- **Text only.** The clipboard has to hold text; images and files are
  ignored, and you get a status flash saying so.
- **Characters the target keymap cannot produce are reported, not
  silently dropped** — the flash reads `pasting 412 chars (3
  unsupported dropped)…`.
- **Press the hotkey again mid-paste to abort it.** A long paste shows
  progress on the status bar. The abort is a second hotkey press rather
  than a dialog because the video widget holds `grabMouse` /
  `grabKeyboard` while captured, so a modal dialog raised over it
  cannot be clicked or dismissed.

Pastes are chunked at the server's per-request limit and posted strictly
one at a time — see [CHANGELOG.md](CHANGELOG.md) for why posting them in
parallel scrambled them.

### Extract text from the target to the host clipboard — `Ctrl+Alt+Shift+C`

Drag out a rectangle on the window under the cursor. The region is sent
for text extraction and the result lands on your host clipboard — error
messages, serial numbers, stack traces, anything you would otherwise
retype by eye off a video stream.

- A spinner shows the region size while the request is in flight.
- **Press the hotkey again while it is spinning to abandon the
  request**, rather than queueing a second one.
- On success the flash reads `copied 128 chars to clipboard`; on
  failure it names the error and stays up for eight seconds.

This one needs a provider. There are four, and they differ in what they
cost you, not just in how they work:

| `ocr.provider` | Needs | Image leaves the LAN? | Speed | Quality |
|---|---|---|---|---|
| `pikvm` | nothing — stock firmware has it | no | slowest; scales with area | Tesseract |
| `command` | a host tool, e.g. `tesseract-ocr` | no | your host's | whatever you point it at |
| `openai` | an API key | yes | one round trip | reads screens well |
| `tui` | a Kultivator account | yes | one round trip | reads screens well |

The trade is not subtle. `pikvm` costs nothing and sends nothing, and is
also several times slower and visibly less accurate than the two
model-backed providers — [Choosing a provider](#choosing-a-provider) has
the measured numbers.

**`pikvm` is the default and needs no configuration at all.** The PiKVM
runs its own Tesseract and will OCR a rectangle of the frame it is
already serving, so nothing is installed anywhere and the screenshot
never leaves your network. Confirm your firmware has it with:

```bash
curl -ks -u admin:PASS https://<pikvm>/api/streamer/ocr
```

```yaml
ocr:
  provider: pikvm
  langs: ["eng"]        # whatever /api/streamer/ocr lists; stock has eng + osd
```

> **Keep `pikvm` selections small.** OCR runs on the Pi, and the time
> scales with area: on a PiKVM 3 a 300×100 region took about 2 s, an
> 890×320 region about 11 s, a 1400×600 region about 31 s, and a full
> 1920×1080 frame exceeded the 60 s timeout nginx enforces in front of
> kvmd and came back 504. The other providers do not have this problem.

**`command`** runs any host program with the PNG on stdin and reads the
text off stdout. On noble, `sudo apt install tesseract-ocr` is about
8 MB:

```yaml
ocr:
  provider: command
  command: ["tesseract", "-", "-"]
```

Nothing is linked against it, so an install using another provider
carries no dependency on it.

**`openai` and `tui`** send the selected image to a model, which reads
screens considerably better *and* several times faster than Tesseract —
especially dense UI, proportional fonts and anything where a transposed
character matters.
They differ only in endpoint and auth header (`Authorization: Bearer`
versus `X-API-Key`):

```yaml
ocr:
  provider: openai
  api_key: "secret://openai-key"
  # endpoint: "https://api.openai.com/v1/chat/completions"   # the default
  # model: "auto"
  # reasoning_effort: "none"        # transcription needs no reasoning
  # max_tokens: 8000                # below this the model can return
                                    # 200 with empty content
  # prompt: "..."                   # default asks for verbatim text
```

Because the wire shape is the common one, `provider: openai` with an
`endpoint` pointed at a local Ollama, vLLM or LM Studio works without
any new code — and keeps the image on your own machine.

`api_key` is a `secret://` reference resolved exactly like a PiKVM
password (see [Configure secrets](#configure-secrets)).

`tui` points at Kultivator's own endpoint and needs an account there, so
it is not something an arbitrary clone of this repo can just switch on.
Without one, `openai` — against OpenAI, or against a model you run
yourself via `endpoint` — gets you the same class of result, and `pikvm`
gets you something that works today with nothing to sign up for.

#### Choosing a provider

Start on `pikvm`: it works immediately and nothing leaves your network.
Move to a model-backed provider when the text matters more than keeping
the image local. Measured on one snapshot, cropped identically for both
sides, so this is the same pixels at the same moment:

| selection | `pikvm` | `tui` |
|---|---|---|
| 300×100 | 2.2 s | 0.5 s |
| 890×320 | 11.2 s | 3.4 s |
| 1400×600 | 31.0 s | 7.8 s |
| 1920×1080 (whole frame) | 504 at 60 s | 500 — too large to send |

Both track pixel area at about the same rate, so the gap is a roughly
constant 3–4× rather than something that closes at one end; run-to-run
variance is about a second on the larger regions.

**Both have a ceiling, and they are worth knowing about, because a big
enough selection fails rather than merely taking a while.** The PiKVM's
OCR runs on the Pi behind an nginx timeout it cannot ask for more time
from, so past roughly a quarter of a 1080p screen it stops being slow
and starts returning 504. Tui's limit is request size rather than time:
its front end accepts 1 MiB of request body, the PNG is base64'd into
the JSON at 4/3 its size, and a 1920×1080 screenshot PNG runs about
1.3 MB — so a whole-frame selection is refused instantly with a 500. The
largest selections measured getting through were around 1600×700 of
terminal content; a busier screen compresses worse, so treat it as a
budget in bytes, not a fixed rectangle. `openai` against OpenAI itself
has no comparable wall — it documents 20 MB per image.

Accuracy diverges on exactly the characters you would be extracting text
in order to avoid retyping. From the same 890×320 crop of a terminal:

| | `pikvm` | `tui` |
|---|---|---|
| an email address | `first last@example.com` | `first_last@example.com` |
| a shell prompt | `USERGHOST-NZ-LL` | `USER@HOST-NZ-LL` |
| a command | `git 1` | `git l` |
| a commit SHA | two characters wrong | correct |
| a timezone | `+1360` | `+1300` |

Tesseract loses `_` to a space, `@` to `G` and `l` to `1`, and added a
few standalone `1` lines that were not on screen. Fine for a sentence of
prose; not fine for a hash, a path or an address.

Set `provider: none` to turn extraction off; the hotkey will still let
you drag a selection and will say that nothing is configured. The paste
hotkey is unaffected by any of this.

## Watchers

A watcher is a rectangle of the target's screen that Glasshouse checks on
**every decoded frame** and records an event about whenever it changes.
That rate is the point: a notification toast lives a few seconds, so
anything on a timer misses most of them.

Create one by dragging — `Ctrl+Alt+Shift+W`, then type what you care
about — or from an MCP client with `glasshouse_watcher_create`. Both ask
for the same sentence, and it is not decoration: the rectangle says
*where* to look and nothing about *what matters there*, so whatever
reads the event later needs it to judge.

**Nothing is pushed anywhere.** Events queue in the viewer until
something collects them with `glasshouse_watcher_poll`, which takes a
cursor and returns one. That is deliberate — polling needs no protocol
feature, so any MCP client works, and Glasshouse keeps no subscriber
list, no outbound connection and no opinion about whether the consumer
is running.

Detection is local, free, and needs no OCR provider configured. It is a
mean luma difference against the previous frame over the rectangle only,
sampled with a stride, so a large rectangle costs about what a small one
does.

```yaml
watch_hotkey: "Ctrl+Alt+Shift+W"

watchers:
  enabled: true
  max: 10                  # ten is the designed ceiling
  threshold: 12            # mean luma delta, 1-255, that counts as a change
  cooldown_ms: 3000        # floor on how often one watcher can fire
  settle_ms: 600           # trigger: settle — how long "quiet" has to last
  history_bytes_per_watcher: 16777216
  history_pool_bytes: 96000000
  min_history_frames: 30   # sets how large a region may keep history
```

### Two triggers

`change` (the default) fires the instant the region changes, which is
what a transient needs. `settle` fires once a region that *was* changing
has gone quiet for `settle_ms` — "the page finished loading". It is a
per-watcher property because the two want opposite capture moments.

A region that never goes quiet — a clock, a progress bar, a video —
reports at the cooldown's rate rather than once and then never again.

### History

Ask for `keep_history` and the watcher keeps a rolling buffer of recent
crops, so you can look back *through* a change instead of only at the
frame that tripped it. Detection is always slightly late, and the most
useful frame is often the one before.

Frames are retained **on change, not on tick**, so a quiet region costs
one frame for as long as it stays quiet, and memory scales with activity
rather than with elapsed time. It is held in memory, never written to
disk, and dropped when the watcher is removed.

`glasshouse_watcher_frames` reads it back two ways. A **filmstrip**
tiles many moments into one image with each one's wall-clock time drawn
beneath it, so a model limited to a few images per request can still see
a whole transition; **stills** returns individual crops at full
resolution, which is what you want once the filmstrip has told you which
moment to read.

History is **refused rather than silently degraded** above an area
ceiling (about 186,000 pixels at the default budget — roughly 500×350).
The watcher still works; it just keeps no history, and the reply says
which of the two you got.

### Keeping the noise down

The three defences, in the order they matter:

1. **Drag a tight rectangle.** A sloppy one around a system tray
   contains a clock and fires every minute.
2. **The cooldown** stops a flicker becoming a stream of events.
3. **The pointer is ruled out for free.** The PiKVM captures the
   target's cursor over HDMI, so a pointer crossing the region looks
   exactly like the event — and Glasshouse knows where it put the
   pointer, so it suppresses detection while it is inside. If a watcher
   has gone quiet, check `suppressed` in `glasshouse_watcher_list`: the
   target's cursor may simply be parked in it.

> **A whole-screen rectangle is not a shortcut.** The metric is a *mean*
> over the rectangle, so a toast covering 2% of a 1080p frame moves it
> by about 4 — under the default threshold of 12, and it will not fire
> at all. Measured, not assumed. Watch the region you mean.

The local detector is meant to be a little trigger-happy, because
whatever reads the event is what decides whether the thing actually
happened.

### Telling a notification from a tab switch

A pixel comparison cannot do it — a banner and a browser tab switch move
the same pixels. But there is one difference that *is* mechanical, and
every event carries it:

| `transience` | what it means |
|---|---|
| `reverted` | The region went back to the content it had before. Something appeared and then left — the shape of a notification. |
| `persisted` | It changed and stayed changed. |
| `pending` | Not resolved yet. Poll again from a cursor just below that event's `seq`. |

> **`persisted` does not mean uninteresting.** A dialog waiting to be
> answered persists. So does a build that failed and is still showing
> the error. This signal only ever *ranks*; it never suppresses a
> firing, and it should not be read as a reason to ignore something.

What it is really for is deciding what to throw away. A transient's
frames are the only record it ever happened; a persistent change is
still on the screen and can be re-read with `glasshouse_screenshot` any
time. So when the 16 MB history fills, the persistent frames go first.

**And you can tell it outright.** `glasshouse_watcher_verdict` writes
`relevant` or `spurious` back onto an event. A `spurious` verdict is not
just a label: it drops that event's frames immediately and hands the
budget back, which is what keeps a busy watcher's history worth reading.
The event itself stays, because the pictures are what cost. The bundled
`watcher-triage` skill does this in a loop — and since the verdict is
stored on the event, it needs no cursor at all: poll from `0`, judge
whatever is still `unjudged`.

## MCP server

Glasshouse can expose the running session over
[MCP](https://modelcontextprotocol.io/) so other tools can see the
target's screens and drive its keyboard and mouse. It runs **inside the
viewer**, because the tools need the decoded video frames and the
session view — which head is where, which PiKVM carries HID — and
neither exists in a separate process.

**It is off by default, and it is a remote-control surface.** Anything
that can reach the port can type on the target and click on it. Turn it
on deliberately, leave `bind` on loopback unless you have a specific
reason not to, and set a `token` if anything else shares the machine.

### Enable it

```yaml
mcp:
  enabled: true
  bind: "127.0.0.1"
  port: 8765
  # token: "secret://mcp_token"     # requires Authorization: Bearer <value>
```

`token` is a `secret://` reference resolved exactly like a PiKVM
password (see [Configure secrets](#configure-secrets)). If it is set but
cannot be resolved, **the server does not start** — it will not fall
back to listening unauthenticated.

Restart the viewer, then register it with a client:

```bash
claude mcp add --transport http glasshouse http://127.0.0.1:8765/mcp
```

The transport is plain `POST` JSON-RPC over HTTP. There is no SSE
stream — a `GET` returns 405 — so clients poll rather than subscribe.

### Tools

| Tool | What it does |
|---|---|
| `glasshouse_session` | Screens, each head's rectangle within the combined desktop, which PiKVM carries HID. Call it first. |
| `glasshouse_screenshot` | Current frame of one screen as a PNG, downscaled to `max_width` (`0` for native). |
| `glasshouse_type` | Types text through the HID master. |
| `glasshouse_keys` | Presses a key or a chord, pressed in order and released in reverse. |
| `glasshouse_mouse_move` | Moves the pointer. `settle_ms` waits before replying, for callers about to screenshot. |
| `glasshouse_mouse_click` | Clicks, optionally moving there first. |
| `glasshouse_mouse_scroll` | Scrolls the wheel. |
| `glasshouse_watcher_create` | Registers a [watcher](#watchers) on a rectangle of one screen. |
| `glasshouse_watcher_list` | Every watcher, with fire counts and how much history it holds. |
| `glasshouse_watcher_poll` | Events since a cursor, plus the next cursor. The only delivery path. |
| `glasshouse_watcher_frames` | A watcher's retained crops, tiled into a filmstrip or returned as stills. |
| `glasshouse_watcher_verdict` | Marks events `relevant` or `spurious`; spurious frees their frames. |
| `glasshouse_watcher_delete` | Removes a watcher and drops its history. |

Coordinates can be screen-relative (`screen` plus `x`/`y`) or
desktop-absolute (`desktop_x`/`desktop_y`, origin at the top-left of the
combined desktop). Both map exactly: the monitor layout is configured
and verified against the streamer, so a desktop pixel converts straight
to absolute HID coordinates with **no calibration step**.

> **Watch the two numbering schemes.** `screen` is a 0-based index into
> the list `glasshouse_session` returns. `target_monitor` / `monitor_id`
> in your config is 1-based. The monitor you call "screen 2" is
> `screen: 1` to these tools; `glasshouse_session` reports both so you
> can check.

Every input tool refuses unless there is a HID master that is connected
and whose kvmd reports the target's HID online, and says which of those
failed — a keystroke that goes nowhere is worse than a refusal. Tool
calls flash on the status bar of every window, because something else
driving your keyboard should not be silent.

### Trying it without hardware

```bash
./build/bin/glasshouse-mcpprobe        # synthetic two-head session on 18766
```

Serves a 3840×1080 two-screen session with no PiKVMs attached, so you
can inspect the tool surface and schemas. The input tools correctly
refuse, since there is no HID master to route to.

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

# MCP probe — synthetic two-head session on 18766, no PiKVMs needed.
./build/bin/glasshouse-mcpprobe
```

## Project layout

```
glasshouse/
├── CMakeLists.txt              top-level build; declares yaml-cpp via FetchContent
├── CMakePresets.json           `default` = Debug in ./build; `release` in ./build-release
├── CHANGELOG.md                release-by-release changes
├── README.md                   you are here
├── CONTRIBUTING.md             how to file issues, send patches, cut releases
├── SECURITY.md                 how to report security-affecting bugs
├── LICENSE                     LGPL-3.0 (top-level for GitHub auto-detection)
├── LICENSES/                   LGPL-3.0 + MDI icon NOTICE
├── specs/DESIGN.md             architectural source of truth
├── specs/HARNESS.md            design for the harness built on the MCP server
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
├── src/mcp/                    HTTP front end, MCP protocol, tools, glasshouse-mcpprobe
├── src/cli/                    glasshouse-cli (Phase 1 harness)
├── src/viewer/                 glasshouse-viewer (the GUI; ties everything together)
└── tests/                      QTest: config validator, coord transform, keymap,
                                MCP transport + protocol
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

See [CONTRIBUTING.md](CONTRIBUTING.md) for build/test, where to file
issues, conventions, and how releases are cut. Three load-bearing
rules summarised here:

- **Empirical verifications matter.** When implementation contradicts
  the design (latency, API behaviour, Qt quirks, PiKVM firmware
  surprises), update `specs/DESIGN.md` §10 in the same commit that
  reveals it.
- **Tests for pure logic.** Pure-function code — config validation,
  coord transform, keymap — must have QTest coverage in `tests/`.
  Network-heavy behaviour is exercised by the harnesses against real
  hardware.
- **Don't modify the PiKVM.** Glasshouse runs against stock firmware;
  workarounds for firmware quirks live client-side.

Security-affecting bugs go via [SECURITY.md](SECURITY.md) — please
don't open public issues for those.

## Design and references

- [specs/DESIGN.md](specs/DESIGN.md) — architecture, coord model,
  PiKVM API subset, empirical verifications. The MCP server is §10.11.
- [specs/HARNESS.md](specs/HARNESS.md) — where the MCP server is
  going: watchers, frame-change detection, and what consumes the
  events.
- [Model Context Protocol](https://modelcontextprotocol.io/) — the
  protocol the server speaks.
- [CHANGELOG.md](CHANGELOG.md) — what changed, version by version.
- [PiKVM handbook](https://docs.pikvm.org/)
- [PiKVM HTTP API reference](https://docs.pikvm.org/api/)
- [kvmd source](https://github.com/pikvm/kvmd) (the Janus signalling
  reference is `web/share/js/kvm/stream_janus.js`)
- [ustreamer source](https://github.com/pikvm/ustreamer) (the Janus
  plugin's accepted verbs are in `janus/src/plugin.c`)

## License

Glasshouse is licensed under [**LGPL-3.0-or-later**](LICENSE),
matching the dynamic Qt 6 linkage requirement (Qt 6 is LGPL v3 with
The Qt Company's GPLv3 exception). Full text lives in
[LICENSE](LICENSE) at the repo root and at
[LICENSES/LGPL-3.0.txt](LICENSES/LGPL-3.0.txt).

The MDI greenhouse glyph used as the application icon is distributed
under the Pictogrammers Free License — see
[LICENSES/material-design-icons-NOTICE.txt](LICENSES/material-design-icons-NOTICE.txt).
