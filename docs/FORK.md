# E33Lanes: fork of kipavy/e33-coop

E33Lanes adds **lane combat** (3 players, each with a full party, against one shared enemy state) and later **free roam** to [kipavy/e33-coop](https://github.com/kipavy/e33-coop). The design brief is in `../e33-coop fork Lane Combat + Free Roam — Design Brief.md` (outside this repo); decisions specific to the fork are in `DECISIONS-FORK.md`; spike results are in `SPIKES.md`. Upstream docs (French) stay as they are.

- **Repository:** github.com/ryguyqwerty/e33-coop, branch `fork/main`. Remote `upstream` = kipavy/e33-coop.
- **License:** GPL-3.0, same as upstream. Upstream copyright notices are kept; UE4SS is MIT.
- **Game build:** Steam 23765773 (same as upstream at the pinned commit). Every offset and Blueprint signature in `GAME-INTERNALS.md` is tied to it.

## Pinned upstream commit

`6f05893` (v0.6.35, 2026-10-25), "CHANGELOG 0.6.35: a different hit count is a desync, not normal play".

Rebase onto upstream with `git fetch upstream && git rebase upstream/main` on a branch, then rerun the scenario battery. Record each rebase below.

| Date | Upstream commit | Notes |
| --- | --- | --- |
| 2026-11-16 | `6f05893` | Fork point |

## Divergences from upstream

Keep this list current: it is the checklist for every rebase.

### Identity (Phase 1): never join, update or load against upstream installs

| What | Upstream | Fork | Where |
| --- | --- | --- | --- |
| Mod folder, DLL target, mod name | `ue4ss/Mods/E33Coop`, `E33Coop` | `ue4ss/Mods/E33Lanes`, `E33Lanes` | `native/mod/CMakeLists.txt`, `dllmain.cpp`, workflows, `tools/package/*`, installer |
| Co-op save folder | `SaveGames/e33coop` | `SaveGames/e33lanes` | `sync/coopsave.cpp`, `explore_join.cpp`, `update.cpp`, `remote/*` |
| Steam lobby tag | `e33coop` | `e33lanes` | `net/steam.cpp` |
| Protocol magic | `0x43333345` "E33C" | `0x4C333345` "E33L" | `protocol/messages.hpp` |
| Release repo (updater, installer) | `kipavy/e33-coop` | `ryguyqwerty/e33-coop` | `common/release.hpp` (`kRepo`), `common/https.hpp`, `ui/ui.cpp`, installer |
| Release assets | `e33coop-player.zip`, `Installer E33 Coop.exe` | `e33lanes-player.zip`, `Installer E33 Lanes.exe` | `release.hpp`, workflows, packaging |
| Display name | E33 Coop | E33 Lanes | UI, installer |

Done as one rename of `E33Coop`/`e33coop`/`E33 Coop` across `native/` (except `third_party/`), `tools/`, `.github/`, `lua/`. Internal macros (`E33COOP_VERSION`, `E33COOP_API`) are unchanged. Launch options (`-e33run`, `-e33lang`…) are unchanged: both mods ship their own UE4SS, so they are never loaded together.

### Added

- `lua/LaneProbe`: the one-PC viability probe (see `SPIKES.md`, S0). Dev tool only, not packaged.
- `docs/FORK.md`, `docs/DECISIONS-FORK.md`, `docs/SPIKES.md` (English).

## CI secrets

The `mod` job checks out RE-UE4SS, whose `UEPseudo` submodule is private: it needs a GitHub account linked to an Epic Games account, and a token in the repository secret `UEPSEUDO_PAT` (see upstream `DECISIONS.md` D2). Without it only the `tools` job (fake peer, installer, unit tests) builds.
