# Glasshouse harness — design

Status: **proposed, unbuilt.** Captured 2026-10-11 from a design
discussion; §5.4, §5.5, §6.1, §8.1 and the §9–§10 decisions added
2026-10-12.
Nothing here is implemented; the pieces it depends on (region capture,
clipboard paste) are. See DESIGN.md §10.9 / §10.10.

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
reusing the sanitiser, the sequenced chunks and the abort from
DESIGN.md §10.9.
The only new part is a model generating the text instead of the
clipboard holding it.

**Consequence worth stating: mouse control is not on the critical
path.** Typing text and sending chords need no coordinates, so the hard
problem — a model hallucinating "click there" — can be deferred,
possibly indefinitely. Mouse control waits until there is a demonstrated
need for it.

## 5. Watchers

The user says what to watch — in words, or by dragging the rectangle
themselves (§5.5). Getting the rectangle happens once; checking it is
cheap and happens continuously.

> "Watch the system bar, and if a bullet with a number appears over the
> Chrome icon, that is a new email — let me know."

1. **Locate (once).** Either send the full screen to a vision provider
   with the description and take the rectangle it returns, or take the
   rectangle the user dragged. Two modes, one output — see §5.5.
2. **Register.** Store `{screen index, rect in frame pixels,
   description, prompt, trigger kind, cooldown, reference patch}`.
3. **Watch (continuous, local, free).** Compare the rectangle against
   its reference. No model involved.
4. **Fire.** On a change, snapshot the crop, append the event to the
   watcher's log and — if the watcher asked for it — raise a host
   notification. Nothing is pushed to a consumer; the event waits to be
   collected via `watchers/poll` (§9.1).
5. **Judge.** Whatever consumed the event decides whether the thing
   actually happened, with the conversation as context, and answers
   there. §8.1 has the three kinds of consumer this can be.

The local detector is allowed to be trigger-happy, because the model
filters it. That is what lets a cheap pixel comparison be good enough.

### 5.1 The description is the truth; the rectangle is a cache

Anchors drift. Taskbar icons re-flow as applications open and close, and
a resolution change invalidates every stored rectangle outright.

The durable thing is the sentence the user wrote. Coordinates are a
derived lookup that can always be recomputed by replaying step 1. The
layout-drift detection in DESIGN.md §10.8 is the natural hook: when it
fires, mark the registry stale and re-locate.

Rectangles are stored in **frame pixels with a screen index**, not
window coordinates, so they survive the user moving or resizing a
Glasshouse window.

None of this applies to a hand-drawn rectangle, which has no sentence to
replay. §5.5 gives it a different and cheaper recovery path.

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

**Sizing, with ten watchers as the stated ceiling.** One knob: 16 MB per
volatile watcher, letting the region's size decide the depth rather than
configuring a frame count. At 135 KB a frame that is about 120 retained
frames, several complete toast lifecycles. A 96 MB global pool across
all volatile watchers bounds the worst case, and at the four or five
that are realistic it is never approached.

The area ceiling then falls out of that budget instead of being a second
number to keep consistent with the first. Require a minimum useful depth
of 30 retained frames, and 16 MB / 30 is about 533 KB a frame — roughly
0.17 MP, some 500x350. Generous for a toast or a tray badge, and it
refuses anything window-sized.

Frames carry wall-clock timestamps rather than only frame indexes. "A
message arrived" is far less useful than "a message arrived at 14:32",
and the motivating case is explicitly about returning to a machine after
being away from it.

**A second, decimated lane is deferred.** One frame every couple of
seconds across the last few minutes would answer "what happened while I
was away" as opposed to "what just happened", and with change-based
retention it would be nearly free on a quiet region. But it is a second
retention policy and a second eviction path, and §8 already answers the
question it was for: the conversation is the inbox, so what happened
while the user was away sits in the assistant's persisted message, not
in replayed frames. The hot ring has the narrower job of letting the
model read a toast that has already gone. Build that, and add the cold
lane only if a question turns up that the hot ring demonstrably cannot
answer.

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
roughly 6 MB per frame — 44 frames would exhaust the entire global pool.
Above the ceiling, volatile is **refused rather than silently
degraded**: the watcher still works, it simply keeps no history, and the
user is told which of the two they got.

### 5.5 Manual and prompted selection are peers

Dragging the rectangle is a first-class way to create a watcher, not a
fallback. `RegionSelector` already emits `selected(QRect imageRect)` in
image-pixel coordinates — precisely the storage format step 2 wants — so
a manual watcher is the existing, proven selector feeding a registry
entry, on the free `Ctrl+Shift+Alt+W`, with the same drag already
trusted for extraction (DESIGN.md §10.10).

It earns its place on three counts:

