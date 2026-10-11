# Glasshouse harness — design

Status: **proposed, unbuilt.** Captured 2026-10-11 from a design
discussion; §5.4 added 2026-10-12. Nothing here is implemented; the
pieces it depends on (region capture, clipboard paste) are. See
DESIGN.md §10.9 / §10.10.

---

## 1. What it is

An assistant that watches the target machine over the PiKVMs, works out
what is happening, and tells the user about it **in a chat window they
open when they want it** — not in popups that interrupt.

Representative uses, from least to most involved:

* Draft text to be typed into something on the target.
* Analyse a region of the screen the user cut out and offer insight, or
  research the problem they are stuck on.
* Watch a part of the screen the user described in words and, when it
  changes, judge whether the thing they cared about actually happened.

The last one is the reason the rest exists. The target sits behind a
KVM, so its own notifications cannot reach the user — they appear on a
screen that may be on another desktop, or that the user has walked away
from. The harness is the bridge.

## 2. Why this lives in Glasshouse and not in PiKVM

"One monitor, one computer" is a hardware property of a PiKVM: one HDMI
capture path, one USB HID gadget. Multi-head is a different product, not
a patch.

The deeper reason is that **no single PiKVM can know the others exist.**
The combined desktop — the logical extent, which monitor is on the left,
which box carries HID — is a relationship *between* devices, and can
only exist where the streams meet. That is the client. Even a
hypothetical multi-head PiKVM would still need something to present N
streams as N windows and route one input session across them.

A fork would also have to be maintained across real drift: the two units
in this deployment are different hardware generations running different
firmware (§13.5).

**The model: PiKVM is a per-head device driver, Glasshouse is the
session layer.** For any capability, the only question is whether it is
per-head or per-target.

| | examples | where it lives |
|---|---|---|
| per-target | keyboard, mouse, paste, shortcuts, ATX, MSD | routed to the HID master |
| per-head | video, streamer tuning, GPIO, health, capture resolution | per window / per instance |

Everything already built falls out of that rule, which is a good sign it
is the right one. `InputRouter` *is* this rule in code.

The things that can never move down into PiKVM are exactly the things
the harness needs most: monitor indexes, the proportional coordinate
transform, the single grab holder, cross-window wheel routing, pointer
confinement across the session bounds, layout-drift detection. They are
not features PiKVM lacks; they are features that can only exist a layer
up.

## 3. Tool surface: a Glasshouse MCP server

Kahu reaches server-side tools over MCP rather than compiling them in,
so Glasshouse supplies an MCP server rather than tool code.

There is an existing PiKVM MCP server, and it is tempting to reuse. It
is the wrong choice here: it speaks per-PiKVM pixel coordinates, so an
agent using it cannot reason about the desktop as a whole. "Restore the
windows that were closed after a restart" spans both screens, and an
agent that does not know the two monitors are adjacent — or which is on
the left — cannot do it.

The Glasshouse MCP server exposes the session the way the user sees it:
screen indexes, the combined desktop, HID-master routing.

## 4. It proposes; it does not act

**Decided.** The assistant offers; acting is something the user asks for
explicitly, gated the way destructive changes already are (the ATX
confirmation is the existing precedent).

This removes a whole class of design. An earlier sketch had the agent
acting autonomously, which would have meant arbitrating the shared HID
between the user and the agent — a lease, pre-emption, the user always
winning. None of that is needed if the agent only acts when asked,
because the user is present and not typing. A kill switch and an action
log are enough.

**Gate at the granularity of a request, not an action.** Confirming
every keystroke would make the one use the user actually anticipates —
helping draft text to be typed into the target — unusable. "Put this
paragraph in that window" is one confirmation showing the text.

Which means the first act path is nearly built already: the assistant
drafts, the user approves, the text goes in through `/api/hid/print`,
reusing the sanitiser, the sequenced chunks and the abort from §10.9.
The only new part is a model generating the text instead of the
clipboard holding it.

**Consequence worth stating: mouse control is not on the critical
path.** Typing text and sending chords need no coordinates, so the hard
problem — a model hallucinating "click there" — can be deferred,
possibly indefinitely. Mouse control waits until there is a demonstrated
need for it.

## 5. Watchers

The user describes what to watch in words. Resolving that description to
a rectangle is expensive and happens once; checking the rectangle is
cheap and happens continuously.

> "Watch the system bar, and if a bullet with a number appears over the
> Chrome icon, that is a new email — let me know."

1. **Locate (once, costly).** Send the full screen to Tui with the
   description; it returns the best bounding rectangle.
2. **Register.** Store `{screen index, rect in frame pixels,
   description, prompt, trigger kind, cooldown, reference patch}`.
