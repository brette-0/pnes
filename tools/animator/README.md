# `pnes Animation Designer`

This is an internal tool used to author sprite animations for use with the pnes library.

### Building

Standalone, like `tools/uitk` -- the root CMake build never adds it as a
subdirectory:

```
cmake -B build -S tools/animator
cmake --build build
```

Two targets come out of that:

* `animator` -- the designer. Needs Qt6 (Qt5 also works) Widgets development
  packages.
* `animator-export` -- the same export with no designer attached, built from
  the pipeline's Qt-free half. **No Qt at all**, so it builds anywhere a C++23
  compiler and toml++ do.

toml++ is used from the system if installed, fetched otherwise -- the same
fallback `tools/blobman` has. `-DANIMATOR_GUI=` picks what gets built: `AUTO`
(the default) builds the designer when Qt is found and says so when it is not,
`ON` makes a missing Qt an error, `OFF` builds only the exporter.

The root build does configure this directory, at configure time, to build and
run `animator-export` -- see *Headless export* below.

### Usage

```
animator [-t|--tall] [<project.anim> | <manifest.toml>]
animator      -E|--export-all [-i <project.anim>] [-o <dir>] [-v]
animator-export               [-i <project.anim>] [-o <dir>] [-v]
```

The file is either a project (`.anim`) or a blobman manifest (`tools/blobman`,
e.g. `demo/chr/config.toml`) -- told apart by extension, since both are TOML.
Either can also be opened from the File menu. `-t`/`--tall` starts in 8x16 mode,
which *View -> 8x16 Tiles* toggles; a project carries its own mode, so the flag
does not apply to one.

`-E`/`--export-all` exports instead of opening anything; `animator-export`
needs no switch to do that, since it is all it does.

### Current state

**Tile sheet.** A permanent panel down the left of the window -- it is where
every stamp comes from, so it stays visible whichever metasprite is being
worked on. `chr_table.hpp` is the read side of blobman: it parses the
manifest with the same validation blobman applies, loads every include, and
builds a table of cells -- one per 8x8 tile, or per 8x16 tile pair -- each
carrying the PPU tile id (`(VMA + offset) / 16 & 0xFF`, exactly how blobman
derives the symbols in `chr.h`), the byte OAM takes for it (that id with the
pattern-table half the VMA lands in folded into bit 0, for an 8x16 cell -- see
*What a header holds*), the entry and include it came from, and its pixels as
black-RGB palette indices. `tile_canvas.hpp` draws that 16 cells
across, scrolling vertically, at an integer zoom derived from the monitor's
vertical resolution (32px/tile at 1080p, 64px at 4K). Hovering a cell reports
its entry, include, tile index, id and size; clicking picks it.

In 8x16 mode a cell is an even id plus the next one, which is how the PPU pairs
patterns when sprites are 8x16 (the pattern byte's bit 0 selects the pattern
table, bits 7-1 the pair). A tile with no partner -- an odd starting id or an
odd tile count in an include -- is shown as 8x8 and reported as a warning on
stderr rather than mispaired.

**Metasprite tabs.** The tabbed area beside the sheet holds metasprites
(`metasprite_page.hpp`). Each one has an attribute section and a second row of
tabs below it -- one per animation frame.

The metasprite attributes own the extent **in tiles**, which is indifferent to
8x8 versus 8x16: a 1x1 metasprite is one tile, meaning one 8x8 sprite or one
8x16 sprite depending on the mode the sheet is in. The pixel extent and what a
tile currently measures are derived and shown, never entered. The extent applies
to every frame -- frames of one animation are the same size by definition -- and
shrinking drops the sprites that fall outside it, saying how many. Sprites are
stored by slot rather than pixel offset for the same reason, so switching the
mode moves where a slot *is* without disturbing which slot a sprite is in.

**Frames** (`metasprite_frame.hpp`) are what carry sprites. Each has a name (the
tab label follows it as you type) and a timer: a plain integer entry, 0-255, in
NES frames -- a byte of ticks at the console's refresh. The unit and what 0
means are in the row label, not in the box.

> **Playback convention.** Frames are contiguous: they play in tab order, and
> the last frame loops back to the first. A timer of **0** ends the animation on
> that frame instead of advancing, so a one-frame animation with timer 0 is a
> still pose. This is recorded here and in `metasprite_frame.hpp` because the
> editor does not act on it -- there is no preview to act with, and no field to
> set, which is exactly why it has to be written down. Whatever consumes these
> metasprites is what implements it.