* **No provider needed.** Prompted selection requires a vision-capable
  provider (§6.1); dragging requires nothing. A `command`-only or
  fully offline install gets watchers.
* **No locate error.** A described region can come back wrong. A
  dragged one is exactly what was meant.
* **It is how the detector gets debugged.** Testing §5.2's noise
  defences and §5.4's retention needs a rectangle, not a round trip to
  a model.

**Manual replaces step 1 and nothing else.** In particular it does not
replace the intent text: "watch this rectangle" says nothing about what
to care about, so step 5 still needs the sentence. The prompt and the
locating are separable, and only the second is what the two modes
disagree about.

**The drift story differs, and manual comes out ahead.** §5.1 rests on
replaying the description, which a hand-drawn rectangle cannot do. What
it can do is template-match its stored reference patch to re-find
itself — the self-healing §6 already proposes for drifted anchors. That
is local, free, and needs no model call, so a manual watcher recovers
from a re-flowed taskbar more cheaply than a described one does. Both
modes should therefore store the reference patch, which step 2 already
says they do.

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
patch move to". A model answers "what does this mean".** The failure
mode is acquiring a taste for it and starting to hand-write detectors
for specific UI elements, at which point the result is a brittle vision
system instead of a model that already works.

### 6.1 The remote half is pluggable

Region extraction (DESIGN.md §10.10) currently constructs a `TuiClient`
directly. That was right for one feature on one workstation and wrong as
the only
option in a public LGPL-3.0 repository: a clone that does nothing
without a Kultivator API key is a poor artifact, and it leaves the
privacy row in §11 with no mitigation but "the user chose the
rectangle".

So extraction goes behind an interface. `TuiClient` already has exactly
the right surface — `extractText(QByteArray png)`, `busy()`, `cancel()`,
`finished(ok, text, reason)` — so this is a rename and a base class, not
a redesign.

| provider | covers | cost |
|---|---|---|
| `none` | default for a fresh clone; the hotkey reports that nothing is configured | nil |
| `command` | any host tool, fed PNG on stdin, text from stdout | nil — no build or runtime dependency |
| `openai` | Tui, OpenAI, Ollama, vLLM, LM Studio — anything with `/v1/chat/completions` and `image_url` parts | the existing code, renamed |

**Three providers rather than four.** "Built-in OCR" and "the host's
OCR" collapse into `command`, because the generic form of host OCR on
Linux *is* a subprocess — there is no system OCR service to call, the
way macOS and Windows have one. Tesseract is in noble (5.3.4; about
8 MB with `libtesseract5` and the English data) and reads stdin to
stdout as `tesseract - -`, so local OCR becomes one line of
configuration instead of a linked dependency that every install carries
whatever provider it uses. Link `libtesseract` later only if the
subprocess hop proves to matter.

**Tui stops being special**: it becomes `provider: openai` plus the
endpoint, key and model fields the `ocr:` block already has. Existing
configurations keep working — default the provider to `openai` when an
`api_key` is set and `none` otherwise, and nothing needs migrating.

#### What degrades without a provider

Worth stating plainly, because the four stages of §5 do not have the
same requirements:

| stage | needs |
|---|---|
| region extraction (DESIGN.md §10.10) | any provider, including `command` |
| watcher **detection** | nothing — OpenCV, local, always available |
| watcher **locate** (§5 step 1) | a **vision-capable** provider; `command` and Tesseract cannot find "the Chrome icon in the system bar" |
| watcher **judge** (§5 step 5) | the harness's own model, which is the private package's business |

So watchers need a vision-capable provider configured, while region
extraction works with anything. That is a real constraint, not a
footnote: on a `command`-only install the watcher registry can still
*watch* an explicitly given rectangle, it just cannot resolve one from
a description.

## 7. Kahu integration

Kahu is a toolkit for building harnesses, with Tui as the model backend;
it reaches tools over MCP (§3). Its UI is Vue, so embedding it means a
web view rather than a native Qt client against `contract/PROTOCOL.md`:
porting the Vue kit to Qt is a large undertaking for what is, today, a
single tool used on one client. Revisit only if others adopt it.

**Decided 2026-10-12: Tauri, not QtWebEngine.** This inverts the
expected order, on evidence rather than taste — see §10.

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
DESIGN.md §10.10 is the right shape).

**A host notification, per watcher, opt-in.** Most watchers stay quiet.
A few — "tell me when Ash replies" — are explicitly asked to chase the
user, because that is the entire point of bridging a machine that cannot
reach them. `org.freedesktop.Notifications` over D-Bus; a daemon is live
on the session and Qt's DBus module is already in the build Qt. A
notification can carry an action, so it should open straight to that
conversation rather than merely saying something happened.

### 8.1 Three consumer tiers

