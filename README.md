# Sndio PulseAudio module

A module for PulseAudio to support playing to sndio servers.

This is liberated from the OpenBSD PulseAudio port:
http://cvsweb.openbsd.org/cgi-bin/cvsweb/ports/audio/pulseaudio/files/

This module supports playback only.

## Setup

Load it via `/etc/pulse/default.pa` (Linux) or `/usr/local/etc/pulse/default.pa` (FreeBSD):
```
load-module module-sndio device=snd@thor/0
```

It's best to disable the `suspend-on-idle` module, so comment or remove the following line from `default.pa`:
```
load-module module-suspend-on-idle
```

Also set the default sink if you want:
```
set-default-sink sndio-sink
```
