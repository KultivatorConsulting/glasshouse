---
name: watcher-triage
description: Triage Glasshouse watcher events — poll them, work out which were real and which were noise, and write the verdicts back so spurious ones stop eating the history budget. Use when the user asks what their watchers have caught, asks to clean up or prune watcher history, or when a watcher's retained history is filling up with false positives.
---

# Watcher triage

Glasshouse's watcher detector compares pixels. That is deliberate — it
runs on every decoded frame so it can catch a toast that lives for two
seconds, and a pixel comparison is the only thing cheap enough to do
that. The price is that it cannot tell a notification from a browser tab
switch, because both move the same pixels.

HARNESS.md §5 calls judging that difference step 5, and gives the job to
whatever consumed the event. That is this skill.

## When to use

- "What have my watchers caught?" / "Any updates?"
- "Clean up the watcher history" / "prune the spurious ones"
- A watcher's `history_bytes` is at its budget and the lookback window
  has got too short to be useful.

## The cursor problem, and why there isn't one

Do **not** try to remember a poll cursor between runs. A verdict is
stored on the event, so the verdict *is* the bookmark: poll from 0, and
judge anything whose `verdict` is still `unjudged`. Nothing to carry,
and it self-heals if a run is interrupted half way.

## Procedure

1. `glasshouse_watcher_poll` with `cursor: 0`, `include_images: true`,
   `max_images: 4`. The event list is small JSON; only the images cost,
   which is why the cap matters.
2. Drop from consideration anything with `verdict` already set — it has
   been triaged.
3. Leave anything with `transience: "pending"` alone. Its region has not
   yet had its chance to go back, and judging it now is judging on a
   result that is not in. It will resolve within the revert window and
   be there next run.
4. For the rest, decide `relevant` or `spurious` using the guidance
   below. You will usually have more events than attached images — poll
   again with a cursor just below a specific seq, or use
   `glasshouse_watcher_frames`, when you genuinely cannot tell without
   looking. Do not pull images for events you can already judge.
5. Write the verdicts back in **two calls, not twenty**:
   `glasshouse_watcher_verdict` with `seq` as an array, once for
   `relevant` and once for `spurious`. Add a short `note` saying why.
6. Report to the user: what was caught that matters, how much history
   was freed, and anything still pending.

## Judging

The watcher's `intent` is the specification. It is written by whoever
made the watcher and it says what they cared about — read it first and
judge against it, not against your own sense of what looks important.

`transience` is the strongest mechanical signal:

- **`reverted`** — the region went back to the content it had before.
  Something appeared and then left. That is the shape of a notification,
  a toast, a flash of status. These are usually what the user wanted,
  and their frames are the *only* record that it ever happened.
- **`persisted`** — the region changed and stayed changed.

**`persisted` is not a reason to call something spurious.** A dialog
waiting to be answered persists. A build that failed and is still
showing the error persists. A prompt asking whether to continue
persists. Those are among the most important things a watcher can
catch. Persistence only tells you the thing is *still on screen*, which
means you can go and look at it with `glasshouse_screenshot` — it does
not tell you it was uninteresting.

What actually makes an event spurious:

- The crop shows chrome that merely happens to live under the watched
  region — a browser tab strip, an address bar, a status line — rather
  than the thing the intent describes.
- The region is on a clock, a progress bar or a video, and the event is
  just that rate ticking over.
- The event is the *tail* of a transient already judged relevant: the
  banner leaving is not a second notification.

When you genuinely cannot tell, leave it unjudged. An unjudged event
costs a few hundred bytes and gets another look next run; a wrong
`spurious` throws away the only copy of something.

## What this skill does not do

It does not create, retune or delete watchers, and it does not change
thresholds. If triage keeps finding the same false positive, say so and
recommend a tighter rectangle — do not quietly paper over it by marking
the same thing spurious every run.
