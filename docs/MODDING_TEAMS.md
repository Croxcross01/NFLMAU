# Adding teams beyond the 32 NFL teams

Notes from getting two extra teams to play full matches. Everything here is applied as a patch to your own copy of
the game (`mods/active/` overlay plus `mods/teams.ini`); no game data is part of this repository.

## Team ids

- The game's id allocator table gives TEAM a range up to 1023, with **1005 and above reserved** for fixed league
  teams (AFC 1011, NFC 1010, Free Agents 1009, NFL Greats 1008, the hidden slots 1013 / 1021 ...).
- At match start the setup (`sub_88BFE630`) gives the two playing teams match ids (home 0, away 33) and renumbers
  them in the match database. Teams with reserved ids are **not** renumbered: the follow-up lookup fails (code 23),
  the uniform id stays at the "none" value 1023, the helmet name comes out empty (`_Helmet`) and the match crashes.
- New teams therefore need **ordinary ids below 1005** (we use 100 and 101). A team's roster file is found through
  its team record (`TOID` -> that team's `TDAN` file name), so the spare roster slots can still be used for the file.

## Databases a new team has to be in

| Where | What |
|---|---|
| qkl_boot entry 384 (team lists) | team record, type 0 so team select lists it |
| qkl_boot entry 70 (master roster) | team record **and** its players (`PLAY.TGID`): a match takes players from here |
| the team's roster slot entry | roster file (players, depth chart) with the team's id |

Rules learned the hard way:

- Never change a database's row counts, even within capacity (the game hangs at boot or at "Play Now").
  Take over unused rows instead (e.g. the master row of the NFL Greats pool team).
- Leave the match database's own team table (entry 343) alone.
- Checksums: see `tools/tdb.py` (`fix_checksums`).

## Player ids and depth charts

- Player ids from 0x7F4C up are reserved like team ids from 1005; drafted players with such ids get free ordinary ids.
- Each NFL Greats player has no face art (face id 0); that is fine.
- The on-field lineup is read from the match depth chart table DCGA (`select * from DCGA where TGID = <match team>`),
  filled from the league depth charts. New teams have none, and with two new teams in one match nobody was put on the
  field. `ModBuildDepthCharts` fills DCGA for such a team from the match player table PLGA through the game's own
  database calls.

## Code hooks (config/arcade.toml.in, host/src/mod_hooks.cpp)

| Hook | Purpose |
|---|---|
| `ModArtTeamRemap` | uniform art lookup uses the art team from `mods/teams.ini` |
| `ModDbQuery` | database queries on art/presentation tables for a mod team use its art team |
| `ModTextureGuard` | an invalid texture object in a material slot is treated as empty instead of crashing |
| `GuardRelease`, `GuardReleaseInline`, `GuardSetModel` | skip releases / models that are not game objects (crash fixes) |
| `ModBuildDepthCharts` | builds a match depth chart (per position, by overall) for a match team that has none |
| `ModPrefixUniform`, `ModPrefixMixList`, `ModPrefixSetup` | give a mod team its own uniform prefix (see below) |
| `ModSwapImage` | diagnostics: logs every UI picture request as library / item |
| others (`ModTeamSetupFail`, `ModTeamRenumber`, `ModNameHash`, `ModPlayerInfo`, ...) | diagnostics, active with `MADDEN_MOD_DEBUG` |

`mods/teams.ini` lines are `<team id>=<art team id>[,<own uniform prefix>]`. Set `MADDEN_MOD_DEBUG=1` to log every team query,
renumbering step and uniform request.

## Own uniforms (helmet, jersey, numbers, names, pants, socks)

How the game finds a team's uniform:

1. The uniform prefix comes from the team record (`TEAM.TPFN`, e.g. `EAG`).
2. `<prefix>_MixAndMatch` is an XML list of the team's uniform parts (`<bigfile name="eag_jersey_2010_dargre_big"/>`
   per helmet / jersey / pants / socks, with home / away and the number and name textures).
3. Each `*_big` bundle is a nested BGFA in qkl_fe2ig.ast: XPR2 textures plus a material description (RSF) and, for
   jerseys, the name-letter widths.

For a new team (teams.ini `100=13,HOF`) the host replaces the prefix right after the game reads it (three places, see
the hooks). We then add `HOF_MixAndMatch` and the `HOF_*_big` bundles to qkl_fe2ig.ast. The art team (13 here) still
gives the uniform records and the material layout.

Rules:

- **qkl_ram.ast must not grow.** It is loaded into a fixed buffer at boot; a few KB more and the last entries
  (game scripts) are cut off and a match crashes. Add new entries to qkl_fe2ig.ast only; the game finds them there.
- Bundle textures must keep the template's size, format, mip count and header. Every name in a bundle is renamed to
  the same length (`EAG_JERSEY_2010_DARGRE` -> `HOF_JERSEY_2010_GOLBLK`), so the material file stays valid.
- Texture format: XPR2, header 0x60 bytes, data tiled (Xenos 32x32-block tiles) and 16-bit byte-swapped. Mips follow
  level 0. Mip levels at or below 16 px share one packed tail tile. Stacked textures (numbers, name letters) align the
  slice count of mip levels to 4.

