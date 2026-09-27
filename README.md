# TouchDraw — your touchpad as an absolute drawing tablet

Write and sketch math directly with your laptop's built-in touchpad — no drawing tablet, no stylus.
TouchDraw treats the touchpad as a 1:1 miniature of the canvas: touch the top-right of the pad,
ink appears at the top-right of the canvas. Lift your finger and put it down elsewhere and the
ink jumps there (true absolute mapping, like a graphics tablet — not relative cursor motion).

Built on [`ichisadashioko/windows-touchpad`](https://github.com/ichisadashioko/windows-touchpad),
which solved the hard part: reading absolute finger coordinates from Windows Precision Touchpad
(PTP) devices via raw HID input instead of standard mouse APIs.

## Run it

**Requirements:** Windows 10/11 laptop with a **Precision Touchpad** (most laptops since ~2017).
The app prints a warning at startup if no compatible touchpad is found.

- **Easiest:** double-click `TouchDraw.exe` (no console window), or use the desktop shortcut.
- Or install the MSIX package (see below) to get a Start Menu entry and clean uninstall.
- Or run `touchdraw.exe` (console build) if you want the debug log output.

Press **F3**, draw with one finger on the touchpad, **ESC** to get your cursor back.

## Controls

| Action | Mouse / toolbar | Keyboard |
|---|---|---|
| Enter / exit draw mode | mode indicator (top-right) | `F3` / `ESC` |
| Pen / eraser | Pen, Eraser buttons | `E` |
| Colors | 5 swatches + Custom (native color picker) | `1`–`5` |
| Brush size (1–64px, eraser follows it) | A− / A+ | `[` / `]` |
| Undo (64 steps) | Undo | `Ctrl+Z` |
| Redo | Redo | `Ctrl+Y` or `Ctrl+Shift+Z` |
| Clear canvas (undoable) | Clear | `C` |
| Save drawing (`.tdraw`) | Save | `Ctrl+S` |
| Open drawing (`.tdraw`) | Open | `Ctrl+O` |
| Export picture (`.bmp`) | BMP | — |
| Quit | window X | `Q` |

While in draw mode the cursor is hidden and confined to the TouchDraw window
(`ClipCursor`), so touchpad motion can't click other apps — but the toolbar stays
clickable. Files:

- `.tdraw` — native format (strokes + colors + widths), reopens fully editable.
- `.bmp` — 24-bit export of exactly what's on the canvas.

## How the mapping works

1. At startup the app enumerates HID devices and reads the touchpad's report
   descriptor: per-contact link collections give **logical** X/Y ranges
   (`HID_USAGE_PAGE_GENERIC` X/Y, absolute only) plus tip-switch / contact-ID /
   contact-count usages.
2. Each `WM_INPUT` report is decoded with `HidP_GetUsageValue` / `HidP_GetUsages`
   into down / move / up touch events, tracked by contact ID (single-finger ink).
3. Raw logical coordinates are normalized against the descriptor's
   **LogicalMin/LogicalMax** (not the physical extent, which is in units like
   hundredths of a millimeter), then scaled letterboxed into the canvas —
   aspect ratio preserved, no stretch. Window resizes re-map live; strokes are
   stored in pad units so nothing is lost.
4. The process is per-monitor DPI aware, so mapping is in true physical pixels.

## Build from source

- Toolchain: MSVC (Build Tools 2022 work) + Windows 10 SDK. No extra dependencies.
- `touchdraw/build.bat console touchdraw.exe` — console build (debug log).
- `touchdraw/build.bat windows TouchDraw.exe` — GUI build (no console).
- Or open `touchpad/touchpad.vcxproj` in Visual Studio (`Debug|Win32`).
- Headless logic tests (undo/redo, hit-test, file round-trip — no GUI needed):
  compile `touchdraw/test_logic.c` with the `touchpad/*.c` sources and run it.

## Project layout

```
touchpad/
  main.c         app window, toolbar, mapping, input event flow (WM_INPUT consumer)
  touchpad.c/h   raw-input HID helpers (device list, preparsed data, reports)
  utils.c/h      HID capability table (link collections, X/Y ranges)
  touchevents.c/h  down/move/up state machine keyed by contact ID
  point2d.c/h    capacity-doubled point arrays
  stroke.c/h     Stroke model (points + color + width), hit-testing
  history.c/h    snapshot undo/redo (64 steps)
  fileio.c/h     .tdraw save/open, .bmp export, file dialogs
```

The raw HID parsing (`touchpad.c`, `utils.c`, the `WM_INPUT` block in `main.c`)
is the fragile, hard-won part — everything else is built around it without
changing it.

## License

Same as upstream (`LICENSE`).