3. **Watch (continuous, local, free).** Compare the rectangle against
   its reference. No model involved.
4. **Fire.** On a change, snapshot the crop and open a Kahu turn with it
   and the original intent.
5. **Judge.** The assistant decides whether the thing actually happened,
   with the whole conversation as context, and answers in the
   conversation.

The local detector is allowed to be trigger-happy, because the model
filters it. That is what lets a cheap pixel comparison be good enough.

### 5.1 The description is the truth; the rectangle is a cache

Anchors drift. Taskbar icons re-flow as applications open and close, and
a resolution change invalidates every stored rectangle outright.

The durable thing is the sentence the user wrote. Coordinates are a
derived lookup that can always be recomputed by replaying step 1. The
layout-drift detection in §10.8 is the natural hook: when it fires, mark
the registry stale and re-locate.

Rectangles are stored in **frame pixels with a screen index**, not
window coordinates, so they survive the user moving or resizing a
Glasshouse window.

### 5.2 Noise is where this will bite

A sloppy rectangle around a system tray contains a clock, and fires
every minute. Worse, the PiKVM captures the target's cursor over HDMI,
so the pointer crossing the region trips it, as do hover highlights and
tooltips.

Three cheap defences:

* Ask for a tight rectangle at locate time.
* Edge-trigger with a cooldown, rather than firing on every difference.
* Suppress while the pointer is inside the region — Glasshouse already
  knows where the pointer is, so this one is free.

### 5.3 Transient events break periodic polling

A notification toast lives a few seconds. Polling its region every ten
seconds misses most of them.

Because detection is local and the rectangle is small, run it **on every
decoded frame** (~27 fps) rather than on a timer. A few thousand pixels
per frame is affordable, especially with OpenCV doing the compare.

This also inverts the capture rule. For a steady event — "the page
finished loading" — wait for the region to settle before capturing. For
a transient one, **snapshot at the instant of detection**: by the time
Tui answers, the toast is gone. So it is a per-watcher property, not a
global policy. Detection is also always slightly late, which is what
§5.4 is for.

A fixed region such as "centre top of the primary screen" is the easy
anchoring case — it does not drift, so §5.1 matters much less there.

### 5.4 Volatile watchers keep a short history

Marking a watcher **volatile** gives its rectangle a rolling buffer of
recent crops, so the assistant can look back through a change instead of
seeing only the frame that tripped the trigger.

This matters because detection is late by construction. A threshold
trips once a toast has substantially faded in, and the single most
useful frame is often the one *before* the change — the one that says
what was there to begin with. A buffer supplies both ends of a
transition without having to guess when to sample.

Three properties keep it affordable.

**Buffer the crop, not the frame.** A toast-sized region of 420x110 is
about 135 KB as raw RGB, against 6 MB for the full frame. The crop
already exists; the detector made it.

**Retain on change, not on tick.** The detector computes a difference
metric every frame to drive the trigger, so reuse that number to decide
whether a frame is worth keeping. A quiet region then costs one frame
for as long as it stays quiet, and a whole toast lifecycle — fade in,
dwell, fade out — costs tens of frames rather than hundreds. Memory
scales with activity instead of with elapsed time, which is the right
shape for something left running all day. Cap each watcher in bytes,
evicting oldest first.

**Keep them raw; encode only what is sent.** PNG-encoding at frame rate
would spend the one resource already under pressure — two active streams
sit near 133% CPU (DESIGN.md §10.3). Compression belongs at the point of
transmission, where the frame count is in single digits.

Frames carry wall-clock timestamps rather than only frame indexes. "A
message arrived" is far less useful than "a message arrived at 14:32",
and the motivating case is explicitly about returning to a machine after
being away from it.

A second, decimated lane — one frame every couple of seconds across the
last few minutes — answers "what happened while I was away" as opposed
to "what just happened". With change-based retention it is nearly free
on a quiet region.

#### Getting a timeseries past a four-image limit

Tui accepts four images per request, so a buffer of thirty crops cannot
simply be attached. Two mechanisms, both wanted:

* **Filmstrip.** Tile crops into a grid with each one's timestamp drawn
  beneath it: `cv::hconcat` / `cv::vconcat` and `cv::putText`, all
  inside the OpenCV dependency §6 already justifies. Nine of those
  420x110 crops tile to 1260x330, a fraction of the ~2.1 MP threshold
  above which Tui downscales server-side (DESIGN.md §10.10) — so one
  image slot carries nine moments and four carry thirty-six.
* **A queryable buffer.** Expose the ring as a tool on the MCP server
  in §3: fetch a watcher's frames between two times. The firing turn
  then carries a filmstrip to judge from, and the model asks for a
  specific moment at full resolution when it needs to read something.

