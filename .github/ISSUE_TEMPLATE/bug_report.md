---
name: Bug report
about: Report something Glasshouse does wrong
title: ''
labels: bug
assignees: ''
---

## Summary
<!-- One sentence: what should happen vs what does. -->

## Environment

- Glasshouse version: <!-- `dpkg -l | grep glasshouse` for the .deb, or the build sha if running from source -->
- Distro and Qt version: <!-- e.g. Kubuntu 24.04 / Qt 6.4.2 -->
- Display server: <!-- X11 / Wayland -->
- PiKVM model(s) and kvmd version(s): <!-- per affected PiKVM, e.g. PiKVM 4 Plus / kvmd 3.199 -->
- Transport: <!-- mjpeg / janus -->

## Steps to reproduce

1.
2.
3.

## Expected behaviour

## Actual behaviour

## Logs

<!--
Re-run with verbose categories enabled and paste the relevant section
of the log here. Redact any IPs or hostnames you don't want public.

    QT_LOGGING_RULES='glasshouse.*=true' \
        glasshouse-viewer --log-file /tmp/glasshouse.log
-->

```
(paste log excerpt here)
```
