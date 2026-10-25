# Podium

A fast, native presentation editor for Linux that uses PowerPoint's `.pptx` as its file format.
C++20 + Qt 6, about 8k lines.

- **Edit**: text boxes with rich text (fonts, sizes, bold/italic/underline, colors, alignment,
  bullets, numbering, list levels), ~30 preset shapes, lines and arrows, images (insert, paste,
  drag-and-drop), tables, grouping, z-order, snapping guides, undo/redo, speaker notes.
- **Slides**: new slide from the deck's layouts, duplicate, delete, hide, drag to reorder,
  backgrounds, transitions (fade, push, wipe, cover, cut), entrance builds (appear, fade, dissolve;
  on click / with previous / after previous, by paragraph).
- **Present**: full-screen slideshow with transitions, builds and embedded video; presenter view
  with notes, next slide and timer on your laptop while the slides go to the second screen
  (`Alt+F5` rehearses it on one screen).
- **Share**: export to PDF (vector text, one page per slide).

## Compatibility

Podium opens and saves `.pptx` directly. Everything Podium edits is written as standard
PresentationML; everything it doesn't (charts, SmartArt, other animations, effects, embedded fonts,
custom XML, …) is carried through untouched: unchanged parts of the package are copied byte for
byte, and each object keeps its original XML with only the edited properties rewritten. Saved files
validate against the ECMA-376 schemas.

Known limits: masters and layouts can't be edited; colors are saved as RGB rather than theme
references; EMF/WMF pictures show as placeholders. Text renders with the fonts installed on your
system (`fc-match` decides substitutes, e.g. Calibri → Carlito, Aptos → Liberation Sans), so line
breaks can differ from PowerPoint when a deck's fonts are missing.

## Build

Dependencies: Qt 6.5+ (Widgets, Svg, Concurrent, Multimedia), libzip ≥ 1.10, pugixml, CMake.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build                         # model, pptx round-trip and UI tests (offscreen)
cmake --install build --prefix ~/.local         # binary, desktop entry, icon
```

## Use

```sh
podium deck.pptx                    # edit
podium --show --start 3 deck.pptx   # present from slide 3
podium --export-pdf out.pdf deck.pptx
podium --export-png dir/ --width 1920 deck.pptx
```

| | |
|---|---|
| Slideshow | `F5` from start, `Shift+F5` from current, `Alt+F5` presenter view |
| In the show | `→`/`Space`/click next, `←` back, number + `Enter` jump, `B`/`W` black/white, `Esc` end |
| Editing | double-click or `Enter` edits text, `Tab`/`Shift+Tab` list level or next cell, `Esc` stops |
| Objects | arrows nudge (`Shift` ×10), `Ctrl+G`/`Ctrl+Shift+G` group/ungroup, `Ctrl+D` duplicate |
| Moving | `Shift` keeps the axis / aspect ratio / 15° steps, `Alt` disables snapping |

## Layout

| | |
|---|---|
| `src/model.h` | the document model: plain values, cheap to copy (undo is a snapshot) |
| `src/pptx_read.cpp`, `src/pptx_write.cpp` | `.pptx` import (resolves theme → master → layout → slide) and export |
| `src/package.cpp` | OPC zip container (lazy reads, raw copies on save) |
| `src/render.cpp`, `src/richtext.cpp`, `src/geometry.cpp` | the one renderer used by the editor, thumbnails, show and PDF |
| `src/canvas.cpp`, `src/mainwindow.cpp`, `src/inspector.cpp`, `src/slidepanel.cpp` | the editor |
| `src/show.cpp` | slideshow, presenter view, builds and transitions |

Test helpers: `PODIUM_SAMPLE=deck.pptx` adds a real deck to the round-trip tests;
`PODIUM_SNAPSHOTS=dir` saves screenshots from the UI tests;
`tests/validate_schema.py <xsd-dir> file.pptx` checks saved files against the ECMA-376 Part 4
transitional schemas.