A watcher firing into a log nobody reads is worth nothing, so it is fair
to ask what consumes the events. There are three answers, and only the
third is the harness:

| tier | consumer | what it gives | what it lacks |
|---|---|---|---|
| 1 | none — the viewer itself | the opt-in D-Bus notification above, fired directly: "the region you marked changed" | no judgement; it cannot tell a real event from the clock ticking |
| 2 | any MCP client | judgement on demand, on a surface that already exists | no background presence; it only looks when opened |
| 3 | `glasshouse-harness` | §8's two channels: accumulated context, proposal, persistence | has to be built |

Tier 1 costs almost nothing, because §8's D-Bus path is wanted anyway.
It is dumb but not useless — "something changed in the build output"
is a real thing to want.

**Tier 2 is the one worth noticing**, because it already works. Claude
Code is an MCP client; pointing it at the Glasshouse MCP server gives
`watchers/poll` a real consumer with no private-repo work at all. This
is the same way the existing PiKVM MCP server is already used.

**Which settles the build order.** Ship the public side first — watcher
engine, MCP server, both selection modes — and consume it from tier 2
until the event shape and the `watchers/poll` contract have been
exercised by a real client. Only then build the harness, against a
surface that has been used rather than imagined. Doing it the other way
round means freezing the tool contract against a hypothetical consumer
and discovering its shape is wrong after the Tauri shell exists, which
is the expensive part.

Tier 3 remains the end state, not a nicety: tier 2 is not sitting in the
background polling every thirty seconds, so "I stepped away — tell me
when Ash replies" genuinely needs something persistent. It is just not a
prerequisite for building or validating any of the rest.

## 9. Where the code lives: public viewer, private harness

Glasshouse is LGPL-3.0 and public (`github.com/KultivatorConsulting/`);
Kahu is proprietary and lives on `git.kultivator.net.nz`. Shipping a
Kahu backend inside `glasshouse-viewer` would put a proprietary payload
in a package that declares LGPL-3.0. So the harness is a **separate
private repository and a separate package**, `glasshouse-harness`,
depending on `glasshouse-viewer`.

**The split line is partly forced, not a matter of taste.** Detection
runs on every decoded frame against `QVideoSink` / the FrameMailbox.
Behind a process boundary that would mean moving 6 MB x ~27 fps x 2
streams across IPC, so the watcher engine is structurally part of the
viewer. The MCP server follows the same logic: it exists to speak for
the whole session — screen indexes, combined desktop, HID-master
routing — so it must live where that state is. Neither has any Kahu
dependency, and OpenCV is Apache-2.0, which sits fine under LGPL-3.0.

| public `glasshouse-viewer` | private `glasshouse-harness` |
|---|---|
| watcher registry, ring buffers, OpenCV detection | Tauri shell hosting `chat-vue` |
| MCP server (screens, desktop, HID, frame queries) | Kahu Go backend |
| the `watchers/*` tool family (§9.1) | the §7 turn-opening logic |

This is worth wanting for its own sake, not just for the licence: the
public repo stays complete. Someone cloning it gets watchers and an MCP
server they can point at their own agent — Claude Code, anything
OpenAI-compatible. What is proprietary is the particular Kahu harness,
not the capability. A viewer with a hole where the interesting feature
used to be would be a worse artifact.

### 9.1 Watching is tools, and the harness polls

The viewer does not push. **Watching is another MCP tool family, and
one of its calls polls for changes** — which means any harness, not
only Kahu, uses the same mechanism.

```
watchers/create   description -> locate -> registry entry
watchers/list
watchers/poll     cursor -> events since, plus the next cursor
watchers/frames   watcher + time range -> filmstrip or stills (§5.4)
watchers/delete
```

The model configures its own watchers through the same surface it reads
them from, and `watchers/frames` is the queryable buffer §5.4 already
argued for.

**Polling needs no protocol feature**, which is exactly why it is the
portable choice: polling is calling a tool repeatedly. What MCP adds is
the push alternative — `resources/subscribe` on a URI, then
`notifications/resources/updated` — and that is the part with uneven
support, since `subscribe` is an optional server capability and clients
vary in whether they handle the notification. Build on the floor, not
on the option.

Three further reasons polling is the right default here, beyond
portability:

* **The events are already buffered.** §5.4 holds a ring of timestamped
  crops, so `poll(since: cursor)` is served from data already in hand.
  There is no missed-event problem for push to solve — the buffer *is*
  the durability.
* **Delivery is decoupled from detection.** Detection runs at frame
  rate locally; a watcher fires a few times a day. Polling every
  10–30 s is ample, and the latency that mattered — a toast gone before
  it could be captured — was solved by the ring, not by delivery speed.
