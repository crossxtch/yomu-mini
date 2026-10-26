# yomu

EPUB and plain-text reader for the Xteink X4 Classic. This checkout is `yomu-mini`. On the device the name is yomu. Books are `.epub` and `.txt` files on the SD card. This firmware typesets them. It does not read `.xtch` or `.xtc`.

The panel is 800×480 landscape. UI and pages are 480×800 portrait. Book opening and page layout come from the sibling `typeset` repo.

## Layout

```
src/main.cpp          boot + loop
src/core/             screen stack, input, settings, power, list UI
src/screens/          home, browser, reader, chapters, settings, update
src/platform/         chip workarounds
lib/hal               hardware wrappers (still CrossPoint-shaped)
lib/Gfx               portrait framebuffer + UI text
lib/Book              book session, 2-bit paint, and the picture sidecar
lib/BookFace          reading face used by books and lists
lib/ReadFace          TrueType glyphs cached as 2-bit em boxes
lib/EpdFont           Ubuntu UI fonts
reference/            CrossPoint submodule (lookup only)
freeink-sdk/          display and board support
```

Settings, fonts, and per-book progress live in `/.yomu` on the SD card. A card that still has `/.crossjp` is renamed to `/.yomu` on boot.
Boot and runtime logs go to the Serial console only (no SD log file).

Firmware update from SD (same idea as CrossPoint):

- Settings → **Update firmware** → pick a `.bin` → confirm.
- Recovery picker: hold **Down + Power** at wake (side-right + power). Side-left is GPIO0, a boot strap. Back returns to home.

Reading needs a TrueType file. Put a `.ttf`, `.otf`, or `.ttc` in `/.yomu/fonts/` and select it. There is no built-in reading face. The FreeType build has no CFF driver, so a CFF OpenType such as Hiragino Mincho will not open. Use a TrueType or TrueType-collection face. Face index 0 of a `.ttc` is used. Horizontal Latin uses the full em cell. Vertical punctuation uses the face's vertical metrics when it has them.

The stock Xteink updater is gone once this firmware is installed.

## Build

One env, `x4c`: ESP32-S3, 16 MB flash, 8 MB octal PSRAM, 1-bit SDMMC.

```bash
pio run              # env:x4c
pio run -e x4c -t upload
pio device monitor
```

## Release

Bump `[yomu] version` in `platformio.ini` to match the tag, push `develop`, then:

```bash
git tag -a v0.1.0 -m "v0.1.0"
git push origin v0.1.0
```

GitHub Actions builds `yomu-x4c.bin` and attaches it to a GitHub Release for that tag.

Optional `platformio.local.ini` for a local upload port (gitignored).

`reference/` is the CrossPoint Reader tree. Do not build it as this product.

```bash
git submodule update --init --recursive
```
