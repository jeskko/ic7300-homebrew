# sstv-rx (planned)

App 4, the SSTV receiver. Not built yet; design and research: [`../../sstv-app-design.md`](../../sstv-app-design.md).

`prototype/sstv_proto.py IN OUT.png` is the host prototype of the planned decoder: a quadrature
FM discriminator at 12 kHz, VIS decode, and per-line sync lock, Scottie family only.
`prototype/scottie2-testcard.png` is its decode of `scratch/samples/SSTV.test.au`, and
`scottie2-testcard-slowrx-cli.png` is slowrx-cli's decode of the same file, for comparison.