Tools (private dev-tools repo, `tools/uniforms/`):

| Script | What |
|---|---|
| `xenos_tex.py` | XPR2 read / DXT1/3/5 decode and encode / tiling / mips (round-trips the game's textures) |
| `bgfa_write.py` | build nested bundles; add entries to a top-level archive (`add_entries`) |
| `make_jersey.py`, `make_numbers.py`, `make_gear.py` | paint jerseys, numbers, name letters, captain patch, helmet, pants, socks |
| `make_bundles.py` | builds every bundle and MixAndMatch list for the teams in its `TEAMS` table |
| `apply_uniforms.py` | adds the built bundles to a copy of qkl_fe2ig.ast (run against the original game file) |

## Own logos (team select, large logos, scoreboard)

- `TEAM.TLGO` is the team's logo number. Every screen asks for a picture by it: the UI calls a library / item pair.
  `ModSwapImage` logs these pairs.
- The picture packs are nested BGFAs without names. Each entry's id field (first 4 bytes of the hash, little-endian)
  is the **item key**, and the game **binary-searches** the keys. So the keys must stay unique and sorted.
  Renumbering entries, or appending keys out of order, blanks or crashes those screens.

| Pack | Where | Keys |
|---|---|---|
| team select tiles home / away | qkl_boot 92 / 91 | logo number 0..31 (we add 32 blank, 33, 34) |
| large team logos | qkl_boot 90 | 0..34 (33 / 34 were NFC / AFC, now Hall Of Fame / Legends) |
| scoreboard team plates | qkl_fe2ig 327 | home = logo number, away = 1000 + logo number (we add 33, 34, 1033, 1034) |

Hall Of Fame uses logo 33 and Legends 34. They are set by `OWN_LOGO` in `build_hof.py`. `make_logos.py` builds
the packs; `apply_logos.py` (boot) and `apply_plates.py` (fe2ig) put them in.

## Checklist: adding another team

1. Pick an ordinary team id below 1005 and a free roster slot. Add the team to `build_hof.py` (team record, players
   in the master roster, `NEW_IDS`, `TORD`, `OWN_LOGO`) and rebuild qkl_boot.
2. `mods/teams.ini`: `<id>=<art team>,<3-letter prefix>`.
3. Uniforms: add the team to `make_bundles.py` `TEAMS` (template team, renames, kit colours), run it, then run
   `apply_uniforms.py` on the original qkl_fe2ig.ast.
4. Logos: pick the next free logo number. Add it to `make_logos.py` `TEAMS` and to all four packs, with keys kept
   sorted. The large-logo pack and the position list (LogoPositions, in qkl_ram, which cannot grow) only know 0..34,
   so a third new team needs either a reused slot or more work there.
5. Depth chart: matches are covered by `ModBuildDepthCharts`. For the game's own screens, new players need depth
   chart rows, taking over existing rows.
6. Test: new team vs regular team, new team vs new team, both home and away; check the logs.

## Lines mode: 5-on-5 / 7-on-7 / full lines (mods/lines.txt)

`mods/lines.txt`: `7` = 7-on-7, `full` = all linemen every down, anything else / no file = normal 5-on-5.

How it works (host `mod_hooks.cpp`, hooks in `arcade9.toml`):
- All formations in the playbooks are 11-man; arcade mode fields 5. The Entourage game changer (power-up 9) brings the
  rest of the linemen in through a step in `sub_8871B230` that only runs while bit 0x200 of the game-changer phase
  word (0x894D818C) is set; it walks each team's players and brings in roles 5-9 (offense) / 10-12 (defense).
- Lines mode sets that bit at each new down, whenever the game state ([[0x894AF670]+32]: 1 pre-snap, 3 play, 4 after)
  changes to 1 or 3, and every 8 frames during state 1 (the play pick rebuilds the formation).
- `ModLinesTrigger` makes the team's power-up slot read 9 for that pass, `ModLinesRestore` puts the real power-up back,
  `ModLinesCanUse` ignores the "can use now" check for this step, `ModLinesFilter` / `ModLinesFilterDefense` pick
  who comes in: 7-on-7 = LT/RT (offense) and LE/RE (defense).
- Do not force rules flag 8/9 (vtable+120 of the rules object): flag 8 is a per-frame behaviour flag; forcing it made
  skill players block.
- Alignments: the game files keep the real 11-man alignments (5-on-5 and full lines). The 7-on-7 ones (ends at
  |x| <= 2.6 yards in the defensive playbook, qkl_fe2ig 260; LT/RT at the guard spots, |x| <= 1.67, in the offensive
  playbook, qkl_boot 82 = qkl_fe2ig 261) are in `mods/lines7.bin`, made from the user's files by devtools
  `tools/lines/make_lines_patch.py`. The host finds the playbooks in guest memory by anchor bytes and switches those
  regions to the 7-on-7 or original bytes for the chosen mode (`LinesApplyAlignment`, at the pre-game window and each
  down). `de_inside.py` / `ot_inside.py` are the older file-patching versions (not used now).
- The players-per-side choice is in the pre-game lineup window (LB/RB, Q/E, click); it rewrites mods/lines.txt.
