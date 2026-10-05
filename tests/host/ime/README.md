# tests/host/ime — host-side pinyin IME harness

Runs the **real** `main/ime/IME.cpp` (+ `yong_dict.cpp`, `yong_pinyin.cpp`) on Linux
x86-64 with plain `g++`. No ESP-IDF, no CMake, no device, no flashing. This is the
`tests/host/` convention the architecture review asks for
(`docs/架构评审-2026-10-04.md:250`).

It exists to reproduce and instrument the **"stray remainder" candidate/commit bug**
(typing `zhege` and selecting 这个 left an extra `g` in the composition; `jiushi` →
就是 left `shi`) and now guards against its regression.

## Run it

```sh
tests/host/ime/run.sh                  # build + diagnostic dumps + regression test
tests/host/ime/run.sh --regress-only   # just the regression test (quiet on success)
tests/host/ime/run.sh --verify-fix     # prove the regression FAILS on the pre-fix IME.cpp
tests/host/ime/run.sh zhege 1          # ad-hoc: feed "zhege", commit page index 1
tests/host/ime/run.sh --regress jiushi 就是
```

Exit status is non-zero when a regression case fails. Nothing is written outside
`tests/host/ime/build/` (gitignored).

## Files

| file | what |
|---|---|
| `ime_driver.cpp` | the driver: init like `main.cpp`, feed keys, dump state, commit, assert |
| `stubs.cpp` / `stubs.h` | `g_settings` + the 6 `SettingsManager` getters IME.cpp calls, the embedded dictionary symbols (`.incbin` of the real `main/ime/*.bin`), the candidate-width callback |
| `esp_log.h`, `esp_timer.h` | host shims for the two ESP-IDF headers IME.cpp includes |
| `run.sh` | one-command build + run |
| `build/` | build output (gitignored) |

## What the driver does

1. `IME::getInstance()`, then the same sequence as `main.cpp:686-702`:
   `begin()` → `setPageSize(7)` → `setWidthFn()`/`setDisplayWidth()` →
   `setActive(true)`.
   The diagnostic scenarios also run with **no** width callback (`--fixed`), which drops
   `buildPage()` into its fixed page-size fallback — both paging modes are covered.
2. Feeds the key sequence **one key at a time** through `IME::handleKey()`. That matters:
   the bug is planted by the *intermediate* keystroke (`zheg` / `jiush`), whose lookup
   ends with an empty candidate table.
3. After every keystroke prints `_code`, `displayCode()`, `_pageStart`, `_page.size()`,
   `_all.size()`, `_curPage`, `_partialStart`, `_remainder`, `_prefix`, `_maxMatchLen`,
   plus a table of every candidate with its index, its **`_candLen`** entry and its
   `_predictCandidateKeys` entry (the parallel arrays `commit()` indexes into).
4. `--regress` mode: finds the target word **on the current page**, commits that exact
   page index, and asserts `commit()` returned true, `out ==` the whole word, and that no
   leftover composition remains (`_code`, `_prefix`, `_remainder`, `_ambigCommitted` all
   empty). It deliberately does *not* assert `composing() == false`: that also counts the
   prediction phase, which is a legitimate post-commit state (committing 九十 leaves
   `_predicting` set — see `jiushi` index 1).

### `#define private public`

`_candLen`, `_pageStart`, `_prefix`, `_remainder`, `_partialStart`, `commit()` … are
private. The driver reads them with the classic host-test hack of including `IME.h`
under `#define private public` (`ime_driver.cpp` only; every std header is pre-included
first so the macro cannot corrupt them). **No file under `main/` is modified**, and access
specifiers do not affect layout, so the driver and the verbatim `IME.cpp` see the same
object. This is a host-test-only hack; do not copy it into firmware code.

## What it reproduces (the bug)

Typing `zhege` produces `这个` at page index 0 with `_candLen[0] == 5` — the code length is
correct, so nothing is wrong in the dictionary or the candidate phase. The leftover comes
from state left over by the **previous** keystroke:

```
zheg   _all is empty when Phase 8 runs -> _partialStart = 0, _remainder = "g"
zhege  Phase 4 fills 80 candidates (= IME_FAST_CANDIDATE_LIMIT) and lookup() returns
       early at the "phrase-limit" check, so Phase 8 — the only place _partialStart /
       _remainder are refreshed — never runs; the zheg values survive
commit  partial = (_remainder.length() > 0 && idx >= _partialStart - _pageStart) is TRUE
       for every candidate, so the whole word 这个 takes the continuation branch, and
       because partial is true `_remainder = _code.substr(consumedLen)` is skipped:
       _code = stale "g", _prefix = "这个", displayCode renders 这个'g
```

`jiushi` → 就是 is the same thing with `_remainder = "sh"` left by `jiush`.

The driver's keystroke trace shows it directly. Pre-fix (git HEAD build):

```
zheg     partialStart=0  remainder='g'   all=12
zhege    partialStart=0  remainder='g'   all=80     <- stale
commit(0) returned 0, out=''   _code='g' _prefix='这个'  composing=1
```

Post-fix (`clearCandidates()` now also clears `_partialStart`/`_remainder`):

```
zhege    partialStart=0  remainder=''    all=80
commit(0) returned 1, out='这个'   _code='' _prefix=''  composing=0
```

`run.sh --verify-fix` builds the pre-fix `IME.cpp` from `git show HEAD:main/ime/IME.cpp`
into `build/` (a temp copy, never in the repo) and asserts the regression **fails** there
and **passes** against the working copy. Current status: fails pre-fix, passes post-fix,
both paging modes, both sequences.

Settings the IME snapshots in `begin()` can be overridden on the command line (they must
be set before `begin()`, which the driver does): `--no-sentence`, `--no-docctx`,
`--fuzzy <csv>`, `--predict <mode>`. With the real dictionary none of them change the
behaviour of the two regression cases — they all reproduce pre-fix and pass post-fix.

## Adding a case

Append to `REGRESS_CASES` in `run.sh` (`"<letters> <word>"`), or drive the binary
directly: `build/ime_driver --regress <letters> <word> [--fixed]`. The assertion is
"commits whole, in one step"; anything else (a live composition, a stale `_code`/
`_prefix`, a wrong `out`) fails with rc=1.

## Caveats

- The user-dictionary paths in `IME.cpp` are the device's `/sdcard/settings/*.txt`. That
  directory does not exist on the host, so every `fopen()` fails and the harness never
  reads or writes a user dict: each run starts from the static dictionary, which is what
  a regression test wants. It also means learning-driven reordering is *not* covered here.
- One scenario per process (`run.sh` starts a new process per case) so that in-memory
  learning from one commit cannot perturb the next case.
- Width paging uses `hostime::textWidthApprox()` (ASCII 11px / other 22px, matching
  `FontRenderer::charWidth` for the 22px content font) and a 1204px candidate line. It is
  an approximation of the on-device font, but the bug is independent of paging — both
  `--fixed` and width paging reproduce it.
