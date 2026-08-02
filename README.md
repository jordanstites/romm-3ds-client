# romm-3ds-client

A Nintendo 3DS homebrew client for [RomM](https://github.com/rommapp/romm). Browse
your self-hosted library, download ROMs to the SD card, and sync save files
bidirectionally — including native 3DS save archives.

**Status:** working, actively developed, rough edges. Needs RomM 4.9.0+ for save
sync (developed against 5.1).

## What it does

- **Pairs with your server** by showing a code and a QR that you approve in RomM's
  web UI. Your password never reaches the 3DS.
- **Browses and downloads** your library, filtered by default to platforms a 3DS
  can play. Rows show whether a game is on the console and how many saves exist
  locally versus on the server.
- **Syncs saves for GB, GBC, GBA, SNES, Genesis and DS**, with the server deciding
  what to upload or download. Conflicts always ask rather than guessing, and every
  download writes a `.bak` first.
- **Syncs native 3DS saves**, both savedata and extdata, in a layout Azahar and
  other RomM clients can read.

It does **not install titles**. Downloaded `.3ds`/`.cia` files go to the SD card;
use [GodMode9](https://github.com/d0k3/GodMode9) (`.3ds` → "Build CIA from file")
or [FBI](https://github.com/Steveice10/FBI) (`.cia`) to install them.

## Installing

Copy `romm-3ds-client.3dsx` to `sdmc:/3ds/` and launch it from the Homebrew
Launcher.

On first run, set your server URL in Settings and follow the pairing prompt. If
the connection fails over HTTPS, check the console's clock before anything else —
a wrong date makes a valid certificate look expired, and it is by far the most
common cause.

## Using it

The bottom screen holds the toolbar: home, sync, search, queue, settings, log,
about.

| Screen | Keys |
|---|---|
| Platforms | **A** browse · **X** set this platform's SD folder |
| ROM list | **A** details · **L/R** page |
| ROM details | **X** link an installed 3DS title · **Y** upload its save · **L** restore its save |
| About | **A** list installed titles |

**Badges.** `DL` means the ROM file is on the SD card. `IN` means the game is
installed as a 3DS title, and `IN*` that you have confirmed which one.
`S<local>/<server>` counts saves, in gold when only one side has any.

**Platform folders.** Each platform maps to a folder under your ROM directory,
auto-detected when the folder name matches RomM's platform slug. The platform list
shows the mapping, in gold when unset. Without one, the app cannot tell what is
already on the card.

**Linking 3DS titles.** A 3DS save lives in an archive keyed by title ID, and RomM
stores no title ID, so the two are linked once per game: open the ROM's details and
press **X**, then confirm the installed title. Name matching only *suggests* —
you confirm, because restoring a save to the wrong title cannot be undone.

**Save slots.** A 3DS game may keep its save in savedata, in extdata, or both, so
each is synced separately and appears in RomM under its own slot, `3ds` or
`extdata`. Fantasy Life, for instance, uses only extdata.

## Building

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS
toolchain:

```sh
sudo dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib
brew install librsvg clang-format   # rsvg-convert for the icon; clang-format is enforced by CI
make                     # -> build/output/romm-3ds-client.3dsx
make run ADDR=<3DS IP>   # deploy over Wi-Fi (press Y in Homebrew Launcher first)
```

Run `make format` before committing; CI fails on violations.

`make cia` additionally needs `bannertool` and `makerom` on your `PATH`. On macOS
makerom has a native arm64 build and bannertool runs under Rosetta:

```sh
gh release download makerom-v0.19.0 --repo 3DSGuy/Project_CTR \
  --pattern 'makerom-v0.19.0-macos_arm64.zip'
gh release download v1.2.2 --repo Epicpkmn11/bannertool --pattern 'bannertool.zip'
# unzip both, then put makerom and mac-x86_64/bannertool on your PATH
```

## Attribution

Derived from [`derekprior/rommlet`](https://github.com/derekprior/rommlet) (MIT),
which provided the citro2d UI layer, download queue, SD folder browser, config
handling and build system. The original MIT license is preserved in
[LICENSE](LICENSE).

Substantially changed since: the HTTP transport, authentication, save sync in both
tiers, installed-title handling, and the status/browse UI.

## Third-party references

3DS save-archive handling is informed by reading
[Checkpoint](https://github.com/BernardoGiordano/Checkpoint) and 3hs (the hShop
client, distributed with its source; see <https://hshop.mariko.you>), both
**GPL-3.0**, with [3dbrew](https://www.3dbrew.org/) as the primary API reference.
This project is MIT, so those were read to learn which system calls matter and in
what order — no code was copied from either. See
[reference/3ds-save-archives.md](reference/3ds-save-archives.md).

MIT is GPL-compatible, so relicensing later remains possible if something
substantial is ever worth adopting; the reverse would not be.

Vendored: [cJSON](https://github.com/DaveGamble/cJSON) and
[nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator), both MIT.
