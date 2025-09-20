# Mazu Editor

Mazu Editor is a minimalist text editor with syntax highlight, copy/paste, and search.

## Usage

Command line: (`filename` is optional)
* me `<filename>`

Supported keys:
* Ctrl-S: Save
* Ctrl-Q: Quit
* Ctrl-Z: Undo
* Ctrl-R: Redo
* Ctrl-F: Find string in file
    - ESC to cancel search, Enter to exit search, arrows to navigate
* Ctrl-O: Open file browser
    - Arrow keys to navigate files and directories
    - Enter to open file or enter directory
    - ESC or Ctrl-Q to cancel
* Ctrl-X: Start/stop text marking (selection mode)
    - Move cursor to select text while marking
    - ESC to cancel selection
* Ctrl-C: Copy marked text (or current line if no selection)
* Ctrl-K: Cut marked text (or cut from cursor to end of line)
* Ctrl-V: Paste copied/cut text
* PageUp, PageDown: Scroll up/down
* Up/Down/Left/Right: Move cursor
* Home/End: move cursor to the beginning/end of editing line

Mazu Editor does not depend on external library (not even curses). It uses fairly
standard VT100 (and similar terminals) escape sequences.

## Acknowledge

Mazu Editor was inspired by excellent tutorial [Build Your Own Text Editor](https://viewsourcecode.org/snaptoken/kilo/).

## License

Mazu Editor is freely redistributable under the BSD 2 clause license. Use of
this source code is governed by a BSD-style license that can be found in the
LICENSE file.