On a frame's field: left-click stamps the picked tile into the slot under the
pointer (one sprite per slot, a stamp replaces what was there), right-click
cycles that sprite's mirroring -- none, V, H, H+V, and back round -- and
middle-click removes it. Mirroring is applied when drawing, so the stored art is
always the unmirrored tile; the enum's values are the cycle order, not OAM's bit
layout (there, bit 6 is the horizontal flip and bit 7 the vertical, so anything
emitting OAM has to map rather than cast).

**Preview** (`animation_preview.hpp`) sits beside the metasprite's attributes
and plays the animation those frames describe: Play/Pause, Restart, and a label
naming the frame on screen and its position. It is the only place the playback
convention is actually executed -- frames in order, the last looping back to the
first, a timer of 0 stopping on that frame -- so it is also the only way to see
whether an animation reads right.

The clock ticks once per NES frame (60.0988Hz, not a round 60) and counts holds
down in frames rather than converting each to a duration. Frames are composed to
images by the page whenever anything changes, so the preview re-renders nothing;
playback keeps its position across an edit rather than restarting under your
hands, and the box is a fixed size so editing the extent does not shuffle the
layout. A metasprite too big for the box is fitted rather than drawn at integer
zoom.

A metasprite tab's label carries its frame count; the frame tabs carry their own
names.

*File -> New Metasprite* (Ctrl+N) adds a tab, *Close Metasprite* (Ctrl+W)
removes one, *New Frame* (Ctrl+T) and *Close Frame* (Ctrl+Shift+W) do the same
for frames within the current metasprite -- both tab bars also carry a `+`
button, so adding either does not depend on finding the menu -- and *Edit -> Clear Frame* empties the
visible frame's field. Closing every tab is allowed -- the sheet is unaffected, since it is not a tab. Re-reading the
manifest (*Reload*, or toggling 8x16) drops the pick, since cell indices only
mean anything for the table they came from; sprites already placed keep their
own copy of the art and stay put.

There is no animation timeline -- the field is the composing surface only.
Frames become C++ through *File -> Export Header*, below.

### Palettes

*Window -> Palettes...* opens the palette editor (`palette_window.hpp`) in a
window of its own -- palettes are edited now and then, while the sheet and the
fields are in constant use and want the width. It is non-modal, so it can sit
open beside the main window while tiles are being placed.

A palette is four entries, as the PPU takes them: index 0 plus three colours,
each an index into the console's own 64-colour master palette (`palette.hpp`),
picked from a grid of it rather than a general colour dialog -- an arbitrary RGB
value is not something a palette could hold. **Index 0 is shown but not
editable**: it is the transparency/backdrop slot, fixed at NES black, because a
sprite's index 0 shows whatever is behind it.

Palettes are named, and the name is the key: every metasprite records the
palette it uses by name (its own **Palette** attribute, per metasprite -- not
per frame, since an animation recolouring halfway through is not what a frame is
for), and the tile sheet has its own dropdown above it. The sheet's choice is a
*view* setting, independent of any metasprite: picking tiles is easier in the
colours they will be drawn in, but the sheet is not part of one metasprite.
Deleting a palette leaves anything that used it on the placeholder colours
rather than on whichever name took its place.

The master palette's RGB values are an approximation -- every 2C02 palette is,
since the console generates composite video -- and they are for display only.
What a saved palette holds is indices.

### Unsaved changes

Anything a save would record -- a metasprite's name, extent or palette, a
frame's name, timer or sprites, a palette's colours, the tile mode, which
manifest the tiles come from -- marks the project dirty, which shows as a `*` in
the title bar.

Every way the window can close (title bar, *File -> Quit*, Ctrl+Q, the session
ending) goes through one `closeEvent`, which offers **Save / Discard / Cancel**.
Choosing Save and then cancelling the Save As dialog, or a write that fails,
leaves the project dirty and the close is abandoned -- nothing is discarded on a
save that did not happen. Opening another project asks the same question first.

Closing a metasprite or frame tab that holds sprites also asks, since nothing
undoes it; an empty one closes without the interruption. Those prompts are
suppressed while a project is being opened, where the tabs being closed are the
ones just replaced.

### Project files

A project is an `.anim` file plus a sidecar folder of `.msprite` metasprite
manifests (`project.hpp`). Both are TOML, like everything else in this project's
tooling, so one parser covers the lot and the files stay hand-editable and
diffable. *File -> Save Project* (Ctrl+S) writes both; *Open Project*
(Ctrl+Shift+O) restores the window from them.

```toml
# player.anim
[project]
chr         = "../demo/chr/config.toml"   # the blobman manifest, relative to this file
metasprites = "metasprites"               # sidecar folder, relative to this file
tile_mode   = "8x16"                      # "8x8" or "8x16"
files       = [
    "player_walk.msprite",
]
```

