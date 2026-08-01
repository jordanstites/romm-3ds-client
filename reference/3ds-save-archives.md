# 3DS save archives — API notes for Phase 5

Working notes for syncing native 3DS title saves. Everything here is a fact
about the **system API**, not code to copy.

## Licensing — read this first

[Checkpoint](https://github.com/BernardoGiordano/Checkpoint) and JKSM are the
canonical implementations, and both are **GPL-3.0** — Checkpoint additionally
invokes GPLv3 terms 7.b and 7.c, which require preserving author attributions.

**This project is MIT.** Copying any code from either would force the entire
project to GPLv3. So: read them to learn *which* system calls matter and *in
what order*, cross-check against [3dbrew](https://www.3dbrew.org/) as the
primary reference, and write original code. The call sequences below are public
system API, documented on 3dbrew; the value of reading Checkpoint is knowing
which of them are load-bearing.

## Enumerating installed titles

SD-installed titles — the common case here, since the target console has its
games installed rather than on cartridges:

```c
u32 count = 0;
AM_GetTitleCount(MEDIATYPE_SD, &count);
u64 *ids = malloc(count * sizeof(u64));
AM_GetTitleList(NULL, MEDIATYPE_SD, count, ids);
```

`MEDIATYPE_GAME_CARD` works the same way but returns at most one title.
`MEDIATYPE_NAND` covers system titles and DSiWare.

`amInit()` acquires `am:net`, which Luma3DS grants to `.3dsx` homebrew — so no
CIA build is required just to enumerate.

A title ID splits into `highid = id >> 32` and `lowid = id & 0xFFFFFFFF`. The
**unique ID** is `(lowid >> 8) & 0xFFFFF`, needed for the secure value below.

`AM_GetTitleProductCode(media, id, code)` gives the product code (e.g.
`CTR-P-...`), useful for display and for matching against a RomM entry.

## Opening another title's save archive

`ARCHIVE_SAVEDATA` resolves the save from the *calling process's own* exheader,
so it only ever opens our own save. To open someone else's, use
`ARCHIVE_USER_SAVEDATA` with an explicit binary path:

```c
const u32 path[3] = {mediatype, lowid, highid};
FSUSER_OpenArchive(&archive, ARCHIVE_USER_SAVEDATA, (FS_Path){PATH_BINARY, 12, path});
```

NAND/system titles use a different archive and a 2-word path:

```c
const u32 path[2] = {mediatype, (0x00020000 | lowid >> 8)};
FSUSER_OpenArchive(&archive, ARCHIVE_SYSTEM_SAVEDATA, (FS_Path){PATH_BINARY, 8, path});
```

Extdata is `ARCHIVE_EXTDATA`, also a 3-word binary path. **Back up savedata, not
extdata** — a game's actual save lives in savedata; extdata holds StreetPass,
Festival Plaza and mystery-gift style content. Note the extdata ID is usually
`lowid >> 8` but there are hardcoded exceptions (Pokémon Y, Omega Ruby, Moon,
Ultra Moon, several Fire Emblem SKUs), which is why Checkpoint keeps a quirks
table.

Requires `fs:USER` with `use_extended_savedata_access` and broad
`fs_access_info`. Luma's 3dsx loader sets `fs_access_info = 0xFFFFFFFF` and
`use_extended_savedata_access = true`, so `.3dsx` is sufficient.

## Writing a save back — both steps are mandatory

```c
FSUSER_ControlArchive(archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);

u8 out;
u64 secureValue = ((u64)SECUREVALUE_SLOT_SD << 32) | (uniqueId << 8);
FSUSER_ControlSecureSave(SECURESAVE_ACTION_DELETE, &secureValue, 8, &out, 1);
```

1. **Without the commit, writes are silently discarded.** The save archive is
   journalled; uncommitted changes are dropped. No error is reported.
2. **The secure value is anti-rollback**, added in firmware 4.0.0-7. A 64-bit
   value that increments on each save; on launch the game compares its stored
   value against the system's and treats a mismatch as a restored backup.
   Ignoring it means the game reports a **corrupted save** — Pokémon titles in
   particular will refuse or delete it.

   The working approach is to **delete** the secure value rather than set it, so
   the system forgets the expected value and the game regenerates it on next
   launch.

TWL (DS Virtual Console) saves need neither step.

## Shape mismatch with RomM

A 3DS save is a **directory tree**; RomM stores a save as a **single file**. So
the on-device side has to pack to zip before upload and unpack after download.
`minizip` is already linked for the ROM-extraction path, so this costs nothing
new.

Note that RomM computes `content_hash` for a zip as a *composite* — MD5 of
`"name:md5"` lines for each entry, sorted by name, joined with `\n`, then MD5'd
— not MD5 of the zip bytes. See `_compute_zip_hash` in
`backend/handler/filesystem/assets_handler.py`. Tier 1 saves are plain files and
use plain MD5; 3DS saves will need the composite form to match.

## Guardrails before any of this touches a real save

- Match the title ID before restoring; restoring to the wrong title is
  destructive and unrecoverable.
- Require an explicit confirmation.
- Always write a local `.bak` first.
- Test on a title you do not care about before pointing it at Pokémon Sun.
