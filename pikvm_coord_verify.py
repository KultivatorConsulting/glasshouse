#!/usr/bin/env python3
"""
PiKVM absolute-coordinate verification script.

Sends a deterministic sequence of absolute mouse coordinates to a PiKVM
via its HTTP API so you can observe the resulting cursor behavior on the
target host. The goal is to empirically determine:

  1. Whether (0, 0) places the cursor at target-desktop CENTER or TOP-LEFT.
  2. The integer range the API accepts (s16? s32? clamped? wrapped?).
  3. Whether the mapping is linear across the target's full logical desktop,
     or whether it's per-HDMI-output / per-monitor.

Recommended workflow:
  * On the TARGET host, in one terminal:
        sudo libinput debug-events --show-keycodes
    Watch for POINTER_MOTION_ABSOLUTE events while this script runs.
  * On a DEV machine (where this script runs), run:
        python3 pikvm_coord_verify.py --host pikvm-a.local --user admin
    The script pauses between each send so you can record what you see.

Requires: Python 3.8+, `requests` (pip install requests).

PiKVM setup prerequisites:
  * HID USB cable connected to the target host.
  * Mouse in absolute mode (default). The script verifies this and bails
    out if not. Set via the PiKVM web UI (System -> Mouse) or the API:
        curl -k -X POST -u admin:admin \\
             'https://<pikvm>/api/hid/set_params?mouse_output=usb'
"""

import argparse
import getpass
import sys
import time
import urllib3

try:
    import requests
except ImportError:
    sys.exit("Install requests first: pip install requests")

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


class PiKvm:
    """Minimal PiKVM HTTP client scoped to the handles this script needs."""

    def __init__(self, host, user, passwd):
        self.base = f"https://{host}"
        self.s = requests.Session()
        self.s.verify = False
        self.s.headers.update({"X-KVMD-User": user, "X-KVMD-Passwd": passwd})

    def hid_state(self):
        r = self.s.get(f"{self.base}/api/hid", timeout=5)
        r.raise_for_status()
        return r.json()["result"]

    def info(self):
        r = self.s.get(f"{self.base}/api/info", timeout=5)
        r.raise_for_status()
        return r.json()["result"]

    def move(self, to_x, to_y):
        r = self.s.post(
            f"{self.base}/api/hid/events/send_mouse_move",
            params={"to_x": to_x, "to_y": to_y},
            timeout=5,
        )
        # Surface rejections cleanly; some ranges may be refused.
        if not r.ok:
            raise requests.HTTPError(f"{r.status_code} {r.text.strip()}")
        return r.json()


def pause(msg, delay):
    print(f"    {msg}")
    time.sleep(delay)


def preflight(kvm):
    print("== Preflight ==")
    info = kvm.info()
    platform = info.get("hw", {}).get("platform", {})
    print(f"  Platform: {platform.get('base', '?')} ({platform.get('model', '?')})")

    state = kvm.hid_state()
    mouse = state.get("mouse", {})
    kbd = state.get("keyboard", {})

    if not mouse.get("absolute"):
        sys.exit(
            "  !! Mouse is NOT in absolute mode. Switch via the web UI "
            "(System -> Mouse) or:\n"
            "       /api/hid/set_params?mouse_output=usb\n"
            "     then re-run."
        )

    print(f"  mouse.absolute = True")
    print(f"  mouse.online   = {mouse.get('online')}")
    print(f"  mouse.outputs  = {mouse.get('outputs', {}).get('active')}")
    print(f"  keyboard.online = {kbd.get('online')}")
    print()


