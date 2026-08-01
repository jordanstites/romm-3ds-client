# romm-3ds-client

A Nintendo 3DS homebrew client for [RomM](https://github.com/rommapp/romm). Browse your
self-hosted library, download ROMs to the SD card, and sync save files bidirectionally.

**Status:** early development. See [DESIGN.md](DESIGN.md) for the original scoping doc.

## Attribution

This project is derived from [`derekprior/rommlet`](https://github.com/derekprior/rommlet)
(MIT), which provided the citro2d UI layer, download queue, SD folder browser, config
handling, and build system. The original MIT license is preserved in [LICENSE](LICENSE).

Substantial changes from upstream are planned or in progress:

- HTTP transport moved from `httpc`/`sslc` to curl + mbedTLS, with TLS certificate
  verification **enabled** (the 3DS's native SSL module caps at TLS 1.1 and cannot talk to
  a modern server)
- Authentication moved from plaintext HTTP Basic to RomM's device authorization flow
- Bidirectional save sync via RomM's device sync protocol (`/api/sync/negotiate`)
- Transfers moved off the UI frame loop

## Building

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS toolchain:

```sh
sudo dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib
brew install librsvg     # rsvg-convert, for the SVG -> icon step
make                     # -> build/output/romm-3ds-client.3dsx
```

Copy the resulting `.3dsx` to `sdmc:/3ds/` and launch it from the Homebrew Launcher.

`make cia` additionally requires `bannertool` and `makerom` in `PATH`. The `.3dsx` build
does not — it uses devkitPro's own `smdhtool`.

## Installing downloaded 3DS titles

This app downloads files; it does not install them. Use
[GodMode9](https://github.com/d0k3/GodMode9) (`.3ds` → "Build CIA from file", then install)
or [FBI](https://github.com/Steveice10/FBI) (`.cia`).
