# Branding

- `DesertLogo.png` — the logo (sand), owner's choice 2026-09-24. `DesertLogoMono.png` — the monochrome variant
  for single-colour places.
- `DesertIcon1024.png` — the logo padded to a square; the macOS Dock icon (set by the splash, `kAppIcon`).
- `Desert.icns` / `Desert.ico` — built from `DesertIcon1024.png` (16…1024 px / 16…256 px). `Editor.rc` embeds
  the `.ico` as `GLFW_ICON`, which Windows uses for the `.exe` and GLFW for every window.

Regenerate after changing the logo: `sips --padToHeightWidth` to a square, `sips -z` per size, `iconutil -c icns`
for macOS, and pack the PNG sizes into an `.ico` (PNG-in-ICO entries).