def probe(kvm, delay):
    # Each tuple: (human label, to_x, to_y, hypothesis to confirm/reject)
    tests = [
        ("origin",                       0,      0,
         "If cursor lands at DESKTOP CENTER -> signed/centered. "
         "If at TOP-LEFT -> unsigned/origin-zero."),
        ("+max x (s16)",                 32767,  0,
         "Expect right-edge of desktop (centered) or ~50% right (offset)."),
        ("-max x (s16)",                 -32768, 0,
         "Expect left-edge (centered) or origin (offset)."),
        ("+max y (s16)",                 0,      32767,
         "Expect bottom-edge (centered) or ~50% down."),
        ("-max y (s16)",                 0,      -32768,
         "Expect top-edge (centered) or origin."),
        ("top-left (s16)",              -32768, -32768,
         "Expect desktop top-left corner if signed/centered."),
        ("bottom-right (s16)",           32767,  32767,
         "Expect desktop bottom-right corner if signed/centered."),
        ("half positive",                16384,  16384,
         "Halfway between origin and bottom-right. Linear check."),
        ("half negative",               -16384, -16384,
         "Halfway between origin and top-left. Linear check."),
        ("beyond +s16",                  65535,  0,
         "Does the API clamp at s16_max, wrap, or reject?"),
        ("beyond -s16",                 -65536,  0,
         "Does the API clamp at s16_min, wrap, or reject?"),
        ("return to origin",             0,      0,
         "Should land at same point as the first test."),
    ]

    print("== Probing coordinate space ==")
    print(f"   Delay between sends: {delay}s. Move nothing on the target.")
    print()

    for label, x, y, hypothesis in tests:
        print(f"[SEND] {label:>20s}  to_x={x:>7d}  to_y={y:>7d}")
        try:
            kvm.move(x, y)
            print(f"       hypothesis: {hypothesis}")
        except requests.HTTPError as e:
            print(f"       !! REJECTED by API: {e}")
            print(f"       -> This tells us the value is outside accepted range.")
        pause("(observe target cursor & libinput debug-events output)", delay)

    print()
    print("== What to answer from your observations ==")
    print("  Q1. Where did (0, 0) land?")
    print("      a) center of full logical desktop -> s16 signed, centered")
    print("      b) top-left corner -> unsigned u16, origin at (0,0)")
    print("      c) somewhere else -> note exact location")
    print()
    print("  Q2. Did (32767, 32767) land at bottom-right of FULL 3-monitor")
    print("      desktop, or just at the bottom-right of ONE monitor?")
    print("      (This confirms whether the coord range spans the full")
    print("       logical desktop, as Option A's coord math assumes.)")
    print()
    print("  Q3. Are intermediate points linear?")
    print("      The 'half positive' point should be halfway between origin")
    print("      and bottom-right (or halfway to whatever bottom-right maps to).")
    print()
    print("  Q4. What happened with 65535 / -65536?")
    print("      - HTTP 400 -> API validates and rejects outside its range.")
    print("      - Cursor identical to 32767 / -32768 -> silently clamped.")
    print("      - Cursor at different position -> wider range or wrap.")
    print()
    print("  Once you have answers to Q1-Q4, the coord transform in the Qt")
    print("  client is a one-liner. Record results in DESIGN.md under")
    print("  'Empirical verifications'.")


def main():
    ap = argparse.ArgumentParser(
        description="Verify PiKVM absolute-coordinate range and mapping.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--host", required=True,
                    help="PiKVM hostname or IP (e.g. pikvm-a.local)")
    ap.add_argument("--user", default="admin",
                    help="PiKVM username (default: admin)")
    ap.add_argument("--passwd", default=None,
                    help="Password. Prompted interactively if omitted.")
    ap.add_argument("--delay", type=float, default=2.5,
                    help="Seconds between each send (default 2.5)")
    args = ap.parse_args()

    passwd = args.passwd or getpass.getpass(
        f"Password for {args.user}@{args.host}: "
    )

    kvm = PiKvm(args.host, args.user, passwd)

    try:
        preflight(kvm)
        probe(kvm, args.delay)
    except requests.ConnectionError as e:
        sys.exit(f"Connection failed: {e}")
    except requests.HTTPError as e:
        sys.exit(f"HTTP error: {e}")


if __name__ == "__main__":
    main()
