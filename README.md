# crossjp

EPUB and plain-text reader for the Xteink X4 Classic. Books are `.epub` and `.txt` files on the SD card. This firmware typesets them. It does not read `.xtch` or `.xtc`.

The panel is 800×480 landscape. UI and pages are 480×800 portrait.

## Layout

```
src/main.cpp          boot + loop
src/core/             screen stack, input, settings, power, list UI
src/screens/          home, browser, reader, chapters, settings, update
src/platform/         chip workarounds
lib/hal               hardware wrappers (still CrossPoint-shaped)
lib/Gfx               portrait framebuffer + UI text
lib/Typesetter        EPUB and UTF-8 text layout
lib/XgfFont           on-device TrueType rasterizer
lib/EpdFont           Ubuntu UI fonts
reference/            CrossPoint submodule (lookup only)
freeink-sdk/          display and board support
```

Settings and per-book progress live in `/.crossjp` on the SD card.
Boot and runtime logs go to the Serial console only (no SD log file).

Firmware update from SD (same idea as CrossPoint):

- Settings → **Update firmware** → pick a `.bin` → confirm.
- Recovery picker: hold **Down + Power** at wake (side-right + power). Side-left is GPIO0, a boot strap. Back returns to home.

Reading fonts are TrueType files (`.ttf`, `.otf`, `.ttc`) under `/.crossjp/fonts/`. The device rasterizes them. The FreeType build has no CFF driver, so a CFF OpenType such as Hiragino Mincho will not open. Use a TrueType or TrueType-collection face. Face index 0 of a `.ttc` is used.

The stock Xteink updater is gone once this firmware is installed.

## Build

One env, `x4c`: ESP32-S3, 16 MB flash, 8 MB octal PSRAM, 1-bit SDMMC.

```bash
pio run              # env:x4c
pio run -e x4c -t upload
pio device monitor
```

## Release

Bump `[crossjp] version` in `platformio.ini` to match the tag, push `develop`, then:

```bash
git tag -a v0.1.0 -m "v0.1.0"
git push origin v0.1.0
```

GitHub Actions builds `crossjp-x4c.bin` and attaches it to a GitHub Release for that tag.

Optional `platformio.local.ini` for a local upload port (gitignored).

`reference/` is the CrossPoint Reader tree. Do not build it as this product.

```bash
git submodule update --init --recursive
```
