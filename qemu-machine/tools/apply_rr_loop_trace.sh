#!/bin/bash
# Applies (idempotently) the TEMPORARY round-robin-main-loop diagnostic patch
# (patches/rr-loop-trace.patch) to the vendored qemu-src/ checkout, then
# rebuilds qemu-system-arm.
#
# Deliberately NOT wired into setup.sh: unlike hw-arm-build.patch (a small,
# permanent registration patch this machine always needs), this one patches
# core accel/tcg/tcg-accel-ops-rr.c purely for one investigation (where the
# real, measured ~65-90us-per-scheduled-device-event host wall-clock cost in
# this loop actually goes -- see qemu-machine/README.md's Status section)
# -- same precedent as irq-mask-trace.patch. Apply this manually only while
# that investigation is live; drop it (git apply --reverse) once resolved.
#
# Usage: qemu-machine/tools/apply_rr_loop_trace.sh
# Then:  RZA1H_RR_TRACE=1 RZA1H_DEBUG=riic <qemu invocation> 2>rrtrace.log
#        (combine with RZA1H_DEBUG=riic -- riic.c's own already-permanent
#        icount_get_raw()/host-timestamp instrumentation in
#        riic_schedule_irq_delay() is what this new trace needs to be
#        correlated against; see tools/trace_rr_loop_overhead.py)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_SRC="$HERE/qemu-src"

if [ ! -d "$QEMU_SRC" ]; then
    echo "qemu-src/ doesn't exist yet -- run setup.sh first." >&2
    exit 1
fi

echo "Applying round-robin-loop trace patch..."
if git -C "$QEMU_SRC" apply --reverse --check "$HERE/patches/rr-loop-trace.patch" 2>/dev/null; then
    echo "  (already applied)"
else
    git -C "$QEMU_SRC" apply "$HERE/patches/rr-loop-trace.patch"
fi

echo "Rebuilding qemu-system-arm..."
ninja -C "$QEMU_SRC/build" qemu-system-arm

echo "Done: $QEMU_SRC/build/qemu-system-arm (set RZA1H_RR_TRACE=1 to enable logging)"
