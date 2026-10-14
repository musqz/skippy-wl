#!/usr/bin/env bash
# Install skippy-wl. PREFIX (default ~/.local) and SUDO override the target.
set -euo pipefail

PREFIX="${PREFIX:-$HOME/.local}"
read -ra SUDO_CMD <<< "${SUDO:-}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${BUILD_DIR:-$SRC/build}"

FILES=(
	"$PREFIX/bin/skippy-wl"
	"$PREFIX/share/man/man1/skippy-wl.1"
	"$PREFIX/share/skippy-wl/skippy-wl.conf"
	"$PREFIX/share/systemd/user/skippy-wl.service"
)

uninstall() {
	for f in "${FILES[@]}"; do
		if [[ -e "$f" ]]; then
			"${SUDO_CMD[@]}" rm -f -- "$f"
			echo "removed $f"
		fi
	done
	if [[ -d "$PREFIX/share/skippy-wl" ]]; then
		"${SUDO_CMD[@]}" rmdir --ignore-fail-on-non-empty -- "$PREFIX/share/skippy-wl"
	fi
}

case "${1:-}" in
	--uninstall)
		uninstall
		exit 0
		;;
	"") ;;
	*)
		echo "usage: $0 [--uninstall]" >&2
		exit 2
		;;
esac

for tool in meson ninja; do
	command -v "$tool" >/dev/null || { echo "missing build tool: $tool" >&2; exit 1; }
done

meson setup --reconfigure "$BUILD" "$SRC" --prefix "$PREFIX" --buildtype release
ninja -C "$BUILD"
"${SUDO_CMD[@]}" meson install -C "$BUILD" --quiet

for f in "${FILES[@]}"; do
	[[ -e "$f" ]] || { echo "not installed: $f" >&2; exit 1; }
	echo "installed $f"
done

"$PREFIX/bin/skippy-wl" --version
if ! command -v skippy-wl >/dev/null; then
	echo "note: $PREFIX/bin is not in PATH" >&2
fi
