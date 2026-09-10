#!/bin/bash
# Applies (idempotently) the TEMPORARY IRQ-mask-transition diagnostic patch
# (patches/irq-mask-trace.patch) to the vendored qemu-src/ checkout, then
# rebuilds qemu-system-arm.
#
# Deliberately NOT wired into setup.sh: unlike hw-arm-build.patch (a small,
# permanent registration patch this machine always needs), this one patches
# core target/arm/helper.c purely for one investigation (what holds CPSR.I=1
# for ~10s during the job-ring-overflow boot stall -- see README.md's Status
# section) and rza1h_debug.h's own header comment explains why this project
# avoids a *permanent* second core-QEMU patch (rebasing pain across QEMU
# version bumps). Apply this manually only while that investigation is live;
# drop it (git apply --reverse) once the resume point is resolved.
#
# Usage: qemu-machine/tools/apply_irq_mask_trace.sh
# Then:  RZA1H_IRQ_TRACE=1 <qemu invocation> 2>irqtrace.log
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_SRC="$HERE/qemu-src"

if [ ! -d "$QEMU_SRC" ]; then
    echo "qemu-src/ doesn't exist yet -- run setup.sh first." >&2
    exit 1
fi

echo "Applying IRQ-mask-trace patch..."
if git -C "$QEMU_SRC" apply --reverse --check "$HERE/patches/irq-mask-trace.patch" 2>/dev/null; then
    echo "  (already applied)"
else
    git -C "$QEMU_SRC" apply "$HERE/patches/irq-mask-trace.patch"
fi

echo "Rebuilding qemu-system-arm..."
ninja -C "$QEMU_SRC/build" qemu-system-arm

echo "Done: $QEMU_SRC/build/qemu-system-arm (set RZA1H_IRQ_TRACE=1 to enable logging)"
