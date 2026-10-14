# skippy-wl
Expose tool like skippy-xd but for wayland

# Unstable

## Requirements
- labwc 0.20 or newer (wlroots 0.20). Other compositors are untested.
- Protocols: ext-foreign-toplevel-list, ext-image-capture-source, ext-image-copy-capture, wlr-foreign-toplevel-management, wlr-layer-shell.
- Build: meson, ninja, wayland, cairo, pango, librsvg, libxkbcommon.

## Install
Run `./install.sh`. It installs to `~/.local`; set `PREFIX=/usr SUDO=sudo` for a system install.
Remove with `./install.sh --uninstall`.

## Usage
Start the daemon, then toggle the overlay:

    skippy-wl --daemon &
    skippy-wl --toggle

Other commands: `--show`, `--switch` (focus the previous window, no overlay), `--activate <identifier>` (focus that window, e.g. one printed by `--multi-select`), `--quit`, `--version`. `man skippy-wl` lists the overlay keys.

Type `/` (or any unbound key) to filter the tiles by app id or title. Escape clears the filter, a second Escape closes the overlay.

Pick several windows with `skippy-wl --show --multi-select`. It prints their identifiers, one per line.

Click or press Space to toggle a window, Enter to confirm, Escape to cancel (exit status 1, no output).

Note: it runs without the daemon and does not focus, close or minimize anything.

Focus one window from a pick: `skippy-wl --activate "$(skippy-wl --show --multi-select | head -n1)"`.

Filter the windows (also for `--switch`) with `--app-id`, `--title` and `--status` (`minimized`, `maximized`, `fullscreen`, `activated`). Lists are comma separated globs; a leading `!` excludes, `\,` is a literal comma:

    skippy-wl --toggle --app-id kitty,foot
    skippy-wl --toggle --title '*vim*'
    skippy-wl --toggle --app-id '!firefox' --status '!minimized'
 `man skippy-wl` lists the overlay keys.

## labwc
Add to `~/.config/labwc/autostart`:

    skippy-wl --daemon &

Add to `~/.config/labwc/rc.xml`, inside `<keyboard>`:

    <keybind key="A-Tab">
      <action name="Execute" command="skippy-wl --toggle"/>
    </keybind>

Or switch like Alt-Tab: hold the modifier, tap Tab to cycle through the windows, release to pick. Pause for a second before releasing to keep the overlay open (`hold`). `W-S-Tab` steps back.

    <keybind key="W-Tab">
      <action name="Execute" command="skippy-wl --show --next"/>
    </keybind>
    <keybind key="W-S-Tab">
      <action name="Execute" command="skippy-wl --show --prev"/>
    </keybind>

### systemd
Instead of the `skippy-wl --daemon &` autostart line (remove it), run the daemon as a user service. Add to `~/.config/labwc/autostart`:

    systemctl --user import-environment WAYLAND_DISPLAY
    systemctl --user start skippy-wl.service

After an install or upgrade: `systemctl --user daemon-reload && systemctl --user restart skippy-wl.service`. Before `./install.sh --uninstall`: `systemctl --user stop skippy-wl.service`.

Logs: `journalctl --user -u skippy-wl`.

## Config
Copy the template and edit it:

    mkdir -p ~/.config/skippy-wl
    cp "${PREFIX:-$HOME/.local}/share/skippy-wl/skippy-wl.conf" ~/.config/skippy-wl/config

To follow the Noctalia colors, set `theme_file = ~/.local/share/themes/noctalia/openbox-3/themerc` under `[colors]`.

Sections: `[layout]` (`margin`, `spacing`, `label_height`, `label`, `font`), `[monitors]` (`mode`), `[switch]` (`pivot`, `wait`, `hold`), `[icons]` (`enabled`, `size`, `theme`), `[animation]` (`enabled`, `duration`), `[background]` (`image`), `[colors]` (`background`, `highlight`, `border`, `theme_file`), `[bind]` (one key per action). Changes apply the next time the overlay opens.

## Development
    meson setup build && ninja -C build && meson test -C build

`build/skippy-wl-probe` prints the window lists of the compositor. `skippy-wl --capture-all <dir>` saves a PNG per window.
