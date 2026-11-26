# Branding

- `DesertLogo.png` — the logo (sand), owner's choice 2026-09-24. `DesertLogoMono.png` — the monochrome variant
  for single-colour places.
- `DesertIcon1024.png` — the logo padded to a square; the macOS Dock icon (set by the splash, `kAppIcon`).
- `Desert.icns` / `Desert.ico` — built from `DesertIcon1024.png` (16…1024 px / 16…256 px). `Editor.rc` embeds
  the `.ico` as `GLFW_ICON`, which Windows uses for the `.exe` and GLFW for every window.

Regenerate after changing the logo: `sips --padToHeightWidth` to a square, `sips -z` per size, `iconutil -c icns`
for macOS, and pack the PNG sizes into an `.ico` (PNG-in-ICO entries).

## Studio

`Studio/` holds the Desert Studio mark (the studio, not the engine; the files above stay the engine's icon).
- `DesertStudioMark.svg` — the mark, white: a D whose stem is cut from the bowl by a slit. `DesertStudioMarkBlack.svg`
  — the same in black. `DesertStudioMark.png` — 1024 px raster, white on transparent.
- `DesertStudioLogo.svg` / `.png` — the lockup: the mark over the wordmark `DESERT STUDIO`.

The geometry has one source: the mark and the letters are analytic shapes; the SVG paths and the PNGs are written
from the same numbers that draw the studio ident (the game's `Content/Movies/DesertStudio.webm`).
