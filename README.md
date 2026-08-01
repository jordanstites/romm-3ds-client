# romm-3ds-client

A Nintendo 3DS homebrew client for [RomM](https://github.com/rommapp/romm). Browse
your self-hosted library, download ROMs to the SD card, and sync save files
bidirectionally — including native 3DS save archives.

**Status:** working, actively developed, rough edges. Requires RomM 4.9.0+ for
save sync (developed against 5.1).

## What works

- **Pairing** via RomM's device authorization flow. The console shows a code and
  a QR; you approve in RomM's web UI. Your account password never reaches the
  3DS, and the stored token is scoped to `roms.read`, `platforms.read`,
  `assets.read`, `assets.write`, `devices.read`, `devices.write` and `me.read`.
- **Browsing and downloading**, filtered by default to platforms a 3DS can
  actually play. Each row shows whether the game is on the console and how many
  saves exist locally versus on the server.
- **Save sync for GB, GBC, GBA, SNES, Genesis and DS** through RomM's device
  sync protocol (`/api/sync/negotiate`). The server decides what to upload,
  download or flag as a conflict; conflicts always ask rather than guessing, and
  every download writes a `.bak` first.
- **Native 3DS save archives** — export a title's save to RomM, or restore one
  onto the console. Restore backs up the existing save first and refuses to
  proceed if that backup fails.

## What does not

- **Installing titles.** The app downloads `.3ds`/`.cia` files to the SD card;
  use [GodMode9](https://github.com/d0k3/GodMode9) (`.3ds` → "Build CIA from
  file") or [FBI](https://github.com/Steveice10/FBI) (`.cia`) to install them.
- **Physical cartridge saves.** 3DS carts would need `MEDIATYPE_GAME_CARD` (a
  small addition); DS carts need cartridge SPI access (a much larger one).
- **Background transfers.** Downloads block the UI, though they no longer block
  on vsync.

## Using it

Copy `romm-3ds-client.3dsx` to `sdmc:/3ds/` and launch it from the Homebrew
Launcher. On first run, set your server URL in Settings, then follow the pairing
prompt.

**Bottom-screen toolbar** — home, sync, search, queue, settings, log, about.

| Screen | Keys |
|---|---|
| Platforms | **A** browse · **X** set this platform's SD folder |
| ROM list | **A** details · **L/R** page |
| ROM details | **X** link an installed 3DS title · **Y** upload its save · **L** restore its save |
| About | **A** list installed titles |

**Badges in the ROM list.** `DL` means the ROM file is on the SD card. `IN` means
the game is installed as a 3DS title, and `IN*` means you have confirmed which
installed title it is. `S<local>/<server>` counts saves — gold when only one side
has one, so a sync would do something.

**Platform folders.** Each platform maps to a folder under your ROM directory,
auto-detected when the folder name matches RomM's platform slug. The platform
list shows the mapping, in gold when unset. Without one, the app cannot tell what
is already on the card.

### Native 3DS saves

3DS saves live in a save archive keyed by title ID, and RomM stores no title ID,
so the two have to be linked once per game: open the ROM's details and press
**X**, then confirm the installed title. Name matching only *suggests* — you
confirm, because restoring a save to the wrong title is unrecoverable.

Saves are stored as a zip of the archive's contents with entries relative to the
save root (`main`, not `<titleId>/main`), which is what lets the same file be
used by an emulator.


## Building

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS
toolchain:

```sh
sudo dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-zlib
brew install librsvg clang-format   # rsvg-convert for the icon; clang-format is enforced by CI
make                     # -> build/output/romm-3ds-client.3dsx
make run ADDR=<3DS IP>   # deploy over Wi-Fi via 3dslink (press Y in Homebrew Launcher first)
```

Run `make format` before committing — CI runs `make format-check` and fails on
violations.

`make cia` additionally needs `bannertool` and `makerom` in `PATH`; the `.3dsx`
build does not, since it uses devkitPro's own `smdhtool`.

## Notes for anyone reading the code

- **TLS.** The 3DS's native `ssl:C` module tops out at TLS 1.1 and its root store
  has no ISRG anchor, so `httpcAddTrustedRootCA` cannot rescue it against a
  modern server. The transport is curl + mbedTLS with verification **on**, using
  the CA bundle Luma3DS ships at `sdmc:/config/ssl/cacert.pem`. Note mbedTLS
  validates certificate dates, which the native module does not — a wrong console
  clock will look like a TLS failure.
- **Hashing.** RomM's `content_hash` is MD5 of the file's bytes, despite its docs
  implying sha1. For a zip it is instead a composite: MD5 of `<name>:<md5>` lines
  sorted by name and joined with newlines.
- **Auth failures are 403, not 401.** Handling only 401 would miss a revoked
  token entirely.
- **Slots.** A save sent without a `slot` bypasses the server's pairing logic and
  is treated as an unconditional upload, silently disabling conflict detection.
- **minizip needs ~64KB of stack** in `zipOpen3`, far more than the main thread
  has, so zip work runs on a dedicated 192KB thread.

- **Redirects are followed by hand.** curl strips `CURLOPT_USERPWD` on a
  cross-host redirect but not custom headers, so with `CURLOPT_FOLLOWLOCATION`
  the bearer token would be sent to whatever host a redirect names — and RomM
  does redirect, to S3 or through a proxy. Each hop is re-issued explicitly and
  the token is attached only when the target origin matches the configured
  server, compared including scheme and port.

## Attribution

Derived from [`derekprior/rommlet`](https://github.com/derekprior/rommlet) (MIT),
which provided the citro2d UI layer, download queue, SD folder browser, config
handling and build system. The original MIT license is preserved in
[LICENSE](LICENSE).

Substantially changed since: the HTTP transport, authentication, save sync in
both tiers, installed-title handling, and the status/browse UI.

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

[DESIGN.md](DESIGN.md) is the original scoping document. Several of its
assumptions turned out to be wrong — see the notes above — but the milestone
structure still broadly holds.
