# Tera Term
## Hex-View plugins
HexView plugins for Tera Term can show a live hex dump of the raw bytes received on the current
connection (TCP or serial), in a separate "Hex View" window next to the main
terminal window. Toggle it from **File > Hex View**.

- The Hex View window stays docked beside the main window: moving or
  resizing either window keeps the other aligned. Its width can be adjusted
  by dragging its border, and it resizes vertically together with the main
  window.
- A toolbar lets you choose how many bytes are shown per row (4, 8, 16, or
  32 bytes); your last choice is remembered across restarts, and the window
  automatically resizes so the offset, hex, and ASCII columns are never
  clipped.
- Selecting text in the terminal highlights the matching bytes in the Hex
  View - picking the correct occurrence even if the same text appears more
  than once in the retained history - and scrolling the terminal scrolls
  the Hex View to follow it.
- The offset, hex, and ASCII columns each use a distinct color for easy
  reading, and automatically match Windows' light/dark app theme.
- Hex View text can be selected and copied like any other text.

Only received data is shown (not what you type/send), matching exactly what
appears in the terminal: NUL bytes are omitted, since Tera Term itself never
displays them either.

### Screenshot:
<img width="2912" height="766" alt="image" src="https://github.com/user-attachments/assets/5ddd1c8c-9d00-4cfe-9885-88730939d718" />

## CI/CD

| CI | Workflow | Status |
|----|----------|--------|
| GitHub Actions | Build Installer | [![Build Status](https://github.com/TeraTermProject/teraterm/actions/workflows/msbuild.yml/badge.svg)](https://github.com/TeraTermProject/teraterm/actions/workflows/msbuild.yml) |
| GitHub Actions | Bulid with cmake (experimental) |[![Bulid with cmake (experimental)](https://github.com/TeraTermProject/teraterm/actions/workflows/build_cmake.yml/badge.svg)](https://github.com/TeraTermProject/teraterm/actions/workflows/build_cmake.yml) |

Tera Term is a free software terminal emulator

[URLs](https://github.com/TeraTermProject/teraterm/wiki/Urls)

## About Signed Binaries

We thank [SignPath.io](https://signpath.io) for providing a free code signing service, and the [SignPath Foundation](https://signpath.org/) for providing a certificate for free code signing.

