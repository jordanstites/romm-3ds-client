# 3DS RomM Client — Design & Scoping Doc

**Status:** scoping
**Goal:** A Nintendo 3DS homebrew app that authenticates against a self-hosted RomM
server over the public internet, browses the library, downloads ROMs to the SD card,
and syncs save files bidirectionally.

**Non-goals (for now):** netplay, achievements, in-app emulation, Switch/Wii U support.

---

## 1. Prior art — read this before writing any code

| Project | What it gives us |
|---|---|
| `derekprior/rommlet` (C, MIT, ~180 commits) | A working 3DS RomM client: platform/ROM browsing, search, download-to-SD with progress, download queue, per-platform folder config, touch toolbar UI, ships `.3dsx` + `.cia`. **Starting point.** |
| `Shalasere/SwitchRomM` (C++) | Same problem solved on Switch. Useful for config schema, download queue/badge state machine, FAT32-safe naming, resume logic. |
| `rommapp/grout` (Go) | First-party handheld client. Reference implementation of the device sync protocol — saves, states, play sessions. |
| `rommapp/argosy-launcher` (Kotlin) | First-party Android client. Reference for pairing flow + bidirectional save sync semantics. |
| Checkpoint / JKSM | The canonical way to read and write 3DS title save archives. Note: both are GPL — **read for API knowledge, do not copy code** unless we're prepared to relicense. |
| FBI (`Steveice10/FBI`) | The canonical way to install `.cia` via the AM services, including error handling and the ticket/title dance. |

**Decision gate — fork vs. contribute upstream to rommlet.** Evaluate against:

- [ ] Is the HTTP layer abstracted, or is `httpc` called inline from UI code? (Save sync needs
      uploads/multipart; if there's no request abstraction, that's a big refactor.)
- [ ] Is there a state/model layer separate from rendering, or is it immediate-mode all the way down?
- [ ] How is config handled? (We need to migrate `config.ini` off plaintext passwords.)
- [ ] Is the download path streaming to SD, or buffering in RAM?
- [ ] Does the maintainer respond to issues? Open one describing the save-sync plan before
      writing 3k lines.
- [ ] Is the CIA build (`make cia`) already wired for the extended service access we'll need?

If 4+ of these are healthy → contribute upstream in stacked PRs. If not → fork, keep the MIT
license and attribution, rename.

---

## 2. Networking & TLS — the load-bearing decision

Requirement: reachable over the public internet so other people can use it, not just LAN.

### The problem
The 3DS's SSL module ships an old root CA store that predates Let's Encrypt's ISRG roots.
Naive HTTPS to a modern cert fails the handshake. The scene's usual workaround is
`httpcSetSSLOpt(ctx, SSLCOPT_DisableVerify)`, which is **unacceptable here** — it turns every
public-network session into a free credential-interception opportunity.

### The fix
`libctru` exposes:

```c
Result httpcAddTrustedRootCA(httpcContext *context, const u8 *cert, u32 certsize);
Result httpcSetSSLOpt(httpcContext *context, u32 options);
```

Embed the root CA as a DER blob in romfs and register it per-context. Verification stays on.

**Implementation notes:**
- Ship a small bundle (ISRG Root X1 + a couple of common alternates), not a single hardcoded root —
  users' servers sit behind different issuers (Let's Encrypt direct, Cloudflare origin certs, ZeroSSL,
  self-signed homelab CAs).
- Allow a user-supplied root: `sdmc:/3ds/<app>/ca/*.der`, loaded at startup. This covers self-signed
  setups without a global "trust everything" switch.
- If verification fails, fail **loudly** with the specific reason. Never silently downgrade.

### Server-side compatibility checklist (Nginx Proxy Manager vhost)
- [ ] TLS 1.2 must remain enabled. TLS-1.3-only will not handshake. (Homebrew talks to GitHub over
      HTTPS today, so 1.2 with modern ECDHE-RSA-AES-GCM suites is known to work in practice.)
- [ ] Prefer an **RSA** certificate; test ECDSA separately before relying on it.
- [ ] Don't require SNI-less fallback; the 3DS does send SNI, but verify.
- [ ] HTTP/2 is fine (the client will negotiate 1.1).

**Action item:** before any app work, prove the handshake. Write a ~50-line 3dsx that does one
GET against the real server and prints the result code. Everything downstream depends on this.

---

## 3. Authentication

Do **not** use HTTP Basic Auth with a stored plaintext password (what rommlet does today).

RomM supports Client API Tokens with a device pairing flow: the server issues a short numeric
code (8 digits, ~5 minute TTL) that the device exchanges for a long-lived scoped token. Each user
can hold up to 25 tokens, and tokens carry a subset of the user's scopes.

**Flow:**
1. User enters server URL on the 3DS (soft keyboard, once).
2. App requests a pairing code, displays it large on the top screen.
3. User approves in the RomM web UI.
4. App exchanges for the token, stores it, shows a "paired" state.

**Storage:** token goes in a separate file from `config.ini`, not in the human-editable config.
It's still plaintext on an SD card (the 3DS has no keystore worth the name) — document that
honestly and scope the token to the minimum: `roms.read`, `assets.read`, `assets.write`,
`platforms.read`. No `users.*`, no `tasks.run`.

**Multi-user consideration:** since other people will connect, treat token revocation as a
first-class path — the app must handle 401 gracefully by clearing state and re-entering pairing,
not by crashing or retry-looping.

---

## 4. Platform tiers

The hard part isn't uniform. Split it.

