TTXHexView -- Hex view panel of raw send/receive traffic

Adds a "Hex &View" toggle to the File menu. When enabled, a panel is
docked inside the main terminal window, on the right side, displaying
a live hex dump of the bytes sent/received over the current connection
(TCP or serial) -- i.e. exactly the same data shown in the terminal,
just formatted as hex.

The panel:
- Is docked inside the main window (the terminal view shrinks to make
  room for it), not a separate floating window.
- Can be resized by dragging the thin handle on its left edge.
- Has a small toolbar with "4 bytes" / "8 bytes" / "32 bytes" buttons
  to change how many bytes are grouped per hex-dump line.
- Toggle it off from the File menu to give the full window back to the
  terminal.
