# Nexus Submission Review Notes

This document gives Raidcore Nexus reviewers a source-backed summary of VfxD Visual Sins Updater's behavior, with file/line references so each claim can be checked directly against the source. It describes addon version `0.10.1.0` ([`src/addon/version.h:7-10`](src/addon/version.h#L7-L10)), built against vendored Nexus API version `6` ([`include/Nexus.h:19`](include/Nexus.h#L19)). **Note for reviewers:** the `Rev` component auto-increments on every build (`bump_rev.py`, run as a pre-build step), so the exact fourth number will already be stale by the time this is read — treat `0.10.1.x` as the relevant baseline, not the literal `.0`.

**Listing status:** Not yet submitted / pre-review. This document is written in preparation for initial Nexus Addon Library submission.

## Reviewer summary

| Review item | Implementation | Where to check |
|---|---|---|
| Reverse-engineered code | **None.** No disassembly-derived game structures/offsets/signatures. Game state is read via Nexus's own public `DataLink_Get` API against the officially-documented Mumble Link and RTAPI structures, not by scanning game memory. | [`src/core/game_state.cpp:30-32`](src/core/game_state.cpp#L30-L32) |
| First-party source availability | Nearly all project-owned code is present in this repository. **One exception**: the real webhook relay URL (`src/integration/webhook_config.h`) is git-ignored and generated locally per build from a private input — see [Network surface](#network-surface) and [Reverse engineering and source policy](#reverse-engineering-and-source-policy) below for exactly what that is and isn't. | [`.gitignore:8`](.gitignore#L8), [`src/integration/webhook_config.example.h`](src/integration/webhook_config.example.h) |
| Game-process access | Uses the public Nexus `AddonAPI` only: `DataLink_Get`, `GUI_Register`/`Deregister`, `Events_Subscribe`/`Unsubscribe`/`RaiseNotification`, `Paths_GetAddonDirectory`, `Log`. No direct game-memory read/write outside the DataLink pointers Nexus itself hands back, no signature scanning, no detouring/hooking, no packet interception, no native GW2 UI hook. | [`src/core/game_state.cpp:25-33`](src/core/game_state.cpp#L25-L33), full call-site list in [Nexus integration surface](#nexus-integration-surface) below |
| Gameplay automation | **None.** No `SendInput`/`keybd_event`/`mouse_event`/`PostMessage`/`SetCursorPos` and no `WriteProcessMemory`/`ReadProcessMemory`/`CreateRemoteThread`/`VirtualAllocEx` anywhere in project code (verified by full-project grep). The addon does not simulate input, use `InputBinds`, move the character, or touch any inventory/trade UI. | n/a — absence verified across `src/` |
| What it actually does | Installs, diffs, updates, hand-edits, backs up, and rolls back local VfxDenoiser JSON config files (the "Visual Sins" effect collection and other VfxD-style files). It does not render or apply visual effects itself — that is VfxDenoiser's job. | [`src/core/sin_files.h:8-16`](src/core/sin_files.h#L8-L16), [`src/core/merge.cpp:1-18`](src/core/merge.cpp#L1-L18) |
| Addon-owned network traffic | HTTPS only, three destinations: GitHub's release API, a GitHub release-asset redirect target, and one Cloudflare Worker relay for the optional community effect-report feature (see below). Full breakdown in [Network surface](#network-surface). | [`src/integration/github_update.cpp:373`](src/integration/github_update.cpp#L373), [`src/integration/webhook_report.cpp:379`](src/integration/webhook_report.cpp#L379) |
| Telemetry and advertising | No analytics or crash-reporting SDK, no ads. The only outbound data beyond the update check is the user-composed, user-triggered effect report described below — never sent automatically. | [`src/ui/report_ui.cpp:404`](src/ui/report_ui.cpp#L404) (explicit "Send" action, opt-in anonymity checkbox) |
| Local secret storage | No account credentials of any kind are stored — this addon never touches the Guild Wars 2 API key. The only semi-sensitive local value is the report relay's obfuscated destination URL, embedded at build time (not written to disk at runtime). | [`src/integration/webhook_report.cpp:154,164-169`](src/integration/webhook_report.cpp#L154) |
| Local persistent storage (new since `0.9.3.0`) | An opt-in, off-by-default "for science" toggle writes a local SQLite file (`vfxd_effect_db.sqlite3`) next to VfxDenoiser's own JSON files, capturing self-cast VFX effect data (guid, timing/behavior fields, and the local player's own profession/race/specialization) for the developer's own analysis. Never enabled automatically; requires a hand-created `VfxD_Greed.json` marker file to even appear as an option (deliberate friction). Nothing in this store is ever transmitted — see [Network surface](#network-surface). | [`src/core/effect_db.h:226,240-254`](src/core/effect_db.h#L226), [`src/ui/live_log_ui.cpp:365-381`](src/ui/live_log_ui.cpp#L365-L381) |
| Updates | Declares the Nexus GitHub update provider for the addon DLL itself. Nexus handles addon-binary update checks/installation. Separately (and unrelated to Nexus's own updater), this addon's *own* code also GETs a *different* GitHub repo's releases to update the Visual Sins JSON content files it manages — see [Network surface](#network-surface). | [`src/addon/entry.cpp:146-147`](src/addon/entry.cpp#L146-L147) (Nexus provider), [`src/integration/github_update.cpp:134-135`](src/integration/github_update.cpp#L134-L135) (separate content-update repo) |
| AI assistance | Heavy AI assistance (Claude, Anthropic) was used throughout development — architecture, refactoring, bug hunting, and documentation. Disclosed for Raidcore's **AI Notice** category; the developer remains responsible for all review, testing, maintenance, and compliance. | [`README.md:3-4`](README.md#L3-L4) |

## What the addon does

1. On load, `AddonLoad` asks Nexus for VfxDenoiser's addon directory and checks it exists on disk; nothing further happens if it doesn't. [`src/addon/entry.cpp:64-83`](src/addon/entry.cpp#L64-L83)
2. `ScanInstalledSinFiles` walks that directory for two kinds of file: `VfxD_<name>[-_]v<N>.json` by filename, and any other `.json` file whose content has a top-level `"version"` key (VfxDenoiser's own file shape) — a filesystem/content check only, no network involved. [`src/core/sin_files.h:22-63`](src/core/sin_files.h#L22-L63)
3. A load-time check compares installed versions against GitHub metadata only (no download) and leaves a note in the options panel; nothing is written to disk until the user explicitly clicks "Check now" / "Install" / "Apply". [`src/addon/entry.cpp:44-50,76-78`](src/addon/entry.cpp#L44-L50)
4. On that explicit click, `github_update.cpp` GETs the latest release JSON from the Visual Sins content repo, matches release assets to sin names/versions, and (only for `Install`/`Apply`) GETs the matching `browser_download_url`. [`src/integration/github_update.cpp:373-441,555-583,754-781`](src/integration/github_update.cpp#L373-L441)
5. `merge.cpp` computes a guid-first/name-fallback diff between the old and new file content in memory — pure JSON structural comparison, no game data involved. [`src/core/merge.cpp:1-18`](src/core/merge.cpp#L1-L18)
6. Every write (`ApplyMergePlan` result, a hand-edit, a rollback) goes through the same backup-then-temp-file-then-rename sequence, so a crash mid-write can't corrupt the live file, and the previous content is preserved as a single-generation `.bak`. [`src/core/backup.cpp:106-149`](src/core/backup.cpp#L106-L149), [`src/core/tree/installed_tree_store.cpp:254-328`](src/core/tree/installed_tree_store.cpp#L254-L328)
7. Optionally, a user can submit an effect report from the options panel — primarily to flag one or more effect GUIDs found missing from, or misclassified in, the Visual Sins collection, alongside a required free-text note (GUIDs are optional; a note-only submission is valid). This is POSTed to a relay and does not touch game files at all. [`src/ui/report_ui.cpp:560-595`](src/ui/report_ui.cpp#L560-L595), note-required/GUID-optional validation at [`src/integration/webhook_report.cpp:300-336`](src/integration/webhook_report.cpp#L300-L336)
8. Optionally (new since `0.9.3.0`), a "for science" toggle in the Live Log panel — hidden/disabled unless a `VfxD_Greed.json` file already exists next to the other sin files, and never created automatically — opens a local SQLite database and, from that point on, records every self-cast VFX effect line into it for the developer's own later analysis. This never writes to, or reads from, any existing VfxDenoiser JSON content file, and is fully mutually exclusive with the update/install/apply path in item 4 above (each is blocked while the other is active). [`src/core/effect_db.h:226,240-254`](src/core/effect_db.h#L226-L254), [`src/integration/github_update.cpp:661-663,774`](src/integration/github_update.cpp#L661-L663)

The addon never calls any Guild Wars 2 API, never reads or writes the official GW2 account API, and performs no in-game action on the player's behalf — everything it touches is local JSON config files plus the two network destinations described below.

## AI Notice

Claude (Anthropic) was used extensively during development — architecture decisions, refactoring, bug hunting, and documentation, per the disclosure at [`README.md:3-4`](README.md#L3-L4).

- The developer reviews and accepts responsibility for all published content and shipped behavior.
- AI assistance does not replace source review, builds, in-game testing, or Raidcore's policy assessment.
- This disclosure is included so Raidcore can apply the **AI Notice** classification if required.

## Reverse engineering and source policy

No reverse-engineered implementation is present in this repository.

Specifically, the project has no:

- disassembly-derived game functions, offsets, signatures, or structures — the only game-state reads go through Nexus's public `DataLink_Get` against the publicly documented Mumble Link (`include/Mumble.h`) and RTAPI (`include/RTAPI.hpp`) layouts;
- memory scanner, injector, detour, or hook library;
- packet capture or protocol interception;
- bundled closed-source DLL, executable, or service.

**One item for reviewer attention:** [`src/integration/webhook_config.h`](src/integration/webhook_config.example.h) — the file actually compiled into the shipped DLL — is git-ignored and not present in this repository ([`.gitignore:8`](.gitignore#L8)). It is generated locally at build time by [`tools/generate_webhook_config.py`](tools/generate_webhook_config.py), which XOR-obfuscates a single URL string (the report-relay endpoint) using the fixed key at [`src/integration/webhook_report.cpp:154`](src/integration/webhook_report.cpp#L154). The committed template ([`src/integration/webhook_config.example.h`](src/integration/webhook_config.example.h)) decodes to a placeholder (`example-worker.example.workers.dev`), not the real endpoint. This is standard practice for keeping a non-secret-but-not-advertised URL out of a `strings` pass over the binary — it is explicitly documented as *not* a credential and *not* meant to resist a debugger or network inspection ([`src/integration/webhook_config.example.h:19-27`](src/integration/webhook_config.example.h#L19-L27)) — but it does mean the literal production URL is not visible in this source tree. All logic that constructs, sends, and decodes that request **is** in this repo and reviewable in full ([`src/integration/webhook_report.cpp`](src/integration/webhook_report.cpp)).

The vendored source consists of the public Raidcore Nexus header (MIT, [`include/Nexus.h:1-6`](include/Nexus.h#L1-L6)), Dear ImGui, nlohmann/json, the GW2 Realtime API header, and (new since `0.9.3.0`) the SQLite amalgamation (`include/sqlite3.c`/`.h`), each under its included license — see [`THIRDPARTY.txt`](THIRDPARTY.txt). SQLite is public domain, so it carries no attribution/license obligation at all — no `THIRDPARTY.txt` entry needed, and none was added; that file already only tracks the libraries vendored wholesale from their own upstream repos (ImGui, nlohmann/json, RTAPI.hpp), not every header under `include/` — `Nexus.h` (MIT, inline copyright notice) and `Mumble.h` (no license text) have no entry there either. [`CMakeLists.txt:11-22,56`](CMakeLists.txt#L11-L22)

## Nexus integration surface

| Public Nexus API surface | Use | Call sites |
|---|---|---|
| `DataLink_Get` | Reads Mumble Link and RTAPI shared-memory pointers (map ID, profession, race, specialization, account/character name) for display in the Live Log/Report panels | [`src/core/game_state.cpp:30-32`](src/core/game_state.cpp#L30-L32) |
| `GUI_Register` / `GUI_Deregister` | Registers/deregisters the options-panel render callback — the addon has no floating window of its own | [`src/addon/entry.cpp:74,89`](src/addon/entry.cpp#L74) |
| `Events_Subscribe` / `Events_Unsubscribe` | Subscribes to `EV_VFXD_SINS_LOG` for the Live Log feature (see caveat below) | [`src/core/live_log.cpp:176,187`](src/core/live_log.cpp#L176) |
| `Events_RaiseNotification` | Raises `EV_VFXD_SINS_LISTEN_START`/`STOP` to signal listen-state changes over the same event bus | [`src/core/live_log.cpp:185,199`](src/core/live_log.cpp#L185) |
| `Paths_GetAddonDirectory` | Locates VfxDenoiser's addon folder (read target) — this addon does not use it for its own settings storage, since it stores no settings | [`src/addon/entry.cpp:66`](src/addon/entry.cpp#L66) |
| `Log` | Writes load/unload/error messages | [`src/addon/entry.cpp:82,122`](src/addon/entry.cpp#L82), [`src/integration/github_update.cpp:155`](src/integration/github_update.cpp#L155) |

**Live Log caveat for reviewers:** `EV_VFXD_SINS_LOG` and its companion events ([`src/integration/vfxd_sins_bridge.h`](src/integration/vfxd_sins_bridge.h)) are a wire contract with VfxDenoiser's own code. That side has been implemented and approved upstream in VfxDenoiser, but has not shipped in a public VfxDenoiser release yet — so today this feature only works against a pre-release VfxDenoiser build used for testing, not against the current stock release. The feature is inert (subscribes, receives nothing) against that current release. [`src/core/live_log.h:1-11`](src/core/live_log.h#L1-L11) **This same caveat applies to the "for science" SQLite capture in item 8 above** — its capture hook (`FeedEffectDb`) is called from the same `IngestLogLine` path this event feeds, so it is equally inert against the current stock VfxDenoiser release and only exercised against the same pre-release test build. [`src/core/live_log.cpp:414-429`](src/core/live_log.cpp#L414-L429)

The addon does not use `InputBinds`, `Renderer`, `WndProc` callbacks, `Fonts`, `UI.RegisterCloseOnEscape`, textures, or any self-managed input interception.

Registration/deregistration pairs are visible in [`src/addon/entry.cpp`](src/addon/entry.cpp).

## Network surface

Addon-owned HTTP behavior is implemented with Windows WinHTTP over HTTPS in two files: [`src/integration/github_update.cpp`](src/integration/github_update.cpp) and [`src/integration/webhook_report.cpp`](src/integration/webhook_report.cpp). Neither uses `WINHTTP_FLAG_SECURE`-disabled (plaintext) requests. No `Authorization` header is sent on any request — all endpoints below are either public or authenticated only by the obscurity of the relay URL.

| Endpoint | Auth | Purpose | Source |
|---|---|---|---|
| `GET https://api.github.com/repos/Xen0phy/VfxD-Visual-Sins/releases/latest` | None | Read latest release metadata for the Visual Sins JSON files | [`github_update.cpp:134-135,373-374`](src/integration/github_update.cpp#L373) |
| `GET <asset browser_download_url>` (redirects to a GitHub-owned asset host) | None | Downloads a specific sin's JSON content for diffing/installing, only on explicit user action | [`github_update.cpp:440,583,781`](src/integration/github_update.cpp#L583) |
| `POST <XOR-obfuscated relay URL>` (Cloudflare Worker; not a fixed literal in this source tree — see above) | None (relay itself rate-limits/dedups) | Sends a user-composed effect report — one or more found/missing/misclassified effect GUIDs plus a required free-text note (GUIDs optional, note is not) — the Worker holds the real Discord webhook server-side and this addon never sees or sends a Discord URL directly | [`webhook_report.cpp:164-169,225,379`](src/integration/webhook_report.cpp#L379) |

Additional URL behavior:

- `GetAddonDef` points Nexus's own updater at `https://github.com/Xen0phy/VfxD-Visual-Sins-Updater` for the addon DLL itself — this is Nexus's provider mechanism, not addon-owned downloader code. [`src/addon/entry.cpp:146-147`](src/addon/entry.cpp#L146-L147)
- There are no other network destinations in project-owned runtime code.

## Data handling and privacy

- This addon never requests, reads, stores, or transmits a Guild Wars 2 API key — it has no concept of one.
- The only account-identifying data it can transmit is optional and user-controlled: the effect-report form pre-fills the local account/character name (read via `DataLink_Get`, not the official GW2 API) into an editable field, and defaults to **including** it — a "Send anonymously" checkbox lets the user opt out or clear the field before sending. [`src/ui/report_ui.cpp:90,121-124,572-588`](src/ui/report_ui.cpp#L572-L588)
- Scan results, diffs, and in-memory JSON are held only in process memory; the addon still has no settings file, and writes nothing to disk beyond the VfxDenoiser JSON files/backups it's explicitly told to change, plus (new since `0.9.3.0`, opt-in, off by default) the local `vfxd_effect_db.sqlite3` file described in item 8 above.
- Data captured into that local SQLite file is never transmitted anywhere — it is not referenced anywhere in the report/webhook code path, only read/written locally. [`src/integration/webhook_report.cpp`](src/integration/webhook_report.cpp) has no reference to `effect_db`/`EffectDb`/`sqlite` at all.
- The captured data is limited to effect timing/behavior fields plus the local player's own profession/race/specialization (read via the same `DataLink_Get` path as the rest of the addon) — never any other player's data, never chat, never account credentials.
- No data is cached or transmitted to any analytics/tracking/advertising service.

## Threading and unload behavior

- All network and JSON work for both the update-check and report features runs on short-lived detached `std::thread`s, never the render thread; each increments/decrements an active-thread counter via an RAII guard. [`src/integration/github_update.cpp:68-94`](src/integration/github_update.cpp#L68-L94)
- A single atomic in-flight flag serializes check/diff-load/apply against each other, since they touch the same files. [`src/integration/github_update.cpp:73`](src/integration/github_update.cpp#L73)
- Every disk write (update apply, hand-edit save, rollback) follows the same backup-then-temp-file-then-rename sequence so a crash mid-write can't corrupt the live file. [`src/core/backup.cpp:114-149`](src/core/backup.cpp#L114-L149), [`src/core/tree/installed_tree_store.cpp:254-328`](src/core/tree/installed_tree_store.cpp#L254-L328)
- On unload: shutdown flags are flipped first, in-flight WinHTTP handles are closed to unblock any thread waiting inside a call, then unload polls (bounded to 2 seconds) for the active-thread counters to reach zero before returning, logging a `LOGL_CRITICAL` message if any thread is still running past that window. [`src/addon/entry.cpp:94-126`](src/addon/entry.cpp#L94-L126)

## Build and dependency review

- Target: Windows x64 shared library (cross-compiled via MinGW `x86_64-w64-mingw32-g++`). [`CMakeLists.txt:1-6`](CMakeLists.txt#L1-L6)
- Export: `GetAddonDef` with C linkage. [`src/addon/entry.cpp:135`](src/addon/entry.cpp#L135)
- Addon signature: `0x58565355` ('XVSU'). [`src/addon/entry.cpp:137`](src/addon/entry.cpp#L137)
- Addon version: `0.10.1.0` (`Rev` auto-increments per build — see note at the top of this document). [`src/addon/version.h:7-10`](src/addon/version.h#L7-L10)
- Nexus API version: `6` (`NEXUS_API_VERSION`). [`include/Nexus.h:19`](include/Nexus.h#L19)
- Update provider: Nexus GitHub provider. [`src/addon/entry.cpp:146`](src/addon/entry.cpp#L146)
- Build target definition: [`CMakeLists.txt:20-48`](CMakeLists.txt#L20-L48)
- Statically linked (`-static-libgcc -static-libstdc++ -static`), symbols stripped (`-s`). [`CMakeLists.txt:71-78`](CMakeLists.txt#L71-L78)
- Third-party licenses: [`THIRDPARTY.txt`](THIRDPARTY.txt)

No compiled DLL, executable, static library, object file, or ZIP is tracked in the repository; `build/` and `backup/` are git-ignored. [`.gitignore:1-4`](.gitignore#L1-L4)

## Reviewer code map

| File | Primary review purpose |
|---|---|
| [`src/addon/entry.cpp`](src/addon/entry.cpp) | Addon definition, Nexus API registration, load/unload lifecycle, updater metadata |
| [`src/addon/addon.cpp`](src/addon/addon.cpp) | Options-panel UI glue, install/check/apply action row |
| [`src/core/sin_files.cpp`](src/core/sin_files.cpp) / [`.h`](src/core/sin_files.h) | Filesystem scan and detection logic for VfxDenoiser JSON files |
| [`src/core/merge.cpp`](src/core/merge.cpp) | JSON diff/merge algorithm between old and new sin file content |
| [`src/core/backup.cpp`](src/core/backup.cpp) | `.bak` scan and single-step restore |
| [`src/core/game_state.cpp`](src/core/game_state.cpp) | Nexus `DataLink_Get` usage (Mumble Link / RTAPI) |
| [`src/core/live_log.cpp`](src/core/live_log.cpp) | Nexus event subscribe/raise for the Live Log feature and the "for science" capture hook (`FeedEffectDb`) — both inert against the current stock VfxDenoiser release, see the Live Log caveat above |
| [`src/core/effect_db.cpp`](src/core/effect_db.cpp) / [`.h`](src/core/effect_db.h) | The opt-in local SQLite store for "for science" capture — storage shape, capture API, the `VfxD_Greed.json`-existence gate, mutual exclusion with the update/apply path |
| [`src/integration/github_update.cpp`](src/integration/github_update.cpp) | Exact GitHub endpoints, release/asset parsing, threading, shutdown handling |
| [`src/integration/webhook_report.cpp`](src/integration/webhook_report.cpp) | Effect-report POST construction (GUIDs + required note), XOR-decode of the relay URL, threading |
| [`src/integration/vfxd_sins_bridge.h`](src/integration/vfxd_sins_bridge.h) | Shared event-payload struct/contract with VfxDenoiser (see Live Log caveat) |
| [`CMakeLists.txt`](CMakeLists.txt) | Build target, source list, compiler/linker settings |

## Policy references

- [ArenaNet policy on third-party programs](https://help.guildwars2.com/hc/en-us/articles/360013625034-Policy-Third-Party-Programs)
- [Raidcore `AddonDefinition` documentation](https://docs.raidcore.gg/structAddonDefinition__t.html)
- [Raidcore Nexus API header](https://github.com/RaidcoreGG/Nexus-API/blob/main/Nexus.h)
- [VfxDenoiser (required dependency)](https://github.com/HasKha/VfxDenoiser)
- [VfxD Visual Sins effect collection (companion repo)](https://github.com/Xen0phy/VfxD-Visual-Sins)

This document should be updated and the addon re-reviewed if a future version adds game-memory access, native game hooks, input automation, new network destinations, new API permissions, or a private/closed-source component. (Local persistent storage was added once already, in `0.10.x` — see item 8 above and the new table row in the reviewer summary — and this document has been updated to reflect it. The same standard applies to the next such change.)