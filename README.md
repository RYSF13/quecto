# Quecto

**Quecto** (or simply `q`) is a text editor designed with extreme minimalism in mind. It follows the **KISS** (Keep It Simple, Stupid) principle, stripping away everything non-essential to provide a raw, distraction-free editing experience.

## Why "Quecto"?

In the metric system:
*   **Nano** ($10^{-9}$) is small.
*   **Pico** ($10^{-12}$) is smaller.
*   **Quecto** ($10^{-30}$) is currently the smallest named prefix in the International System of Units.

While other editors claim to be lightweight, Quecto takes it to the absolute limit.

## Features

*   **Tiny Footprint**: Binary size is approximately **15KB - 20KB** (linked against libc).
*   **Distraction-Free**: No line numbers, no colors, no status bar clutter. Just your code.
*   **Modern Basics**: Full **UTF-8** support (including CJK characters).
*   **Power Tools**: Built-in **Regex** (Regular Expression) support for find and replace.
*   **Zero Config**: No dotfiles, no plugins, no startup latency.

## Installation

Quecto is designed for Linux x86_64.

1.  Clone the repository.
2.  Run the build script:

```bash
make
```

This will compile the binary, strip it, and install it to `/usr/local/bin/q`.

## Usage

To open a file:

```bash
q filename
```

### Keybindings

Quecto starts in **Edit Mode** immediately.

| Key | Action |
| :--- | :--- |
| **Arrows** | Move cursor |
| **Ctrl + S** | Save file |
| **Ctrl + Q** | Quit (Warns if unsaved) |
| **Ctrl + X** | Enter **Command Mode** |

### Command Mode

Press `Ctrl + X` to jump to the bottom command bar.

*   `w` : Save.
*   `q` : Quit.
*   `wq`: Save and Quit.
*   `q!`: Force Quit (discard changes).
*   `w <name>`: Save as new filename.

### Find & Replace (Regex)

In Command Mode (`Ctrl + X`), use standard regex syntax:

*   `r/pattern/replacement/` : Replace first occurrence on current line.
*   `r/old/new/g` : Replace **all** occurrences on current line.
*   `r/foo/bar/G` : Replace **all** occurrences in the **entire file** (Global).

## License

MIT License.
