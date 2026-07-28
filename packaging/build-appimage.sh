#!/usr/bin/env bash
# Builds a Sable AppImage (D-106).
#
# AppImage first because Sable's dependency list is short — SDL3 and libc are
# essentially it, since ImGui, lodepng, miniz and nlohmann/json are compiled in.
# That makes a single self-contained file cheap to produce, which is not true
# of an application that drags a desktop toolkit behind it.
#
# Usage:  packaging/build-appimage.sh [build-dir]
# Output: Sable-x86_64.AppImage in the repository root.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build-appimage}"
appdir="$build/AppDir"
arch="$(uname -m)"

cd "$root"

echo "==> Configuring a release build"
cmake -S . -B "$build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DSABLE_BUILD_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX=/usr

echo "==> Building"
cmake --build "$build"

echo "==> Installing into AppDir"
rm -rf "$appdir"
DESTDIR="$appdir" cmake --install "$build"

# AppImage expects the icon and desktop file at the AppDir root as well as in
# the usual share/ locations. Symlinks rather than copies, so there is one
# source of truth for each.
ln -sf usr/share/applications/sable.desktop "$appdir/sable.desktop"
ln -sf usr/share/icons/hicolor/256x256/apps/sable.png "$appdir/sable.png"
ln -sf usr/share/icons/hicolor/256x256/apps/sable.png "$appdir/.DirIcon"

cat > "$appdir/AppRun" <<'EOF'
#!/usr/bin/env bash
# Prefer the host's SDL3 when it is present and new enough: the bundled copy
# cannot know about the host's graphics drivers, and a pen tablet that works
# natively must not stop working inside an AppImage.
here="$(dirname "$(readlink -f "${0}")")"
export LD_LIBRARY_PATH="${here}/usr/lib:${LD_LIBRARY_PATH:-}"
exec "${here}/usr/bin/sable" "$@"
EOF
chmod +x "$appdir/AppRun"

# Bundle SDL3 itself. Everything else Sable needs is either compiled in or is
# part of any Linux system old enough to matter.
mkdir -p "$appdir/usr/lib"
sdl="$(ldd "$appdir/usr/bin/sable" | awk '/libSDL3/ {print $3}')"
if [ -n "${sdl:-}" ] && [ -f "$sdl" ]; then
    cp -L "$sdl" "$appdir/usr/lib/"
    echo "    bundled $(basename "$sdl")"
else
    echo "    WARNING: libSDL3 not found; the AppImage will need it on the host" >&2
fi

tool="$build/appimagetool-$arch.AppImage"
if [ ! -x "$tool" ]; then
    echo "==> Fetching appimagetool"
    url="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$arch.AppImage"
    if ! curl -fsSL -o "$tool" "$url"; then
        echo "Could not download appimagetool. The AppDir is complete at:" >&2
        echo "  $appdir" >&2
        echo "Run appimagetool against it by hand to finish." >&2
        exit 1
    fi
    chmod +x "$tool"
fi

echo "==> Packing"
# --appimage-extract-and-run avoids needing FUSE, which is missing in most
# containers and CI runners.
ARCH="$arch" "$tool" --appimage-extract-and-run "$appdir" "$root/Sable-$arch.AppImage"

echo "==> Done: $root/Sable-$arch.AppImage"
