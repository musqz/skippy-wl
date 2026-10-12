# Capture notes

Measured on nested labwc 0.20.2 (x11 backend, software renderer), kitty windows of 1022x681, all captures started at once (`skippy-wl --capture-all <dir>`):

| windows | all frames ready |
|---|---|
| 2 | 22 ms |
| 6 | 62 ms |
| 11 | 64-150 ms |
| 21 | 227 ms |

Numbers vary run to run; capture cost is roughly 10 ms per window.

- Occluded and minimized windows return real content, not blank frames.
- The compositor offers only `XBGR8888` over shm; `capture.c` swaps R/B in place after `ready`. Native `ARGB8888`/`XRGB8888` is preferred when offered.
- `shm_format` carries DRM fourccs on wlroots, not `wl_shm` enum values.
- Windows on another workspace: untested.
