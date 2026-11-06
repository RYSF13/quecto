# quecto

![LOGO](logo.png)

A small terminal text editor written in plain C99, with no dependencies
beyond the standard library.

Quecto opens a file, lets you type into it, and writes it back. That is the
whole idea. There are no modes, no plugins and no configuration file; the
source is short enough to read in an afternoon and the binary is small
enough to keep on a rescue image.

---

## Features

- **Small.** One C file plus a header, under 1,800 lines including comments,
  no third-party code.
- **UTF-8 aware.** Wide characters are drawn two columns wide, the cursor
  steps over them in one move, and one press of Backspace removes a whole
  character. Invalid bytes are shown, not mangled.
- **Real tab handling.** Tabs expand to the next stop of three columns and
  the text stays aligned, instead of appearing as `^I`.
- **Search.** Plain substring search with `Ctrl+F`, wrapping round at the
  end of the file. Press Enter on an empty prompt to repeat it, so
  `Ctrl+F`, `Enter` is "find next".
- **Go to line.** `Ctrl+G` takes an absolute number or a relative one
  (`+20`, `-5`).
- **Atomic saves.** The buffer is written to a temporary file, flushed, and
  renamed over the original, so an interrupted save cannot leave a
  half-written file behind.
- **Soft wrapping.** Long lines wrap at the window edge without being
  changed on disk.
- **Fast on large files.** Per-line layout is cached, so a keystroke in a
  200,000-line file still costs a fraction of a millisecond.
- **Boring on purpose.** No syntax highlighting, no colours, no plugins.
- **MIT licensed.**

---

## Build

```sh
git clone https://github.com/RYSF13/quecto
cd quecto
make build
```

This produces `./quecto`. To compile with warnings treated as errors:

```sh
make check
```

## Install

```sh
sudo make install
```

installs three things:

| Path                            | What                                    |
| :------------------------------ | :-------------------------------------- |
| `/usr/local/bin/quecto`         | the binary                              |
| `/usr/local/bin/q`              | symlink, for people who type quickly    |
| `/usr/local/share/man/man1/quecto.1` | the manual page (`man quecto`)     |

Install somewhere else, or stage the install for a package:

```sh
PREFIX=$HOME/.local make install
DESTDIR=/tmp/stage PREFIX=/usr make install
```

Build a static binary (Alpine, containers, rescue media):

```sh
make STATIC=1 build
```

Remove everything again:

```sh
sudo make uninstall     # installed files
make clean              # local binary
make purge              # both
```

`make help` lists every target and variable.

---

## Usage

```sh
quecto [file]     # or: q [file]
```

If the file does not exist it is created on the first save. With no
argument, quecto starts an empty buffer and asks for a name when you save.

### Keys

| Key                     | Action                                              |
| :---------------------- | :-------------------------------------------------- |
| `Ctrl+W`                | Save                                                |
| `Ctrl+N`                | Save as, and continue editing the new name          |
| `Ctrl+Q`                | Quit (press twice if there are unsaved changes)     |
| `Ctrl+F`                | Find; Enter on an empty prompt repeats the last one |
| `Ctrl+G`                | Go to line (`12`, `+3`, `-1`)                       |
| `Ctrl+K`                | Insert a character by code point, e.g. `0x4e2d`     |
| `Ctrl+V`                | Show the version                                    |
| `Ctrl+L`                | Redraw the screen                                   |
| Arrows                  | Move the cursor                                     |
| `Home` / `End`          | Start / end of line                                 |
| `PageUp` / `PageDown`   | Move a screenful                                    |
| `Backspace`             | Delete the character before the cursor              |
| `Delete`                | Delete the character under the cursor               |
| `Tab`                   | Insert a tab                                        |
| `Esc`                   | Ignored                                             |

A `*` next to the file name in the status bar means there are unsaved
changes. The number on the right is the cursor position: line, then display
column.

### Search

`Ctrl+F` asks for a string and jumps to the first match after the cursor,
carrying on from the top of the file if it reaches the end (the status bar
says so). Pressing `Ctrl+F` again and then Enter repeats the search, so the
usual rhythm is `Ctrl+F`, type, Enter, `Ctrl+F`, Enter, `Ctrl+F`, Enter.

It is a plain substring match, not a regular expression, and there is no
search-and-replace: `sed` already does that better than an editor ever
would.

Case is smart. A pattern in lower case matches either case, so `todo` finds
`TODO`; a pattern containing an upper case letter is matched exactly, so
`TODO` finds only `TODO`.

### Saving

Saving writes a temporary file beside the target, flushes it to disk, and
renames it into place. Either the save completes or the old file is still
there, byte for byte -- there is no window in which the name points at a
half-written file. Permissions are carried over from the old file, and the
temporary file never survives a successful save.

This is a different thing from a swap file. A swap file (`.file.swp`, as vi
writes) is a crash *recovery* journal: the editor keeps a second copy so a
killed session can be resumed. Quecto has no session to recover, because
the save either happened or did not. Nothing is left on disk to clean up.

Two exceptions, both deliberate: a symlink, hardlink, device node or pipe is
written through in place instead of being replaced, because a rename would
quietly break the link. Those saves are not atomic.

### A short session

```sh
quecto notes.txt      # open or create
# type, move around with the arrow keys
# Ctrl+W to save, Ctrl+Q to quit
```

---

## UTF-8 and tabs

These two areas are where most small editors fall over, so it is worth
saying exactly what quecto does.

