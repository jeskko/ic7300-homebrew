#!/bin/bash
# Idempotent setup for the rz-a1h QEMU machine: clone QEMU at the pinned tag
# (if not already present), symlink our sources into it, apply the small
# Kconfig/meson.build patch, configure, and build just the arm-softmmu
# target. See qemu-machine/README.md for why this shape (a patch onto a
# gitignored vendored checkout, not a fork or a full multi-arch build).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QEMU_SRC="$HERE/qemu-src"
QEMU_TAG="v11.1.1"  # matches the pacman-installed qemu-system-arm exactly

if [ ! -d "$QEMU_SRC" ]; then
    echo "Cloning QEMU $QEMU_TAG (shallow)..."
    git clone --branch "$QEMU_TAG" --depth 1 https://gitlab.com/qemu-project/qemu.git "$QEMU_SRC"
fi

echo "Applying hw/arm build patch..."
if git -C "$QEMU_SRC" apply --reverse --check "$HERE/patches/hw-arm-build.patch" 2>/dev/null; then
    echo "  (already applied)"
else
    git -C "$QEMU_SRC" apply "$HERE/patches/hw-arm-build.patch"
fi

echo "Symlinking sources..."
for f in rz_a1h.c rz_a1h.h ostm.c; do
    ln -sf "../../../src/$f" "$QEMU_SRC/hw/arm/$f"
done

mkdir -p "$QEMU_SRC/build"
cd "$QEMU_SRC/build"
if [ ! -f build.ninja ]; then
    echo "Configuring (arm-softmmu only)..."
    ../configure --target-list=arm-softmmu
fi

echo "Building qemu-system-arm..."
ninja qemu-system-arm

echo "Done: $QEMU_SRC/build/qemu-system-arm"
