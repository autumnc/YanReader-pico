# tests/host/parser — host-side probe for the EPUB chapter parser

Runs the **real** `ChapterHtmlSlimParser` (`components/crossmux/lib/Epub/Epub/parsers/`)
on Linux x86-64 with plain `g++`, and dumps what it parsed: per page, the **footnote
table**, the **link rects**, and the **anchor table**. No ESP-IDF, no CMake, no device,
no flashing, no zip — it takes one `.xhtml` file.

It exists to answer one question cheaply: *did this parser change silently alter which
`<a>` elements get registered as footnotes, and which get a touchable link rect?*
That question came up in the "注文区点注号跳不回正文" fix (see `--verify-fix` below), and it
is exactly the kind of change you cannot see on the device without a book that happens to
have the right markup.

## Run it

```sh
tests/host/parser/run.sh                 # build + run fixture.xhtml, diff vs expected.txt
tests/host/parser/run.sh FILE.xhtml      # build + run your own .xhtml (prints, no diff)
tests/host/parser/run.sh --update        # regenerate expected.txt from fixture.xhtml
tests/host/parser/run.sh --verify-fix    # also build the PRE-FIX parser and show the delta
```

Exit status is non-zero when the transcript drifts from `expected.txt`. Nothing is written
outside `tests/host/parser/build/` (gitignored).

Pass `fontId` / `width` / `height` straight to the binary if you need to — the driver takes
`probe <file.xhtml> [fontId] [width] [height]` and defaults to `0 684×1000`:

```sh
tests/host/parser/build/probe some.xhtml 1 1216 684
```

## Files

| file | what |
|---|---|
| `probe.cpp` | the driver: construct a `ChapterHtmlSlimParser`, parse one file, print each page's transcript |
| `stubs.cpp` | `Page::addLink`/`addFootnote` capture into globals, plus the image / bidi / hyphenation stubs the parser links against |
| `stubs/*.h` | host shims for `Arduino.h`, `HalStorage.h`/`HalFile`, `GfxRenderer.h`, `Epub.h`, `Logging.h`, … |
| `fixture.xhtml` | a two-note sample: body markers + a note section with `<a class="note-backref">` |
| `expected.txt` | the golden transcript for `fixture.xhtml` |
| `run.sh` | one-command build + run + diff |
| `build/` | build output (gitignored) |

## What the fixture exercises

Two body paragraphs each carry a marker `<a href="fixture.xhtml#note-NNN">`, and the note
section carries the matching back-links `<a class="note-backref" href="fixture.xhtml#ref-NNN">`.
Both use the `filename#anchor` form (calibre's habit) so the `localAnchorOf` filename rule
is in play. The golden transcript is therefore sensitive to:

- the **footnote registration direction** — body markers must become footnotes, the
  back-links must not (2 entries, not 4);
- the **link rects** — body markers get one each, *and the back-links get one each too*
  (4 rects, not 2). Zero rects on the back-links is the bug.

## `--verify-fix` (why this harness has teeth)

By default it builds the parser from `${PRE_FIX_REF:-11344f2^}` — the revision before the
"注文区点注号跳不回正文" fix, where a back-ref `<a>` was dropped from the internal-link path
together with its footnote registration — and diffs its transcript against the working
copy. Expected delta on `fixture.xhtml`:

```
-total: 2 footnote entries, 2 link rects, 1 pages
+total: 2 footnote entries, 4 link rects, 1 pages
```

i.e. the fix adds the two back-link rects **without** disturbing the footnote count. If the
diff is empty, `run.sh` exits non-zero ("is the fix present?"). Override the reference with
`PRE_FIX_REF=<rev> tests/host/parser/run.sh --verify-fix`.

## Build gotchas (already handled in `run.sh`)

- **expat must be compiled as C.** `g++` on `lib/expat/*.c` fails on `void*` conversions;
  the three objects are built with `gcc` and `-DXML_CONTEXT_BYTES=1024 -DXML_NS=1`
  (matching `components/crossmux/CMakeLists.txt`).
- **`TextBlock.cpp` is linked for real, never stubbed.** Its `xpos` array is arena-backed;
  a stub constructor that drops it makes `wordXpos(i)` read out of bounds and segfault.
  (The same crash reproduces on an unmodified parser, which is how you can tell it is the
  stub's fault and not a regression.)
- **`stubs/` comes first on the include path** so `GfxRenderer.h` / `HalStorage.h` /
  `Arduino.h` shadow the real ones. The parser's `BidiUtils::BidiBaseDir` enum (declared
  inside `namespace BidiUtils` in the real `GfxRenderer.h`) is copied into the stub.

## Caveats

- **Font metrics are synthetic** (`getLineHeight` 16, glyph advance 8px), so page breaks
  are *not* the device's. The probe validates registration / rects / anchors, not pagination
  fidelity — never read the page numbers as a layout prediction.
- **No CSS is loaded** (the parser gets an empty stylesheet path), so style-driven
  behaviour — e.g. the `border-bottom` annotation lines — is out of scope here.
- **It parses a single `.xhtml`, not a zip**, and it drives `parseAndBuildPages()`
  directly — so it does **not** cover the reader's lazy-build / `loadPage` path (the
  "注文被截断" bug lived there, on the reader side, not in the parser).
- `expected.txt` is a golden transcript against *this* stub set. Changing a stub (font
  metrics, a new required virtual in `GfxRenderer`) legitimately changes it — rerun with
  `--update` and eyeball the diff before accepting.

Related: `tests/host/ime/` (same convention, a different subsystem), and the `dither` /
`layout` / `rotation` hosts.