```toml
# metasprites/player_walk.msprite
[metasprite]
name   = "player_walk"
width  = 2    # in tiles; a tile is 8x8 or 8x16 by the project's tile_mode
height = 2

[[frame]]
name  = "walk_1"
timer = 8
sprites = [
    { col = 0, row = 0, id = 0x40, entry = "normalSprites", include = "player_standing", tile = 2, flip_h = false, flip_v = false },
]
```

Notes on the format:

* The `chr` path is stored **relative to the `.anim`**, so a project and its art
  move or check out together.
* Array order is the ordering: `files` is metasprite order, `[[frame]]` order is
  play order, and `sprites` order is placement (draw) order.
* A sprite names its art by `entry`/`include`/`tile`, not by `id` alone -- an id
  is ambiguous, since two entries at the same offset into their VMAs share ids
  (`demo/chr/chr.h` has several). `id` is written anyway because it is what a
  consumer puts in OAM. On load, `entry`/`include`/`tile` is matched first and
  the id is the fallback for a manifest reorganised since the save; a sprite
  matching neither is skipped and counted on stderr.
* Palettes live in the `.anim` as `[[palette]]` tables, since they are shared by
  every metasprite; each `.msprite` names the one it uses in `[metasprite]
  palette`. A name, not an index, so adding or removing a palette cannot
  repoint a metasprite at a different one, and a name nothing answers to falls
  back to the placeholder colours.
* Mirroring is two booleans, `flip_h` and `flip_v`, rather than a packed value --
  nothing then has to know whether the packing meant the editor's cycle order or
  OAM's bit layout.
* Renaming a metasprite **moves** its file: the next save writes the new name
  and removes the old stem, so the folder never collects a stale copy. Only a
  stem this project wrote before, and that nothing claims now, is removed --
  a save never touches a file it did not write, including the manifest of a
  metasprite that has since been closed. Writes all happen before any removal,
  so two metasprites swapping names cannot lose one.
* Two metasprites whose names fold to the same filename stem are refused rather
  than silently overwriting each other.

### Generated code

*File -> Export Header...* (Ctrl+E) writes the metasprite being edited as a
generated `.hpp`; *Export All Headers...* (Ctrl+Shift+E) asks for a folder and
writes one header per metasprite into it, each named after the metasprite
(`gen/mary_walking.hpp`). Exporting is not saving: it writes generated C++
somewhere of its own and leaves the project exactly as dirty as it was, since
none of it is recorded in the `.anim`. The folder is not cleaned out either --
a header for a metasprite since renamed or closed is left alone, because unlike
the sidecar folder, nothing says that folder is this tool's to own.

```cpp
// Generated by the 'pnes' Animation Designer -- do not edit by hand.
// Metasprite "mary_walking", of animations.anim.
namespace gen {
class mary_walking {
public:
    mary_walking() = delete;
    static constexpr u8  kWidth  = 2;   // extent in tiles
    static constexpr u8  kHeight = 1;
    static constexpr u16 kSize   = kWidth * kHeight;   // slots a frame holds
    static constexpr u8  kFrames = 3;
    static constexpr u16 kBytes  = kFrames * kSize;
    static constexpr u8  kNoTile = 0xff;
    static constexpr u8  kFrame_frame_1 = 0;           // one per frame, by name
    static constexpr u8  kTimers[kFrames] = { 8, 8, 8 };
    static constexpr const u8* Get() { return &kTiles[0][0]; }
    static constexpr const u8* Get(u8 frame) { return &kTiles[frame][0]; }
private:
    static constexpr u8 kTiles[kFrames][kSize] = { ... };
};
} // namespace gen
```

What a header holds is **pattern bytes**, one per slot, row-major, and the
sizes a consumer would otherwise hand-count: the extent, the slot count (`kSize` -- the
metasprite's total size, which is also how many hardware sprites drawing it
takes), the frame count and the byte total. The frames are one contiguous
array, so the frame after `frame` starts at `Get(frame) + kSize` and an
animation can be walked with a pointer -- which is what `Actor::animationFrame`
in the demo is. `kTiles` itself is private: a frame is reached through `Get`,
by `kFrame_<name>` where a name is wanted.

The timers come too, in a table of their own rather than in the middle of a
frame's tiles: they are frame data, the playback convention is counted down in
them, and a 0 is what ends an animation (see the convention above). A timer
outside 0-255 can only come from a hand-edited `.msprite`, and is clamped.

What is deliberately **not** in a header:

* the **palette**, which the editor keeps per metasprite but the console takes
  as an OAM attribute byte and a PPU palette write -- not frame data;
