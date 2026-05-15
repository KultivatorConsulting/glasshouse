# Contributing to Glasshouse

Thanks for considering a contribution. A few ground rules so patches
land smoothly.

## Build, run, test

See [README.md — Build from source](README.md#build-from-source) for
apt build dependencies, CMake configure/build/test, and how to run
the binaries from the build tree. Tests are QTest-based; `ctest
--test-dir build --output-on-failure` is a hard prerequisite for any
PR touching pure-function code (config validator, coord transform,
keymap).

## Where to start

- **Bug reports.** Open a [GitHub
  issue](https://github.com/kultivator-consulting/glasshouse/issues).
  Include your kvmd / ustreamer version (`kvmd --version` on the
  PiKVM), distro and Qt version on the client, and the failing flow's
  log if you can capture one:

      QT_LOGGING_RULES='glasshouse.*=true' \
          glasshouse-viewer --log-file /tmp/glasshouse.log

- **Feature ideas.** Open an issue first to talk through scope before
  sending code. Glasshouse is a focused desktop client; not every
  feature belongs.
- **Patches.** PRs welcome. Match the existing style: lowercase
  snake_case file names, Qt-style includes, terse comments only where
  the *why* isn't obvious from the code.

## Conventions

- **Empirical verifications matter.** When implementation contradicts
  the design (latency, API behaviour, Qt quirks, PiKVM firmware
  surprises), update `specs/DESIGN.md` §10 in the same commit that
  reveals it. The slots in §10 exist for exactly this.
- **Tests for pure logic.** Network-heavy behaviour (auth, WS, Janus)
  is exercised by the harnesses against real hardware. Pure-function
  code — config validation, coord transform, keymap — must have QTest
  coverage in `tests/`.
- **Don't modify the PiKVM.** Glasshouse runs against stock firmware;
  workarounds for firmware quirks live client-side. If you find
  yourself wanting to patch kvmd or ustreamer, find a client-side
  path instead.
- **One concern per commit.** Bug fixes, refactors, and doc updates
  should land separately. Keep the commit log scannable.

## Releases

Tag-driven via `.github/workflows/release.yml`. Push a `v<semver>`
annotated tag on `main`; the workflow builds and publishes the `.deb`
to the [Releases
page](https://github.com/kultivator-consulting/glasshouse/releases).
Untagged pushes to `main` produce a `0.0.0+sha.<short>` artifact via
`build.yml` (CI artifact only — not auto-installed).

When cutting a release, promote the `[Unreleased]` block in
[CHANGELOG.md](CHANGELOG.md) to `[<version>] - <YYYY-MM-DD>` and
update the footer compare links in the same commit.

## Licensing of contributions

By submitting a contribution you agree it can be distributed under
[LGPL-3.0-or-later](LICENSE), the project's license. There is no CLA.
