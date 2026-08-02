# Backlog

Work that is deliberately not being done yet, and why. Not a roadmap — things
leave this list when there is a reason to pick them up.

## Installing `.3ds` cartridge images

**Status:** the conversion is written and unit-tested ([convert.c](source/convert.c),
[ciabuild.c](source/ciabuild.c)); nothing calls it.

Installing a `.cia` already works end to end, so anyone whose library is CIAs can
download and install without leaving the app. This would extend that to `.3ds`
dumps, which currently need [GodMode9](https://github.com/d0k3/GodMode9) — its
"install game image" does the same repackaging behind a different button.

Deferred because it is convenience rather than capability, and the trade is not
obviously worth it: new code registering titles with AM, against a tool with
years of use behind it. Saving an app-switch is a smaller win than the risk is a
loss.

Remaining work is the flow, not the format: download to SD, then convert and
install, with progress across both passes. Both passes are already reported
against one total so a progress bar does not reset halfway.

## Save slots for Tier 1 platforms

GB/GBC/GBA/SNES/Genesis/DS saves sync on numeric slots (`0`, `1`, …) because the
slot doubles as the TWiLight Menu++ filename suffix — slot 1 means `.sav1`.
Argosy uses named channels, so the two never pair for these platforms.

Fixing it means decoupling the wire slot from the on-disk suffix. Native 3DS
saves already carry names (`3ds`, `extdata`) because nothing on disk depends on
them.

## Extdata layout for emulators

Native savedata is written inside the container Azahar keeps it in, so a save
moves between console and emulator. Extdata is written relative to its own root
instead, because there is no verified sample of what Azahar writes for it.

Low priority: Argosy does not read extdata at all — its 3DS handler resolves
saves only under `title/<high>/<low>/data`, and the string "extdata" does not
appear in its source. So there is no client on the other side to match. Worth
revisiting if that changes.

## Background downloads

The queue processes items in a loop rather than driving the worker across them,
so a queued batch runs one at a time with the UI waiting on each.

## Untested paths

Both are destructive and have never run on hardware:

- **Native save restore through sync.** Downloads used to be written to a file
  and discarded, so this direction never actually wrote to a save archive. It
  now does, backing the existing save up first and refusing to continue if that
  backup fails.
- **Convert and install**, once wired up.

Try each on a title you do not care about first.