* **mirroring**, which the editor stores per sprite and nothing emits yet;
* whether a slot is **8x8 or 8x16**, which is PPUCTRL's business. The pattern
  byte is written for the height the cell was built at, and an 8x8 cell has no
  half bit to carry because PPUCTRL supplies it.

What a slot's byte *does* carry, for an 8x16 one, is the **pattern-table half**
in bit 0: the hardware reads the pair as bits 7-1 and the half as bit 0, and
the half is whichever one the entry's VMA puts the art in. The manifest is the
only thing that knows that, so it is folded in here rather than left to the
consumer. A bare id handed to OAM does not merely lose the half -- it addresses
the same index in the *other* pattern table, and draws whatever happens to be
banked there.

A slot nothing was stamped into holds `kNoTile` (`0xff`). That is a convention
of this data and not of the hardware -- 0xff is a legal tile -- so a metasprite
with a hole in it has to skip the slot rather than write it to OAM.

A metasprite with no frames is skipped by *Export All* (named on stderr) and
refused by *Export Header*: there is no animation to emit, and a zero-length
array is not something C++ takes. Two metasprites whose names fold to one
identifier are refused outright, before anything is written, for the same
reason the `.msprite` stems are: one header would overwrite the other.

### Headless export

The export is also a command line, and the same one either binary takes:

```
animator-export -i <project.anim> -o <dir> [-v]
animator --export-all -i <project.anim> -o <dir> [-v]
```

It writes what *Export All Headers* writes -- one header per metasprite, into
`-o` (`gen/` beside the project if left out) -- and nothing else: no window is
opened, no display server is needed, and `animator-export` does not even link
Qt. Every piece of the pipeline it drives (the manifest reader, the project
format, the code generator) is deliberately Qt-free, which is what makes that
possible; the designer runs the identical code, so a headless export and a
*File -> Export All* of the same project write identical bytes.

**Pattern bytes are re-resolved from the manifest on every export**, not taken
from the `.msprite` files. A saved sprite names the art it was stamped from --
its entry, its include and the tile index within that include -- and that is
what survives a CHR edit: repacking an include or moving an entry's VMA
renumbers tiles, and the sprite still means the same picture. So the manifest
is read, each sprite is matched back to its cell, and the cell's pattern byte
is what gets emitted. A sprite that names art the manifest no longer has
**fails** the export rather than exporting a hole where a drawn sprite should
be: the project and the CHR layout genuinely disagree, which is for a human to
settle.

What it logs, on stdout, is what it is exporting: the project and its sizes,
the manifest it resolved against, the output folder, and one line per header
with the extent, the slot count, the frame count and the bytes. `-v`/`--verbose`
adds every frame's pattern bytes exactly as the header will hold them, the
sheet's own cell warnings, and every sprite whose stored byte the manifest
disagreed with. Problems go to stderr, and a failed export exits non-zero with
the reason on it.

```
animator: project demo/animations/animations.anim: 2 metasprites, 1 palette, 8x16 tiles
animator:   manifest ../chr/config.toml -> demo/chr/config.toml
animator:   output   demo/animations/gen
animator:   sheet    4 entries, 70 cells, blob 16384 bytes (27 cell warnings -- -v lists them)
animator:   mary_standing.hpp: 2x1 tiles, 2 slots, 1 frame, 2 bytes, palette 'player'
animator:     frame 0 "standing" timer 0: 05 07
animator: exported 2 headers, 8 bytes of frame data, into demo/animations/gen
```

That log is the point of the headless path having one at all: it runs inside
somebody else's build output, where a step that said nothing would be the one
step nobody could tell had gone stale.

### In the build and in CI

The root `CMakeLists.txt` runs this at **configure** time -- the
`animator-export` step, beside the `uitk-export` and `blobman` ones -- so the
generated headers are current before anything compiles against them. It
configures this directory with `-DANIMATOR_GUI=OFF`, builds `animator-export`
and runs it once per project it finds -- the projects are globbed, so a new one
needs no edit there. Every project file, every `.msprite` beside them and the
animator's own sources are configure dependencies, so editing an animation
reconfigures, re-exports and rebuilds on the next `cmake --build <builddir>`
with nothing to remember.

It sits *after* the blobman step deliberately: pattern bytes are resolved
against the CHR layout, so the export has to see the manifest that was just
packed.

A host that cannot build the exporter -- no compiler of its own, no system
toml++ and no network to fetch one, which is several of the cross-build images
-- gets a `STATUS` line and the committed headers instead, exactly as the
blobman step falls back to the committed CHR pair. The generated headers are
committed for that reason. CI's `animator-check` job is what catches them
drifting: it builds the exporter, re-exports verbosely and diffs the result
against what is committed.
