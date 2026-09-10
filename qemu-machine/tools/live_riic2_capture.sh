#!/bin/bash
# Live RIIC2 (EEPROM SCL/SDA) bus-timing capture, using a real Openbench Logic Sniffer against
# the actual IC-7300 hardware -- built 2026-09-10, see README.md's Status section for the full
# derivation and bench-test results behind these specific parameters.
#
# GOAL: catch a real, representative slice of the dense ~6700-byte "combined settings struct"
# EEPROM read (FUN_2006cb84, this project's own long-tracked ring-overflow-adjacent scan) on real
# silicon, to directly cross-check the ~340kHz / ~26.4us-per-byte RIIC2 bit-rate formula this whole
# project's timing model depends on -- derived from decompiled register values, never directly
# observed on real hardware until now.
#
# TIMING BASIS (fresh, current-build measurement, tools/trace_eeprom_addr_gdbfree.py 20): the dense
# sequential 0x20-byte-chunk scan starts at real elapsed t~4.25-4.3s (since QEMU/boot start under
# -icount shift=auto, itself calibrated to real per-byte bus timing) and runs to ~t=5.0s -- about
# 0.7-0.8s total duration. This is our best available estimate for real hardware, not a guarantee:
# real power-on sequencing (voltage rail ramp, reset controller behavior) isn't modeled at all in
# this emulation, so real timing could differ by however much that adds. EXPECT TO ITERATE across
# a few power cycles, adjusting --delay, rather than expecting one perfect shot.
#
# SAMPLE-RATE/WINDOW TRADEOFF (bench-tested empirically on the actual OLS, not assumed): the
# device's single-acquisition sample-count ceiling is a FIXED ~1,572,864 samples regardless of
# configured rate (confirmed: identical cutoff at 100kHz through 2MHz) -- so real-time window
# covered = ceiling / rate. Chose 1MHz (yielding a ~1.57s window) over higher rates specifically to
# maximize the real-time margin around the timing estimate's own uncertainty, accepting a more
# modest ~1.5x oversampling of the real bus's ~340kHz signal -- adequate for confirming the
# bus frequency/per-byte timing (this project's actual question), not for laboratory-grade edge
# jitter measurement. Bump SAMPLE_RATE up (at the cost of window size) for a second pass once the
# scan's real onset is empirically pinned down from a first successful capture.
#
# HOOKUP (see notes/ic7300-signal-chain.md, notes/ic7300-hardware.md): IC351 (GT24C128B EEPROM),
# SCL/SDA labeled ECK/EDT on the schematic (CPU pins P1_4/P1_5) -- probe directly at IC351's own
# SOIC pins (or its pull-up resistors, if more accessible), NOT the CPU package (a BGA, not
# practically probeable). Channel assignment below is arbitrary but must match how you actually
# clip the two probes -- SCL(ECK) to channel 0, SDA(EDT) to channel 1, common GND to the radio's
# own ground.
#
# Usage: live_riic2_capture.sh [delay_seconds] [output_prefix]
#   delay_seconds: how long after applying power to START the capture (default 3.5s -- centers
#                  the ~1.57s window on the estimated ~4.25-5.0s scan, with margin both sides).
#   output_prefix: base name for the saved .sr session file (default: riic2_capture)
#
# Procedure: run this command FIRST (it starts counting from when you press Enter), THEN
# immediately apply power to the radio. The script sleeps for delay_seconds, then arms the OLS.

set -euo pipefail

DELAY="${1:-3.5}"
PREFIX="${2:-riic2_capture}"
CONN="/dev/ttyACM1"          # confirmed this session -- check `ls /dev/serial/by-id/` if it moves
SAMPLE_RATE=1000000          # 1MHz -- see file comment above for the window/resolution tradeoff
MAX_SAMPLES=1572864          # the confirmed hard ceiling -- requesting more just gets truncated
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUTFILE="${PREFIX}_${TIMESTAMP}_delay${DELAY}s.sr"

echo "Apply power to the radio NOW. Arming capture in ${DELAY}s..."
sleep "$DELAY"

echo "Arming: ${SAMPLE_RATE} Hz, RLE on, ${MAX_SAMPLES} samples (~$(python3 -c "print(f'{$MAX_SAMPLES/$SAMPLE_RATE:.2f}')")s window)"
sigrok-cli --driver=ols:conn="$CONN" --channels 0,1 \
    --config "samplerate=${SAMPLE_RATE}:rle=on" \
    --samples "$MAX_SAMPLES" \
    -o "$OUTFILE"

echo "Saved: $OUTFILE"
echo ""
echo "Quick check for real I2C activity (looks for any decoded start conditions):"
sigrok-cli -i "$OUTFILE" -P i2c:scl=0:sda=1 -A i2c=start 2>&1 | head -5 || true
echo ""
echo "If that showed nothing, the capture window likely missed the scan -- try again with a"
echo "different --delay (e.g. $(python3 -c "print(f'{$DELAY-1:.1f}')") or $(python3 -c "print(f'{$DELAY+1:.1f}')"))."
echo "If it did, run tools/analyze_riic2_capture.py $OUTFILE for full per-byte timing analysis."
