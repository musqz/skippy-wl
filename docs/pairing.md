# ext/wlr toplevel pairing

ext handles give thumbnails (`ext_image_capture_source`), wlr handles give activate/close/state. The two share no id.

## Findings (labwc 0.20.2, wlroots 0.20.2, nested x11 backend)

- `foreign_toplevel_create()` (`src/foreign-toplevel/foreign.c`) creates the wlr handle, then the ext handle, in one call per view. No other code creates handles.
- Live window open: `WLR#n new` is followed directly by `EXT#n new`; no event from another toplevel in between.
- Live close: `WLR closed`, then `EXT closed`.
- Fresh client bind: each list is sent in full, in the same order. Probe binds wlr first; a client binding ext first is untested.
- Order held after: closing a middle window then opening a new one (distinct titles), minimize, unminimize. For identical-title windows only the live event adjacency is evidence; the side-by-side table cannot tell them apart.
- `--act N` moves `act` to `wlr#N` (and clears it on the previously active one); `--min N` / `--unmin N` set `min` on `wlr#N`.

## Verified end to end (Phase 4)

- 4 windows with identical titles and different content: selecting each tile focuses the window with matching content.
- Activating a minimized window unminimizes and focuses it.
- Activating a window on another workspace switches to that workspace (labwc `desktop_focus_view()`), so `ext-workspace` is not used.
- With two outputs, each overlay lists only the windows the wlr handle reports on that output (`output_enter`).

## Not tested

- Capture of other-workspace windows on a real GPU renderer.
- Compositors other than labwc.

## Decision

Option (a): order-based pairing.

- Startup: pair `ext[i]` with `wlr[i]` from the initial lists.
- Live: after a wlr `toplevel` event, the next ext `toplevel` event is its partner.
- Check `app_id` and `title` on every pair; on mismatch drop the pair and rebuild both lists by rebinding. This cannot catch an index shift between identical windows.
- Pair is removed when either side sends `closed`.

Identical windows need the activate-and-check fallback (b) if ordering ever breaks. Phase 4 proves it end to end: activate each of N identical-title windows and check the right one gets focus.

## Probe

`skippy-wl-probe` prints both lists side by side. `--watch` logs events live until Ctrl-C. `--act|--min|--unmin N` acts on wlr index N.

Nested session: `WLR_BACKENDS=x11 labwc` from an X11 desktop, then `WAYLAND_DISPLAY=wayland-0 build/skippy-wl-probe`.
