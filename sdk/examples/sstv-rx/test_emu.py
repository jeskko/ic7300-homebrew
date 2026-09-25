#!/usr/bin/env python3
r"""Emulator test for the SSTV receiver: loader ABI v4 audio hook + the app, end to end.

Builds the loader firmware, a flash image, SSTV.BIN, the host decoder and an SD card holding
\homebrew\SSTV.BIN. Then it boots the emulator, opens MENU > SET > SD Card > Homebrew Apps,
starts SSTV and checks the audio hook is live. It plays an SSTV recording into the fake DSP's
RX audio (DX_REC L, `qom-set /machine/ssif af-file`), waits for the app to finish the image,
and compares the image in guest RAM with host_test's decode of the same file. Last, EXIT
should unload the app and clear the hook.

    python3 sdk/examples/sstv-rx/test_emu.py [--sample scratch/samples/SSTV.test.au]

Exits non-zero on the first failed check; screenshots and images go to --shots.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "sdk" / "loader"))
import test_emu as lt  # noqa: E402  (the loader test's Emu/nm/check helpers)

SCRATCH = REPO / "scratch" / "homebrew" / "sstv"
ST_NAMES = {0: "LISTENING", 1: "RECEIVING", 2: "DONE", 3: "LOST"}


def sh(*cmd, **kw):
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def build(sample: Path) -> tuple[Path, Path, Path]:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    sh(sys.executable, REPO / "sdk/loader/build.py")
    flash = SCRATCH / "flash.bin"
    sh(REPO / "emu/.venv/bin/python3", REPO / "qemu-machine/tools/build_flash.py",
       REPO / "scratch/hb_loader_142.dat", flash, stdout=subprocess.DEVNULL)
    app = SCRATCH / "SSTV.BIN"
    sh(sys.executable, REPO / "sdk/tools/build_app.py", "--keep", HERE / "build/app", "-o", app,
       HERE / "main.c", HERE / "sstv_core.c")
    card = SCRATCH / "sdcard.img"
    sh(sys.executable, REPO / "qemu-machine/tools/build_sdcard.py", "-o", card, "--size-mb", "128",
       stdout=subprocess.DEVNULL)
    sh("mmd", "-i", f"{card}@@1M", "::homebrew")
    sh("mcopy", "-i", f"{card}@@1M", app, "::homebrew/")
    (HERE / "build").mkdir(exist_ok=True)
    host = HERE / "build/host_test"
    sh("cc", "-O2", "-Wall", "-o", host, HERE / "host_test.c", HERE / "sstv_core.c")
    ref = SCRATCH / "host.ppm"
    sh(host, sample, ref, stdout=subprocess.DEVNULL)
    return flash, card, ref


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sample", type=Path, default=REPO / "scratch/samples/SSTV.test.au",
                    help="12 kHz mono 16-bit .au/.wav (host_test needs 12 kHz)")
    ap.add_argument("--shots", type=Path, default=SCRATCH / "shots")
    ap.add_argument("--boot-wait", type=float, default=30.0)
    ap.add_argument("--timeout", type=float, default=600.0, help="wall s for the whole image")
    ap.add_argument("--icount", default="shift=1",
                    help="-icount SPEC, or 'off'. Without it the guest can fall behind the "
                         "virtual clock and the tick ISR misses audio DMA buffers (~19%% of "
                         "blocks lost: 9.7 k instead of 12 k samples per emulated second)")
    ap.add_argument("--continuity", action="store_true",
                    help="mid-image, check the app's audio ring against the file "
                         "(audio_continuity.py) and print the result")
    ap.add_argument("--noise", type=Path,
                    help="instead of decoding: play this (non-repeating) file and check the "
                         "app's audio ring for gaps (audio_continuity.py)")
    ap.add_argument("--log", type=Path, help="QEMU stderr (device debug, RZA1H_DEBUG) to this file")
    args = ap.parse_args()
    args.shots.mkdir(parents=True, exist_ok=True)
    sample = args.sample.resolve()

    flash, card, ref_ppm = build(sample)
    loader = lt.nm(REPO / "sdk/loader/build/loader.elf")
    app = lt.nm(HERE / "build/app/app.elf")
    api = loader["g_api"]

    for p in (lt.QMP, lt.FPS):
        Path(p).unlink(missing_ok=True)
    env = dict(os.environ, RZA1H_AF_TONE="none")
    proc = subprocess.Popen([
        str(lt.QEMU), "-M", "rz-a1h", "-display", "none", "-monitor", "none", "-serial", "none",
        "-kernel", str(flash),
        "-global", f"rza1h-riic.image={REPO / 'qemu-machine' / 'riic2_eeprom.img'}",
        "-qmp", f"unix:{lt.QMP},server,nowait",
        "-chardev", f"socket,id=fpctl,path={lt.FPS},server=on,wait=off",
        "-drive", f"if=sd,format=raw,file={card}",
    ] + ([] if args.icount == "off" else ["-icount", args.icount]), env=env, stdin=subprocess.DEVNULL,
        stderr=open(args.log, "w") if args.log else subprocess.DEVNULL)
    check = lt.check
    try:
        time.sleep(args.boot_wait)
        emu = lt.Emu(args.shots)
        u32 = lambda sym: emu.word(app[sym])

        emu.press("MENU")
        emu.touch(430, 180)                     # SET
        emu.touch(448, 170)                     # page 2/2
        emu.touch(150, 117)                     # SD Card
        emu.touch(448, 170, 1.0)                # page 2/3
        emu.touch(448, 170)                     # page 3/3
        emu.touch(150, 55, 3.0)                 # Homebrew Apps
        check(lt.picker_labels(emu) == [b"SSTV"], "picker lists SSTV")
        print("     launching the app")
        emu.touch(150, lt.ROW_Y[0], 1.0)
        t0 = time.time()
        while emu.word(api + 8) == 0 and time.time() - t0 < 90:     # SD read, slower under icount
            time.sleep(1)
        print(f"     app resident {time.time() - t0:.0f} s after the tap")
        time.sleep(2)
        emu.shot("1-listening")

        check(emu.word(api + 8) != 0, "app resident (idle hook set)")
        check(emu.word(api + 16) == app["audio_isr"], "audio hook installed (ABI v4)")
        s0 = u32("g_samples")
        time.sleep(3)
        s1 = u32("g_samples")
        check(s1 > s0, f"audio flowing: {s1 - s0} samples in 3 s wall")
        def paused_sample():            # both counters at one guest instant (one QMP client)
            q = lt.qmp_open(lt.QMP)
            rd = lambda a: int(lt.qmp_cmd(q, "human-monitor-command",
                                          **{"command-line": f"xp /1wx {a:#x}"})["return"]
                               .split()[-1], 16)
            lt.qmp_cmd(q, "stop")
            v = (rd(app["g_w"]), rd(0xFCFEC004), rd(app["g_gaps"]))   # produced; OSTM0 CNT
            lt.qmp_cmd(q, "cont")
            q.close()
            return v
        c0, e0, g0 = paused_sample()
        time.sleep(8)
        c1, e1, g1 = paused_sample()
        secs = ((e1 - e0) & 0xffffffff) / 32e6
        rate = (c1 - c0) / secs
        check(abs(rate - 12000) < 60, f"  ...at {rate:.0f} samples per emulated second (12000), "
              f"{g1 - g0} lost blocks filled in {secs:.1f} s")
        check(u32("g_status") == 0, "listening")

        if args.noise:          # audio-path check only: play noise, check the ring, stop
            s = lt.qmp_open(lt.QMP)
            lt.qmp_cmd(s, "qom-set", path="/machine/ssif", property="af-file",
                       value=f"{args.noise.resolve()},loop")
            s.close()
            time.sleep(20)
            subprocess.run([sys.executable, str(HERE / "audio_continuity.py"), lt.QMP,
                            str(HERE / "build/app/app.elf"), str(args.noise.resolve())])
            return

        s = lt.qmp_open(lt.QMP)
        r = lt.qmp_cmd(s, "qom-set", path="/machine/ssif", property="af-file", value=str(sample))
        s.close()
        check("error" not in r, f"playing {sample.name} into DX_REC L")

        t0, last, mid_done = time.time(), None, False
        while time.time() - t0 < args.timeout:
            time.sleep(5)
            st, lines = u32("g_status"), u32("g_lines")
            if (st, lines // 32) != last:
                last = (st, lines // 32)
                print(f"     {time.time() - t0:5.0f} s  {ST_NAMES.get(st, st)}  line {lines}  "
                      f"samples {u32('g_samples')}")
            if st == 1 and lines >= 100 and not mid_done:
                mid_done = True
                emu.shot("2-receiving")
                if args.continuity:
                    subprocess.run([sys.executable, str(HERE / "audio_continuity.py"), lt.QMP,
                                    str(HERE / "build/app/app.elf"), str(sample)])
            if st in (2, 3):
                break
        check(u32("g_status") == 2, "image done")
        check(u32("g_vis") == 56, f"VIS {u32('g_vis')} (Scottie 2 = 56)")
        check(u32("g_lines") == 256, "256 lines")
        emu.shot("3-done")

        dump = args.shots / "image.bin"
        s = lt.qmp_open(lt.QMP)
        lt.qmp_cmd(s, "pmemsave", val=app["g_image"], size=256 * 320 * 2, filename=str(dump))
        s.close()
        px = np.frombuffer(dump.read_bytes(), "<u2").reshape(256, 320).astype(np.uint32)
        img = np.stack([(px >> 8) & 0xf8, (px >> 3) & 0xfc, (px << 3) & 0xf8], -1).astype(np.uint8)
        Image.fromarray(img).save(args.shots / "image.png")
        ref = np.asarray(Image.open(ref_ppm).convert("RGB")).astype(float)
        mse = ((img.astype(float) - ref) ** 2).mean()
        psnr = 10 * np.log10(255 ** 2 / mse) if mse else 99.0
        check(psnr > 20, f"matches host_test's decode of the file: PSNR {psnr:.1f} dB")

        emu.press("EXIT", 2.0)
        check(emu.word(api + 8) == 0, "EXIT: app gone")
        check(emu.word(api + 16) == 0, "  ...and the audio hook cleared")
        emu.shot("4-after-exit")
        check(proc.poll() is None, "emulator still running")
        print(f"all checks passed; screenshots in {args.shots}")
    finally:
        proc.terminate()
        proc.wait()


if __name__ == "__main__":
    main()