**Wide characters.** 中 is one character, three bytes, and two columns. The
cursor moves over it in one arrow-key press, one Backspace deletes the whole
thing, and it never gets split across the wrap point. The width of a
character comes from the Unicode East Asian Width property, and combining
marks and variation selectors are treated as zero width so accents sit on
top of the letter they belong to.

**Invalid bytes.** A file full of half-truncated UTF-8 still opens. Each
undecodable byte is shown as one replacement glyph and written back exactly
as it was read, so opening and saving never quietly rewrites a file that was
not valid in the first place.

**Tabs.** A tab is a jump to the next column, not a printable symbol, so it
is drawn as spaces up to the next stop. Stops are every **3** columns
counted from the left edge of the text:

```
 a  x            <- "a\tx"
 abc   y         <- "abc\ty"
 中文  z         <- "中文\tz"
```

The file keeps its tab character; only the display is expanded, so nothing
is rewritten on save. One `#define TAB_WIDTH 3` at the top of `quecto.h`
changes every stop at once.

---

## How it works

The whole editor is `quecto.c` with the data types in `quecto.h`. The parts
worth knowing about:

- **Lines.** The buffer is an array of lines, each a NUL-terminated byte
  buffer of UTF-8 with a length of its own, because a text file may contain
  NUL bytes.
- **The cursor** is a byte offset into the current line. It is always kept
  on a character boundary; moving left means "walk back over the
  continuation bytes" rather than "subtract one".
- **Layout.** One function, `glyphAt()`, decides how a character is placed:
  its width, where the next tab stop is, and whether it fits on the current
  row. Drawing and cursor placement both go through it, which is why the
  cursor lands in the right cell; if they each did their own arithmetic they
  would disagree, and the cursor would drift on any line with a wide
  character or a tab.
- **Caching.** Each line remembers how many screen rows it occupies after
  wrapping (`Line.rows`). Scrolling then costs one addition per line instead
  of re-measuring the text, and every frame is assembled in memory and
  handed to a single `write()`.
- **Saving** streams the lines straight to a temporary file descriptor, so
  a large buffer never has to be copied into a second large buffer, then
  renames it into place and flushes the directory entry.

---

## Performance

Measured on an 11 MB, 200,000-line file with a mix of ASCII, tabs and CJK
text, 80x24 terminal, on an ordinary laptop:

| Action                                   | Time   |
| :--------------------------------------- | :----- |
| Open the file and draw the first frame   | ~240 ms |
| One keystroke, at the top of the file    | ~0.2 ms |
| One keystroke, at line 200,000           | ~0.2 ms |
| 400 consecutive `PageDown` (whole file)  | ~0.9 s  |
| `Ctrl+F`, match on the first screen      | ~1 ms   |
| `Ctrl+F`, no match anywhere (whole file) | ~60 ms  |

Almost all of the open cost is parsing 200,000 lines; almost all of the
keystroke cost is the terminal, not the editor. Nothing in the drawing path
depends on the file size, and nothing depends on the length of a line except
the one line being edited. A search is the only operation that reads the
whole buffer, and only when it has to.

Where the remaining cost sits, if you ever need to care: the row sum for the
cursor position walks every line above the cursor. That is a straight
addition per line and it is fast enough here, but it is the first thing to
replace with prefix sums maintained incrementally if you want the editor to
stay instant on a ten-million-line file.

---

## Roadmap

Ideas that fit the shape of the program, roughly in the order they would
pay off. None of them are implemented, on purpose: every one of them
doubles something.

**Functions worth adding**

1. **Undo/redo.** One ring of line snapshots is enough for a 200,000-line
   file if snapshots record the changed line and its previous contents, not
   the whole buffer. `Ctrl+Z` / `Ctrl+Y`. Biggest usability win by far.
2. **Word-wise motion.** `Ctrl+Left`/`Ctrl+Right`, skipping over runs of
   characters with the same class. Needs an `isWordChar()` helper and
   nothing else.
3. **`Ctrl+S` as an alias for `Ctrl+W`.** Every other editor on earth uses
   it, and `Ctrl+W` is also the terminal's "close tab" on some systems.
4. **Open a second file.** Would need the editor state to stop being a
   single global; at that point the program has outgrown its design, so it
   is the natural boundary.

**Deliberately not worth adding.** Syntax highlighting, a config file, mouse
support, multiple windows, a plugin system, and anything that needs a
library. Each of those is available in an editor that is already installed
on your machine; none of them is why this one exists.

**Performance work, if it is ever needed**

- Maintain prefix sums of `Line.rows` (a Fenwick tree, or a dirty-range
  rebuild) instead of summing them per frame. This is the only remaining
  operation that scales with file size.
- Batch the row-cache rebuild after a big paste instead of one `lineFix()`
  per line.
- For files in the hundreds of megabytes, mapping the file with
  `mmap()` and editing through a gap buffer would remove the copy on open
  entirely.

---

## Limitations

- No undo, no syntax highlighting, no search-and-replace. See above.
- The file is rewritten in full on every save, so saving a huge file takes
  as long as writing it once.
- Saving a symlink or a hardlinked file falls back to writing in place,
  which is not crash safe.
- Files are always written with LF line endings and a trailing newline.
- The editor assumes a terminal that renders wide characters the same way
  Unicode says it should. Terminals that disagree will misplace the cursor
  on those lines only.

---

## License

MIT. See the LICENSE file in the source distribution.
