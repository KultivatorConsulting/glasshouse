# Security policy

## Supported versions

The latest tagged release on `main` is the supported version. Patch
releases supersede prior tags; older 0.1.x tags are not patched.

## Reporting a vulnerability

Please **do not** open a public GitHub issue for security-affecting
bugs. The areas of the code most likely to carry security impact are:

- TLS handling and authentication in `src/core/pikvmclient.cpp` —
  notably the `insecure_tls` opt-in, cookie/Bearer carry-over across
  reconnects, and TOTP composition.
- Secret resolution in `src/core/secrets.cpp` and the
  `~/.config/glasshouse/secrets.yaml` permissions check.
- Mass-storage upload paths in `src/video/msddialog.cpp` (file size /
  name handling crossing the kvmd boundary).
- Anything that lets a malicious or compromised PiKVM run code on the
  client beyond the documented input/video paths.

Email **developers@kultivator.co.nz** with reproduction steps and
the affected version (`glasshouse-viewer --version` or the installed
`.deb` version from `dpkg -l | grep glasshouse`). Expect an
acknowledgment within a week. If a fix is non-trivial we'll
coordinate disclosure timing over email.

## Out of scope

Glasshouse is a desktop client; it has no server component of its
own. Vulnerabilities in the PiKVM firmware itself (kvmd, ustreamer,
Janus) are out of scope here — please report those upstream at
<https://github.com/pikvm/>.
