# Changelog

## Unreleased
- Fill the selected label with the highlight color and pick black or white text for it, unless `[label]` sets colors.

## 0.1.0
- Add a fullscreen overlay with a thumbnail of every window on every output, most recently used first.
- Add `--daemon`, `--toggle`, `--show`, `--switch`, `--activate` and `--quit`.
- Add switch mode: `--next` and `--prev` select a window, and releasing the modifiers picks it.
- Add `--show --multi-select`, which prints the identifiers of the toggled windows.
- Filter tiles by typing, and with `--app-id`, `--title` and `--status`.
- Hide windows for good with `[hide]` rules.
- Close and minimize windows from the overlay.
- Show app icons on the tiles; place them with `icons.position`.
- Show a label on each tile; set its content, position, box, colors and font in `[label]`.
- Round the window previews with `tiles.radius`.
- Fade and grow the overlay when it opens.
- Show a background image (PNG, JPEG, AVIF, HEIC or HEIF) with `background.image`, or take its path from a command with `background.image_cmd`.
- Keep panels and docks visible with `background.panels = show`.
- Set the colors, or read them from a labwc theme file with `colors.theme_file`.
- Bind keys to actions in `[bind]`.
- Show every window on each monitor, or all of them on the monitor under the pointer (`monitors.mode`).
- Read the config from `~/.config/skippy-wl/skippy-wl.rc`.
- Add a systemd user unit.
- Add a man page.
- Add `install.sh` to install and uninstall.
