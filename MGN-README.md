# RPCS3-MGN (MrMadness1 fork)

This is a small fork of [RPCS3](https://github.com/RPCS3/rpcs3) maintained for **MGN** online play experiments. It is **not** an official RPCS3 release. Please report issues on this fork’s repository, not to RPCS3 upstream.

## What changed

1. **Local lobby emulation for RPCN** — Sony-style “lobby” APIs (`JoinLobby`, `LeaveLobby`, `SetLobbyMemberDataInternal`, `GetLobbyMemberDataInternalList`, and related calls) now complete with success callbacks and plausible response data on your PC. RPCN still handles rooms and authentication; no RPCN protocol changes are required. This helps games (including Dark Souls: Prepare to Die Edition on PS3) that wait for lobby callbacks before continuing online.
2. **Branding** — The window title and version string include **MGN** so logs and screenshots are clearly from this fork.
3. **No auto-updater** — The built-in updater that pulls official RPCS3 builds is disabled, so you are not prompted to replace this build with upstream.
4. **CI** — GitHub Actions builds a **Linux x86_64 AppImage** for this fork.

## How to toggle lobby emulation

1. In RPCS3, open **Configuration → Network** and set **PSN status** to **RPCN** (with your RPCN account configured).
2. Open **RPCN account / settings** (from the RPCN UI).
3. Enable or disable **“Emulate lobbies locally (RPCN)”**.

The option is **on by default** in this fork. When enabled, log lines are tagged `[MGN lobby emu]`.

Setting is stored in your RPCN config file (`Emulate lobbies locally`).

## License

RPCS3 is licensed under **GPLv2**. This fork keeps that license and upstream attribution. Source for this fork is available in this repository; you may obtain corresponding source for any GPLv2 binaries distributed to you.