### Tier 1 — GB / GBC / GBA / NDS (do this first)
- **Runs via:** TWiLight Menu++ + nds-bootstrap (DS), GBARunner2 or a native emulator (GBA), GameYob/others (GB).
- **ROM delivery:** plain file copy to SD. Already solved by rommlet.
- **Saves:** plain `.sav` / `.srm` files sitting on the SD card next to the ROM (or in a configured
  saves dir, depending on TWiLight config). Reading and writing them is ordinary `stdio`.
- **Verdict:** save sync here is a file-diff problem, not a 3DS-internals problem. Ship this first.

### Tier 2 — Native 3DS titles: installation
- `.cia` installs through the AM services (`AM_StartCiaInstall` / `AM_CancelCIAInstall` / finalize).
  Requires the app run with `am:net` access — that means the **CIA build with the right access
  descriptor in the RSF**, not a plain `.3dsx` under Homebrew Launcher.
- `.3ds` / `.cci` dumps **cannot** be installed directly. They need conversion to CIA.
  → Do this **server-side**, not on-device. A small sidecar container on the Unraid box that
  watches the RomM 3DS platform folder and produces `.cia` alongside. On-device conversion means
  reading a multi-GB file with ~64MB of RAM.
- FAT32 caps a single file at 4GB. Check `Content-Length` before starting and fail early with a
  clear message.

### Tier 3 — Native 3DS titles: saves (hardest)
- 3DS saves live in the **title's save data archive**, not as a file on the SD card. Access requires
  opening the save archive via the FS service with elevated permissions (again: CIA build), reading
  the archive as a small filesystem tree, and — critically — **committing** the archive after any
  write. Skipping the commit silently discards changes.
- Save data is a *directory tree*, but RomM stores a save as a *single file*. So: pack to zip/tar
  on-device (miniz is the pragmatic choice; rommlet already vendors cJSON, so vendoring a second
  small lib is consistent with the codebase).
- **Secure value:** some titles (notably Animal Crossing: New Leaf) use a save "secure value"
  that must be handled or the game reports a corrupted save. Look at how Checkpoint deals with it.
- **Restoring a save to the wrong title is destructive.** Gate restores behind a title-ID match plus
  an explicit confirm, and always take a local backup to SD before writing.

---

## 5. Save sync protocol

Model it on Grout / Argosy rather than inventing one — that way saves round-trip correctly between
the 3DS, the Steam Machine's RetroDECK, and anything else pointed at the same RomM.

**Core loop per ROM:**
1. Compute local save hash + mtime.
2. Fetch remote save metadata (hash, updated_at, emulator tag).
3. Compare:
   - identical hash → no-op
   - local newer, remote unchanged since last sync → upload
   - remote newer, local unchanged since last sync → download
   - **both changed → conflict**
4. Upload is multipart form (`saveFile`, optional screenshot).

**Conflict policy:** never auto-merge, never silently overwrite. Present both sides with timestamps
and sizes, let the user pick, and keep the loser as a local `.bak`. Save data is irreplaceable and
users will not forgive a silent clobber.

**Sync state:** persist a per-ROM record of `{last_synced_hash, last_synced_at}` locally.
Without it you cannot distinguish "remote changed" from "local never uploaded."

---

## 6. Milestones

**M0 — Spike (half a day).** Minimal 3dsx: TLS handshake against the real server with
`httpcAddTrustedRootCA`, one authenticated GET, print the result. Go/no-go for the whole design.

**M1 — Evaluate rommlet.** Work the decision-gate checklist in §1. Output: fork-or-contribute call,
written down.

**M2 — Auth rewrite.** Pairing flow + token storage + 401 handling. Removes the plaintext password.
Smallest self-contained user-visible win, and a clean first upstream PR.

**M3 — Tier 1 save sync.** Detect save paths for TWiLight/GBARunner layouts, hash, compare,
up/download, conflict UI. This is the feature you actually want.

**M4 — CIA install.** RSF/access-descriptor work, AM install with progress, `.3ds` handling
(reject with a clear message pointing at the server-side converter).

**M5 — Server-side `.3ds` → `.cia` sidecar.** Container for Unraid, watches the RomM library.
Independent of the 3DS work; can be built in parallel and is a good Go project.

**M6 — Tier 3 saves.** Save archive read/write, zip packing, secure-value handling, title-ID
guardrails, mandatory local backup before restore.

---

## 7. Risks

| Risk | Mitigation |
|---|---|
| TLS handshake can't be made to work at all with the target cert chain | M0 spike answers this before anything is built. Fallback: WireGuard-only, or a dedicated vhost with a compatible chain. |
| Save corruption on restore | Title-ID match, explicit confirm, mandatory local `.bak`, commit-after-write. Test on a title you don't care about. |
| Upstream maintainer doesn't want save sync | Decide fork-vs-contribute at M1, before sinking effort into a PR shape that gets rejected. |
| GPL contamination from reading Checkpoint/JKSM | Read for API understanding (3dbrew is the primary reference), write original code. Document that you did. |
| Multi-user exposure | Keep RomM's `DISABLE_DOWNLOAD_ENDPOINT_AUTH` off, keep Fail2ban on the vhost, scope tokens minimally, never disable cert verification. |
| 4GB FAT32 / low RAM | Stream to SD in chunks always; check `Content-Length` up front; HTTP Range for resume. |

---

## 8. Open questions

- Does rommlet's download path already stream, or does it buffer? (Determines M3 effort.)
- Which RomM version is the server on? Pairing flow and save endpoints have version floors.
- Do you want other users on *your* RomM instance, or do you want the app to work against
  *their own* instances? (Very different threat models — the second is just "ship a good client,"
  the first means you're operating a service.)
- Emulator tag convention for saves uploaded from the 3DS — needs to match what RetroDECK writes
  so saves round-trip to the Steam Machine.