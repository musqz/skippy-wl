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

Other commands: `--show`, `--quit`, `--version`. `man skippy-wl` lists the overlay keys.

Filter the windows with `--app-id`, `--title` and `--status` (`minimized`, `maximized`, `fullscreen`, `activated`). Lists are comma separated globs; a leading `!` excludes, `\,` is a literal comma:

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

Or switch like Alt-Tab: hold the modifier, tap Tab to cycle, release to pick.

    <keybind key="W-Tab">
      <action name="Execute" command="skippy-wl --show --next"/>
    </keybind>
    <keybind key="W-S-Tab">
      <action name="Execute" command="skippy-wl --show --prev"/>
    </keybind>

## Config
Copy the template and edit it:

    mkdir -p ~/.config/skippy-wl
    cp "${PREFIX:-$HOME/.local}/share/skippy-wl/skippy-wl.conf" ~/.config/skippy-wl/config

To follow the Noctalia colors, set `theme_file = ~/.local/share/themes/noctalia/openbox-3/themerc` under `[colors]`.

Sections: `[layout]` (`margin`, `spacing`, `label_height`, `label`, `font`), `[monitors]` (`mode`), `[switch]` (`pivot`, `wait`), `[icons]` (`enabled`, `size`, `theme`), `[colors]` (`background`, `highlight`, `border`, `theme_file`), `[bind]` (one key per action). Changes apply the next time the overlay opens.

## Development
    meson setup build && ninja -C build && meson test -C build

`build/skippy-wl-probe` prints the window lists of the compositor. `skippy-wl --capture-all <dir>` saves a PNG per window.