The second is the more valuable. It makes the history something the
assistant navigates on demand rather than a larger fixed payload, and it
is what turns "it has context on what has been going on" into a property
of the system instead of a hope about conversation length.

**The footgun is a large volatile region.** Full-screen at frame rate is
roughly 6 MB per frame, so the flag needs an area ceiling, above which a
watcher is forced into the decimated lane whatever it was marked.

## 6. Local vision: OpenCV

Available from the distribution (`libopencv-core406t64`,
`libopencv-imgproc406t64` on noble — the same `t64` convention as the Qt
runtime deps already in `dist/nfpm.yaml`). No PPA, nothing that touches
system Qt.

It earns its place by fixing the two weakest points above, not by being
faster:

* **Template matching re-finds a drifted anchor locally**, so a
  re-flowed taskbar icon costs nothing instead of another locate call.
  The rectangle becomes self-healing.
* **Structural comparison ignores the noise in §5.2** far better than a
  raw pixel difference.
* Preprocessing — tight-cropping, upscaling, cleaning — should improve
  extraction quality on small text.

**The discipline: OpenCV answers "did this change, and where did this
patch move to". Tui answers "what does this mean".** The failure mode is
acquiring a taste for it and starting to hand-write detectors for
specific UI elements, at which point the result is a brittle vision
system instead of a model that already works.

## 7. Kahu integration

Kahu is a toolkit for building harnesses, with Tui as the model backend;
it reaches tools over MCP (§3). Its UI is Vue, so embedding means either
a web view (the user's preference: "Tauri or something") or a native Qt
client speaking `contract/PROTOCOL.md`. **Not yet decided** — see §9.

**Kahu has no server-initiated messages.** Per `contract/PROTOCOL.md`
§1, "the stream is opened by the request": every exchange begins with a
client `turn.start`, and the server streams `hello` / `token` / `done`
back. There is no channel for the server to push into a conversation.

So **Glasshouse starts the turn** when a watcher fires. It posts the
event — crop plus original intent — and the reply streams into the
conversation for the user to find. This needs no protocol extension and
keeps the watcher loop owned by Glasshouse, which is where it belongs.

The client mints `turnId` and the server **must** deduplicate on it.
Deriving a stable id from the event therefore gives free protection
against a twitchy detector firing the same event twice.

## 8. Surfacing

Two channels, deliberately different.

**The conversation is the inbox.** A watcher firing is a tool call in a
conversation that already has context, so the assistant is not reporting
"pixels changed" — it is interpreting an event against everything else
the user has been doing. Items persist until read; a restart must not
lose an unread one. The only ambient signal is something quiet, such as
an unread count in the status bar (the extraction-spinner slot added in
§10.10 is the right shape).

**A host notification, per watcher, opt-in.** Most watchers stay quiet.
A few — "tell me when Ash replies" — are explicitly asked to chase the
user, because that is the entire point of bridging a machine that cannot
reach them. `org.freedesktop.Notifications` over D-Bus; a daemon is live
on the session and Qt's DBus module is already in the build Qt. A
notification can carry an action, so it should open straight to that
conversation rather than merely saying something happened.

## 9. Open questions

* **UI shell.** Web view (Tauri or QtWebEngine) versus a native Qt
  client against `contract/PROTOCOL.md`. QtWebEngine means Chromium in a
  `.deb` whose client links system Qt 6.4. Read the protocol spec before
  choosing.
* **Where the backend runs.** `docker compose` on the workstation, a
  local Go binary, or remote. Live mode needs `TUI_API_KEY`.
* **Watcher persistence format**, and how many are realistic before the
  per-frame comparisons stop being free.
* **The per-watcher byte budget** for §5.4 buffers, the area ceiling
  above which volatile is refused, and whether the decimated lane is
  worth building at the same time as the hot one.

## 10. Privacy posture

This is a real change and should not be allowed to go stale.

DESIGN.md §11 carries a risk row saying region capture sends target
screen content to an external service, mitigated by the user choosing
the rectangle each time so nothing leaves without a deliberate drag.
**Watchers break that mitigation** — nobody chooses each frame.

What holds it together instead: the locate step sends one full screen
once, per watcher, at the user's request; everything after that is local
until a change fires, and then only a small crop leaves. So the volume
is low and bounded by events rather than by time — but it is a standing
flow rather than an on-demand one, and that row needs rewriting when
this is built, not left as-is.

**Volatile watchers (§5.4) need their own line in it.** A rolling buffer
is a further step again: screen content is *retained* rather than merely
passed through. It must stay in memory, never reach disk, and be dropped
when the watcher is removed or the window closes. And because the MCP
tool makes it fetchable, the ceiling on what can leave in one turn
becomes the buffer's depth instead of a single crop.