* **It inverts the dependency.** Glasshouse keeps no subscriber
  registry, no outbound connection, no delivery retry and no "what if
  the harness is down". It answers questions. That makes the generic
  event surface a property of the protocol shape rather than a rule
  someone has to remember not to break.

Two things remain available if wanted later. `watchers/poll` can
**long-poll** — block until an event or N seconds, with
`notifications/progress` as keepalive — which yields push-like latency
from plain request/response. And each watcher's current crop can
*additionally* be exposed as a resource URI, so clients that do support
subscriptions get them as an optimisation while polling stays the
contract.

**The hotkey is the other attachment point.** `Ctrl+Shift+Alt+H` starts
a third-party application and hands it the current context, exactly as
any other GUI program would be invoked: one-shot, stateless, no
knowledge of what it started. Nothing about the §8 host-notification
case needs more than this plus polling — the harness backend is a
viewer-spawned child (§10), so it is up whenever Glasshouse is, and the
chain is detect -> buffer -> harness polls -> D-Bus.

### 9.2 Consequences

* **Hosting.** `git.kultivator.net.nz:kultivator/glasshouse-harness`,
  beside `kahu-harness`, with a Jenkinsfile on the same `Dockerfile.ci`
  convention. A private `.deb` cannot ship from public GitHub Releases
  anyway. Public CI stays C++/Qt; Go, Rust and Node all land private.
* **Versioning.** `glasshouse-harness` will depend on
  `glasshouse-viewer (>= X)`, which makes the `project(... VERSION
  0.1.0)` that has never been bumped an actual problem rather than a
  cosmetic wart in the About box. Fix it before there are two packages
  to keep in step. The MCP surface needs versioning too, which MCP's
  capability negotiation already provides.
* **Secrets.** One file. The Tui key for DESIGN.md §10.10 extraction
  already resolves through `SecretResolver` from
  `~/.config/glasshouse/secrets.yaml`, so the harness adds a key there
  rather than inventing a second store.

## 10. Decisions, and what is still open

Settled 2026-10-12.

**UI shell: Tauri.** The `.deb` builds against noble's system Qt 6.4
(`qt6-base-dev`, in all three workflows), so the only QtWebEngine it can
depend on is noble's `6.4.2` — Chromium 102, from 2022. Kahu states a
floor in `docs/using/vue.md` as of 2026-09-03, and the **Popover API**
is on it, described there as "doing real work, not saving a few lines":
the thread row's action menu needs the top layer to escape an
`overflow-y: auto` list, and gets light dismiss and Escape for free.
Popover shipped in Chromium 114; `chat-vue` also uses `:has()`, which is
105+. So the menus would not open and the list would clip at its scroll
edge — broken, not merely degraded.

A current Chromium would mean bundling WebEngine from a newer Qt: the
150 MB-in-a-`.deb` problem, plus a Qt version split inside one package.
Tauri's engine clears the floor instead — noble carries WebKitGTK
**2.52.6** in `main` with security updates, past the Safari 17 bar, and
already installed on the workstation with its `-dev` package. Two
incidental wins: `main` is better maintained than QtWebEngine's
`universe`, and `rust/kahu-contract` already exists, so a Tauri shell's
Rust side can use Kahu's own protocol crate rather than reimplement it.

The cost is that the harness window is a separate process rather than a
Qt widget, so integration with KDE window management is looser than a
Glasshouse video window's. Acceptable for a chat window.

**Backend: local, on the host.** A Go binary in the
`glasshouse-harness` package (§9), spawned and owned by the viewer as a
child process so nothing runs when Glasshouse is not, with `TUI_API_KEY`
passed into its environment from the existing `SecretResolver`.

**Watcher count: ten at the ceiling, four or five realistically.** That
closes half of what used to be an open question — ten regions of a few
thousand pixels at ~27 fps is nothing, so the per-frame comparisons stay
free across the whole intended range. Buffer sizing follows from the
same number; see §5.4.

Still open:

* **Watcher persistence format.** Where the registry lives across
  restarts, and whether a stale rectangle re-locates eagerly on startup
  or lazily on first miss.
* **Which vision-capable providers are worth testing** for prompted
  locating beyond Tui (§6.1). Less pressing than it was: §5.5 means a
  provider-less install still gets watchers, so this decides how good
  *prompted* selection can be made, not whether watchers work at all.

## 11. Privacy posture

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

**§6.1 supplies the mitigation this section was missing.** With a
pluggable provider, "nothing leaves the machine" becomes a configuration
rather than an argument: `provider: command` keeps extraction entirely
local, and `provider: none` disables it. The honest form of the risk row
is therefore conditional on the configured provider, with the remote
case being the one that needs the reasoning above. Watchers are the
exception — their locate step needs a vision-capable provider, so a
fully local install can watch a given rectangle but cannot resolve one
from a description.
