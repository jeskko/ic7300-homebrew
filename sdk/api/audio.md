# Audio

The hardware link is real and pin-confirmed; nothing here yet reaches a specific sample-buffer API an app
could call. This is App 4 (SSTV)'s single biggest open question in `app-requirements.md`, and would also
matter for any general "record/play audio" SDK primitive.

## ✅ Hardware link: SSIF0 + SSIF1, paired

`BCLK_`/`FRM_`/`DX_REC`/`DR_AF` (`P2_8`-`P2_11`) and `DX_FMT`/`DR_RSV` (`P3_6`/`P3_7`) form a confirmed,
actively-used CPU↔DSP digital audio link via the RZ/A1H's Serial Sound Interface — SSIF0 (`0xE820B000`)
and SSIF1 (`0xE820B800`) paired, sharing SSIF0's clock/word-select (standard RZ/A1H pairing mode). A
dedicated setup table around `0x20060700`-`0x20060770` references both SSIF register blocks plus paired
DMAC channel addresses — a genuine DMA-driven continuous audio/IQ sample stream between the main CPU and
the DSP, not a one-time handshake (`notes/ic7300-signal-chain.md`). DSP-side pin data independently
confirms the same structure from the other end (McASP0's shared clock/frame-sync + 4 serializer pins).

## 🔎 Open: where do live RX-demodulated audio samples actually live?

**The single biggest open question for any audio-consuming app.** The best existing lead is
`voice_recording_file_task` (`0x2001745c`, verified) — it streams *some* audio to SD card via a 4-slot ring
buffer using `file_rpc_post_command` (see [`filesystem.md`](filesystem.md)), but its own audio **source**
(which SSIF channel, what sample rate/bit depth, RX-demodulated audio vs. the mic/TX path) was never traced
back to its producer — only the file-I/O side is understood. The CI-V manual's `1A 05 01 82`-family "QSO
recorder"/audio-source-select commands confirm a real, configurable "which audio to record" feature exists
at the protocol level, meaning a single shared audio-sample subsystem plausibly exists — a good next place
to look for a tappable RX-audio buffer.

**A same-shaped gap found chasing the RTTY decoder (2026-08-30, `notes/kernel-rtos-history.md`)**: RTTY
decode-to-SD-card logging (the closest already-working digital-mode feature) has the identical blind spot
— traced its file-write path all the way down to a shared struct with a "pending decoded record" field,
but who actually writes the *decoded characters* into that struct wasn't found, and confirmed that cluster
never touches the `SCIF5` DSP-link register directly either. Doesn't resolve this question, but confirms
it's a recurring structural boundary in this project's tracing so far (feature-level code is fully
understood; whatever produces the real content — demodulated audio or decoded text — consistently isn't
yet found for *any* traced feature). Worth remembering if either thread is picked back up: progress on one
likely generalizes to the other.

**Best lead yet, 2026-09-07 (`notes/kernel-rtos-history.md`'s "Picking the RTTY/SSTV thread back up")**:
tracing RTTY's mode-entry path (not its file-write path, a different angle than the above) surfaced
`operating_mode_change_dispatch` (`0x2005807c`) and a real per-operating-mode "on enter this mode"
function-pointer table (`0x2019ac0c`) — most entries are plain no-op `bx lr`, but the entry very plausibly
corresponding to RTTY (and CW-R) points at `0x20056fd4`, an address that turns out to sit in a completely
undissassembled code region and hits the project's known ARM/Thumb bug. Not yet readable, but this is a
much more targeted candidate for "where does mode-specific hardware/DSP setup happen" than anything found
chasing the file-I/O side — worth checking first, once the user applies the manual Thumb fix, before
assuming a live-JTAG session is required to find the real audio/decode-data source.

## 🔎 Open: real-time budget for a main-CPU-side decode task

Not yet assessed whether a task on the *main* CPU (as opposed to the DSP, which is already busy doing the
actual demodulation) has enough spare cycles to run something like continuous SSTV decode without falling
behind. Plausible given the RZ/A1H's clock speed and this being a fairly light DSP task by modern
standards, but not verified — worth a real check once the audio-tap question above is settled and a task
can actually be tested.

## Not needed: the decode algorithm itself

For an SSTV app specifically, the decode algorithm (sync-pulse detection, tone-to-luminance mapping, mode
timing for Robot36/Martin/Scottie) is standard, publicly documented ham-radio DSP technique, independent of
anything reverse-engineered here — see `app-requirements.md`'s App 4. Nothing to research on this project's
side beyond the audio-tap question above.
