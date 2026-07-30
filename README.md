# VfxD Visual Sins Updater

> [!WARNING]
> **AI Notice** — Visual Sins Updater was developed with heavy use of AI assistance, specifically Claude by Anthropic. From architecture decisions and refactoring to bug hunting and documentation, Claude was a core part of the development process.

A [Nexus](https://raidcore.gg/Nexus) addon for Guild Wars 2 that automates installing, checking, editing, and updating the VfxD "Visual Sins" json files (`VfxD_Gluttony.json`, `VfxD_Pride.json`, `VfxD_Sloth.json`, if available) that HasKha's VfxDenoiser reads to apply its effects. It lives in the Nexus options panel, shows you a diff before it touches anything, and takes a backup before every write so you can roll back the most recent change with one click.

The file-management and update features (scanning, diffing, installing, backing up, restoring) only depend on VfxDenoiser's on-disk file locations and json format, not on its code — a standalone tool that just knows where to look. The one exception is the **Live Log** feature, which is built around a Nexus-event wire contract meant to be shared directly with VfxDenoiser's own code. That side doesn't exist upstream yet — **Live Log currently only works against a test fork, not against a stock, unpatched VfxDenoiser install. See [Beta status](#-beta-status) below.**

This tool does **not** generate or author the effects themselves — it only manages the json files that VfxDenoiser consumes. The actual "Visual Sins" effect collection lives in the companion repo:

➡️ **Visual Sins effect collection:** https://github.com/Xen0phy/VfxD-Visual-Sins

## Hard requirement

This addon is **useless on its own**. It requires **HasKha's VfxDenoiser** to already be installed and configured, because VfxDenoiser is what actually reads these json files and applies the visual filtering in-game.

➡️ **VfxDenoiser (required):** https://github.com/HasKha/VfxDenoiser

Install and set up VfxDenoiser first. This addon will not work, and may not even find anything to do, without it.

## ⚠️ Beta status

This is a **beta (v0.9.3.0)**. It has been intensively tested by me, the author, but it hasn't been battle-tested across every setup, and edge cases can have been overlooked. Things *can* break — not guaranteed to, but possible. Expect:

- Rough edges in the UI and update-check flow
- Possible failed or partial merges on unusual/hand-edited json files
- Missing error handling in edge cases that haven't been hit yet
- **Live Log doesn't work against stock VfxDenoiser yet.** It's built on an event contract that VfxDenoiser's own code needs to raise, and that side of the patch hasn't landed upstream — right now it only works against a test fork used during development.

Use it, but don't trust it blindly — read the [Safety & Risks](#safety--risks) section below before installing.

## What it does

- **Scans** your VfxDenoiser addon folder for installed json files (both the GitHub-tracked Visual Sins collection — Gluttony, Pride, Sloth — and any other JSON file it recognizes by content).
- **Checks GitHub** for newer versions of the Visual Sins collection specifically and shows you a diff of what would change before you apply anything. Update-checking and installing from GitHub is limited to that collection; any other recognized json file can still be browsed and edited, it just has no upstream to check against.
- **Installs / applies updates** on your explicit action (button click) — nothing is downloaded or written silently in the background beyond the version check itself.
- **Backs up before every write.** Every install, update, or edit copies the current file to a `.bak` first, and only one backup generation is kept per json file (each new write overwrites the previous backup — it is *not* version history).
- **Lets you roll back the most recent change** for a file from the Backups panel (one step only).
- **Live log** of in-game effect activity inside the options panel — requires a VfxDenoiser-side event patch that hasn't landed upstream yet, see [Beta status](#-beta-status).
- Optional webhook reporting (off by default, `webhook_config.example.h`) — not user-facing; this is for anyone building the addon themselves who wants to wire up their own update-event notifications (e.g. to Discord).

## Safety & risks

Please read this before installing.

- **This is a native C++ DLL that runs inside your Guild Wars 2 client process** via Nexus. Like any Nexus addon, it has the same level of access to your system as the game itself. Only install it from a source you trust, and only build it from source you've reviewed if you're cautious.
- **It writes to files on your disk** (inside your VfxDenoiser addon folder) and **downloads files from GitHub** over the network (release assets for the Visual Sins collection, and version-check metadata). If GitHub, DNS, or a release asset is ever compromised or spoofed, a malicious file could theoretically be pulled down and applied. Review a diff before hitting "Apply" to be careful.
- **AI-assisted code**: this project was developed with heavy AI assistance. A lot of effort went into stability regardless — thread safety, write-safety, and repeated re-evaluation of the logic — but it has not had a full independent security audit. Treat it the way you'd treat any addon from a hobbyist — useful, and carefully built, but not something to blindly grant trust to.
- **Backups are shallow, not a safety net for everything.** Only the most recent `.bak` is kept per file. If you make several changes in a row (via this tool or by hand) without noticing something went wrong, you can only roll back one step, not to an arbitrary earlier point.
- **File-detection assumptions.** The addon detects json files by filename pattern *and* by peeking for a VfxD-style `"version"` key in JSON content. If you keep unrelated JSON files in the same folder that happen to match that shape, they could be picked up unexpectedly. Keep your VfxDenoiser folder limited to files you expect this tool to see.
- **Use at your own risk.** As with any third-party Guild Wars 2 addon, using unofficial tools that interact with the game client carries some inherent risk with respect to ArenaNet's terms of service, in the same category as Nexus and other Nexus addons generally. Make your own call on this based on your risk tolerance.

If you hit a bug, please open an issue rather than working around it silently — this is beta software and reports are how it gets better. Note that the integrated effects-reporting system can also be used to send me an issue directly if no GUID is entered.

## Requirements

- Guild Wars 2 with [Nexus](https://raidcore.gg/Nexus) installed
- [HasKha's VfxDenoiser](https://github.com/HasKha/VfxDenoiser) installed and set up (**required**, see above)

## Installation

1. Make sure Nexus and VfxDenoiser are both installed and working first.
2. Grab the latest release DLL and drop it into your Nexus addons folder.
3. Launch the game. Open the Nexus options panel and find the "VfxD Visual Sins Updater" section.
4. If it found your VfxDenoiser folder automatically, you'll see the list of installed json files and their update status. If not, something's wrong with your VfxDenoiser install — fix that first.
5. Use "Check now" to diff against the latest GitHub release, and "Apply"/"Install" per file to update. Review the diff before applying if you want to know exactly what's changing.