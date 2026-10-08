# tests/host/jpeg — host-side probe for the progressive-JPEG reduced-scale decoder

Runs the **real** `ProgressiveJpegScaled` (`components/crossmux/lib/JpegToBmpConverter/`)
on Linux x86-64 with plain `g++`, and compares its output **pixel by pixel** against
libjpeg's own reduced-scale decode (`djpeg -scale 1/N -grayscale`). No ESP-IDF, no CMake,
no device, no flashing.

It answers the one question you cannot answer on the device: **is the reduced IDCT
numerically right?** On the panel, "a bit soft" and "correctly folded but slightly
mis-scaled" look identical. Here they don't.

## Run it

```sh
tests/host/jpeg/run.sh                 # build + fixture.jpg, assert against thresholds
tests/host/jpeg/run.sh FILE.jpg [...]  # build + any material, print stats, no assertion
tests/host/jpeg/run.sh --bench         # also print host timings
tests/host/jpeg/run.sh --ref-stb       # use the stb reference instead (see below)
tests/host/jpeg/run.sh --dump DIR      # also write our PGM + the reference PGM per scale
```

Exit status is non-zero when a number leaves the thresholds. Nothing is written outside
`tests/host/jpeg/build/` (gitignored).

## What it checks, and why it can

The decoder outputs the **luma** plane at 1/2 or 1/4 (it folds only the Y component's
coefficients). So the right oracle is libjpeg's reduced decode of luma:
`djpeg -scale 1/N -grayscale` is *the same quantity*. Against it our output matches to
**max 2 levels** on every real cover we have — i.e. bit-exact modulo rounding. That single
fact pins down the fold kernel, the scan-sync/EOB accounting, and the Q16 scaling all at
once; any of them being wrong moves hundreds of thousands of pixels, not two.

`--ref-stb` instead builds the reference from a stb **full** decode → RGB → `77/151/28` →
`f×f` box average. That looks more "independent" but is **a different quantity**: the
RGB→grey round-trip loses a few levels at hard *chroma* edges. On `fixture.jpg` it reads
up to 12 levels off — and at those exact pixels we match `djpeg` to 1 while `stb` is the
one 11 off. So stb is only the no-`djpeg` fallback, and its numbers are pessimistic.

The driver needs `djpeg`/`libjpeg-turbo` on the box; without it `run.sh` prints a note and
falls back to `--ref-stb` rather than stopping.

## The bugs this exists to catch

Thresholds are `mean ≤ 1.0`, `>5 levels ≤ 0.10%`, and **zero leaked bytes** (the counting
allocator must be flat after the plane is released). Three real regressions, all re-created
during development and all caught decisively:

| broken variant | 1/2 mean | 1/2 `>2` | 1/2 leak | exit |
|---|---|---|---|---|
| **drop the folded terms** (keep only the u<S kernel prefix — an extra low-pass) | 2.97 | 21.19% | — | FAIL |
| **skip EOB runs** (`0b1rrr_0000` blocks occupy zero bits; not skipping desyncs the bitstream) | 7.18 | 50.65% | — | FAIL |
| **leak the fold accumulator** (no destructor releasing `acc`) | 0.21 | 0.00% | 1 920 000 B | FAIL |
| correct decoder | 0.13–0.29 | 0.00% | 0 B | ok |

The first two move pixels; the third is numerically *perfect* — only the leak check sees it.
It shipped once: each decode leaked the whole `acc` block (~1.9 MB at 1/2), so a few covers
exhausted PSRAM and the fit-check silently fell back to the blurry 1/8 path.

Note the truncation bug is *invisible* at 1/8 (S=1 folds to DC only, so dropping the fold
is a no-op there) — which is exactly why the probe runs 1/2, 1/4 **and** 1/8 and asserts on
each.

## Files

| file | what |
|---|---|
| `probe.cpp` | the driver: SOF parse, decode at 1/2·1/4·1/8, djpeg/stb reference, pixel diff, thresholds |
| `stb_host.cpp` | stb implementation unit with default `malloc`. **Separate from** the device's `StbImageImpl.cpp`, which is PSRAM-pinned (`heap_caps_malloc`) and won't build on the host; macros kept identical so the reference matches the device |
| `mkfixture.py` | writes a synthetic 1200×1600 PPM to stdout (low + mid + high frequency, deterministic) |
| `fixture.scans` | the scan script that makes the fixture's progressive layout match the real corpus |
| `fixture.jpg` | the committed fixture (304 KB) |

## Regenerating the fixture

```sh
python3 mkfixture.py | cjpeg -quality 80 -scans fixture.scans -outfile fixture.jpg
```

The content deliberately mixes frequencies: the truncation bug only bites on mid/high
frequency (hard disc edges, text strokes, a 2 px checkerboard, fine diagonals), so a
smooth gradient-only fixture would pass even with the bug. Dimensions are multiples of 16
so block boundaries land on image boundaries.

`fixture.scans` must stay byte-structurally identical to the real books' progressive
layout — interleaved single DC scan, then per-component non-interleaved AC bands at
`Ah=0/Al=0`. Two gotchas are annotated in that file: cjpeg's component indices are
**0-based**, and libjpeg's default `-progressive` uses successive approximation + interleaved
AC, which is **not** what the corpus looks like.

## Reading a run

```
fixture.jpg  (1200x1600 渐进式)
    scale 1/2 : 600x800 (参考 600x800，比 600x800)  mean=0.208 max=2  >2:0.000%  >5:0.000%
         实测峰值 2400000 B / 估算 2465536 B OK
```

- `mean` / `max` / `>2` / `>5` — abs pixel-difference stats vs the reference.
- `实测峰值` / `估算` — the counting allocator's real peak vs `pjscaled::peakBytes()`.
  The estimate must be an upper bound (it decides whether the decode is attempted on the
  device); a `!! 估算偏低` line fails the run.
- `DECLINED` — the decoder refused the file (baseline SOF, or a feature it doesn't
  implement). Not a failure: the caller falls back to JPEGDEC. The 4 baseline covers in the
  corpus decline here, and that is correct — JPEGDEC decodes those at full scale anyway.
