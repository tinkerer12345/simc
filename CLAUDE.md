# Healer simulation fork of SimulationCraft

Personal fork of SimC that simulates **healers** against scripted boss encounters.
Healer under test: Silverblooms, Restoration Druid (EU/Silvermoon). Encounter: Vashnik, Mythic.

## Goal: measure damage tolerance, not healing output
- Effective HPS is capped by incoming damage, so it can't rank setups. The result is **how much damage a setup survives**: the player's **limit**.
- One healer (the druid) + raid stand-ins, no other healing. The 3 stand-in healers are names only (they take damage, never heal). The removed other-healer work is in git history (commit d55782e103).
- **Limit** = highest % of one healer's share of the damage survived. 100% = the share: `tolerance_share=0.2032` (a quarter of the 4 healers' effective healing / raid damage taken on the Mythic kill log), set in `scripts\restoration_druid_sim.bat`. Reference marks (`tolerance_milestones`): your kill 103.1%, top healer on the kill 112.2%, 3-healer setup 133.3%.
- **Fail rule:** any player dies in more than 5% of fights. Running out of mana is NOT a fail, only information.
- **Damage types**, each with its own multiplier: steady, burst, spot, tank, avoidable (damage a clean raid wouldn't take, kept at the log's rate). All types raised together = the limit; then each type alone with the others held at the limit, to show where the room is.
- Gear sets are compared like DPS sims: **% more damage you can heal than your current gear**, better/worse only beyond the measured noise.
- Results are only as good as the healer APL and the encounter model: compare setups with the same APL.

## Build, run, test
- Build (PowerShell, repo root), only after changes under `engine\`:
  `& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' simc_vs2026.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal`
- Run: `scripts\restoration_druid_sim.bat` = ONE simc call (no PowerShell; user, 2026-10-26: everything the engine can do lives in the engine): the detailed sim of the current gear at the share, the limit search, the gear sets of `scripts\gear_sets.simc`, and the healer report (`scripts\reports\restoration-druid-healer.html`, opened). ~2.5 min with 4 gear sets.
- Profile: `scripts\restoration_druid.simc` (the user's SimC addon export of 2026-10-26: talents, gear, Mid2 2pc/4pc, Omnium runes; `encounter=Vashnik`, `difficulty=mythic`, `max_time=345`). Player options go in the druid section: command-line options apply to the LAST actor (Vashnik).
- `scripts\tests\run_regression.ps1` after every engine change (~1 min, exit 1 on failure, 129 checks; `-UpdateBaseline` only after confirming a change is intended):
  - Vashnik mechanics (`tests\vashnik_mechanics.simc`, immortal stand-ins, no mitigation): per-hit damage, hit counts, damage-type multipliers, timings from the log.
  - The limit search (`tests\tolerance_sets.simc`): JSON shape, a gear set swaps the healer's item, no normal profileset run.
  - `tests\baseline.json`: golden deterministic results for the healer profile and stock MID1 Shadow Priest, Blood DK, Balance/Feral/Guardian and MID2 Feral, proving the fork doesn't change normal sims. Keep their iterations/threads fixed.
- Upstream's own tests (`tests\run.py`) need Python + `simc_support`, not installed here. Equivalent smoke test: every `profiles\MID1`/`MID2` profile on Patchwerk/DungeonSlice/HeavyMovement, druid profiles with `enable_all_talents=1 enable_all_sets=1`; pass = no `Error:` line. Known upstream failures, not ours: 18 MID1 profiles have outdated talent hashes, Demon Hunters refuse DungeonSlice.
- PowerShell gotchas (regression script): `[math]::Min(1, x)` picks the Int32 overload (use `1.0`); names are case-insensitive; SimC warnings on stderr become ErrorRecords (don't treat them as failures).

## Keeping upstream intact
- Normal SimC sims (no `encounter=`, no opt-in options) must behave and report exactly as upstream. Gate fork behaviour on `!sim->encounter_str.empty()`, `receives_buffs()` / `receives_raid_debuffs()` (false only for our bosses), the Restoration spec / its talents, or opt-in options. `is_boss()` alone is NOT enough: it is also true for stock target dummies (the 2026-10-28 review fixed "Boss" labels leaking into every report).
- Additive JSON fields are fine (healers get dtps/deaths/heal_taken; gear gets enchant/gem names; players get `omnium`).

## Damage tolerance (engine)
- `engine/class_modules/bosses/sc_encounter_damage.cpp/.hpp`: `damage_type_e` and `raid_damage_t` (all living players, `random_targets( n )` or `single_target()`; target list rebuilt every cast because enemy actions are never `activate()`d; scaled by its type's multiplier before mitigation). **Every boss damage spell must be a `raid_damage_t` with a type.** Options `encounter_damage_steady/_burst/_spot/_tank/_avoidable` and `encounter_damage_all` (on top).
- `engine/sim/tolerance.cpp/.hpp` (`sim->tolerance`, like `plot_t`), `tolerance=1`: after the main sim (`sc_main.cpp`), child sims (`deterministic=1`, same seed) bracket the breaking point by steps, then halve to the precision. The profile: all damage (`tolerance_iterations` 1000, precision 0.0025), then each of `tolerance_tests` (300 fights, 0.005). Gear sets: the profile's `profileset."Name"+=line` lines, taken over from `profileset_map` at init (so no normal profileset run), each line replacing the healer's same-name option or added at the end of the healer's section (before `encounter=` / `raid_*=` / the next actor); all damage only, starting at the profile's limit. `tolerance_noise_runs` (2) reruns with other seeds give the noise. Item lines may carry `ilevel=<n>`. JSON `sim.tolerance` (version 3: setups[] with role main/set/noise, gear, tests[] with probes).
- `healer_html=<file> healer_html_template=<file>` (needs a JSON report): `tolerance_t::print_healer_html` puts the JSON into the template's `__SIMC_DATA__`.

## Encounters (boss modules)
- `encounter=<name>` -> `bosses::create_encounter()`; `sc_bosses.cpp/.hpp` is a dispatcher ONLY. One file pair per boss (`sc_vashnik.cpp/.hpp`); new source files go into `source_files/engine_make`, `VS_engine.props`, `cmake_engine.txt` and `QT_engine.pri`.
- Base `enemy::enemy_t` (`sc_enemy.hpp`); adds are enemy pets (`PET_ENEMY`). Bosses: `ROLE_NONE`, class "Boss", no player buffs, no raid debuffs.
- Background boss actions must call `sequence_add(...)` to appear in the report timeline. Hand-coded damage: set `base_dd_max = base_dd_min` and create spells with non-zero damage (SimC only applies mitigation to actions with direct damage at init).
- Generic hooks: encounter timers (`sim->encounter_timer( name, ago )`, APL `encounter.<name>.remains` / `.ago`) and dispels (`sim->encounter_dispellable` / `encounter_dispel`, APL `target.dispellable`; Nature's Cure keeps its cooldown only when something was removed, only when the hook is set).
- Wipe rules (encounter sims): everyone dead, every tank dead (`encounter_wipe_on_tanks`), or fewer than `encounter_wipe_raid_pct` (0.2) alive; reaching the fight end = kill. On a death, every actor's HoTs/DoTs on that player are cancelled. Profile sets `infinite_health=0`. No `Healing_Target_N` padding in encounter sims unless `healing=<n>`.

## Vashnik (Mythic)
- All numbers in `vashnik_tuning_t` (`difficulty=mythic`; other difficulties rejected). Sources: Wowhead tooltips (`nether.wowhead.com/tooltip/spell/<id>` shows Mythic values) and the user's WCL CSVs; log values divided by 0.952 (a damage-done debuff the raid kept on enemies). Spell IDs aren't in SimC's data, so damage is hand-coded.
- Mechanics: Imbibe (4s cast at 20s, every 84s) -> Expulsions (108,343 x cast number each), +1 Toxic Vapor stack, Burning Venoms (Burning Presence; Caustic Surge at logged deaths) and 8 Shrouded Venoms (Congealing Bolt until logged deaths). Toxic Vapor from the pull. Plague Froth + Plague Wave, Malignant Catalyst + Catalytic Bile, Dripping Fangs + melee with tank swaps, Exploding Infection (7 waves of 4; removal = Caustic Explosion on everyone; the healer owns 1 dispel per wave, the others follow the log; undispelled ones expire after 30s, an assumption).
- Tags: steady = Toxic Vapor, Burning Presence; burst = Expulsions, Caustic Surge, Caustic Explosion, Malignant Catalyst; spot = Plague Froth, Catalytic Bile, Congealing Bolt, Exploding Infection; tank = Dripping Fangs, melee; avoidable = Plague Wave.
- Not modelled: Stygian Infection (heal absorb; needs a generic heal-absorb feature and application data), Virulent Fumes, Umbral Ejection, Catalytic Bile's no-soak hit, Dripping Fangs' physical-taken debuff. Per hit the sim runs ~10-20% above the log.

## Raid stand-ins
- `raid_tanks/_healers/_ranged/_melee=<n>` (max 39) create stand-ins named after specs (the report reads class colour from the name); `sc_raid_member.cpp/.hpp`. Profile: 2/3/7/7 + the druid = 20, placed before `encounter=`. Health and damage reduction only (`PLAYER_SIMPLIFIED`, `RACE_NONE`, default level: level -1 crashes init); tank reductions calibrated to the log's tank damage, the rest are placeholders.

## Restoration Druid (`sc_druid.cpp`)
- Upstream lacked most of the current Resto kit; implemented and gated by talent/set/spec: Abundance, Power of the Archdruid, Nature's Bounty, Thriving Vegetation, Nurturing Dormancy, Rampant Growth, Overgrowth, Everbloom cleave, Blooming Frenzy, Photosynthesis, Lifetreading, Swiftmend extras, Flourish, Convoke in caster form, the Wildstalker kit, diminishing Harmony, Mid2 set (Genesis). Mana: Innervate restores mana, Master Shapeshifter (caster and 5-CP cat finishers), Starsurge's real cost. Fixed upstream Resto bugs (free Germination, Clearcasting never consumed, Lifebloom double bloom, final-tick `adjust_duration`, Harmony on enemies).
- Trinket implemented in `unique_gear_midnight.cpp`: Sigil of the Impending Eclipse (haste proc scaled by the healed target's missing health).
- Not modelled: Everbloom Lifebloom stacking every 5s (waiting on the user's Lifebloom tick data), Convoke's mini-Tranquility, Ironfur mana.
- APL additions: actions `ironbark`, `natures_swiftness`; expressions `injured_allies.<pct>`, `raid_health.pct`, `active_dot.rejuvenation`, `active_dot.regrowth`. The profile APL follows the user's notes and kill-log cast counts (HoT upkeep on self and one tank, Tranquility ramp before Imbibes, Wild Growth/Nature's Swiftness/Hex Lord's Dooming Idol before Convoke, Heart of the Wild cat weave for mana, Starsurge/Wrath in downtime). Change it with the user, not unilaterally.
- Calibration vs the kill log: most per-hit values within ~20%; raw HPS 481k vs the log's 729k.
- Planned by the user (discuss first): Thriving Vegetation point into Verdancy, Reforestation into Nature's Bounty.

## Opt-in recorders (player options on the druid)
- `raid_frames=1`: the sample fight, every player each second and at each death (health, mana, absorbs, your class-spell HoTs, killing blows). Units use `p->role`, not `primary_role()`.
- `death_recap=<seconds>`: per-second summary before each death, damage by ability, killing blow. Deterministic runs are identical with it on or off.

## Healer report (`scripts/healer_report/healer_report_template.html`)
- All layout and logic (vanilla JS + SVG, dark by default); template changes never need a rebuild. Sections: summary; **Your gear** (character sheet with Wowhead icons, stats, enchants, gems, Omnium runes; the best gear set highlighted); **Your limit** (one number, per kind of damage lowest to highest, reference marks); **Gear comparison** (▲/▼/– vs current gear, grey = noise); then "Fight details: your current gear" (timeline, why you died, casting vs idle, healing, damage, the raid, mana and buffs, details).
- Wording for players (user, 2026-10-26): "limit"; no "requirement", "headroom", "level", script names or run metadata; every section says which gear it shows.
- Item and rune names, icons, colours and in-game tooltips come from Wowhead's `tooltips.js` on `wowhead.com/item=` / `spell=` links; an "In-game tooltips" toggle (localStorage) switches them off, the report's own tooltips stay.
- SimC JSON quirks: `collected_data.deaths` holds death TIMES (fight count = `fight_length.count`); `dtps` is per fight; buff `uptime` is 0-100; `buffed_stats` is empty; averaged mana timelines sink when players die early (use `action_sequence[].resources.mana`).
- Verify without a browser: Edge headless `--dump-dom` / `--screenshot` (URL-encode spaces, scratch `--user-data-dir`, `--virtual-time-budget=8000` for Wowhead's script). Don't restyle SimC's built-in HTML report.

## Debugging
- Debug/log output is buffered and lost on a crash: use temporary `fprintf( stderr, ... )` traces. No cdb on this machine.

## Current state and next steps
- Gear and talents of 2026-10-26: limit 119.1% (burst and tank 119.1%, steady and spot 120.6%). Sigil of the Impending Eclipse at 334: no difference in either trinket slot; Hex Lord's Dooming Idol at 334: -0.6% in either slot. Noise ±0.25%.
- 3-healer question: the 133.3% mark assumes the same raid damage with each healer covering a third; 119.1% is below it, but the sim's raw HPS is below the log's, so it may be pessimistic. Needs logs of raids 3-healing Mythic Vashnik to calibrate.
1. Stygian Infection (heal-absorb feature + data from the user) and Virulent Fumes.
2. Calibration gaps (raw HPS, Thriving Vegetation, Bursting Growth, Wild Growth casts) and the Everbloom stacking question.
3. Speed: cache the noise reruns while the profile doesn't change; per-type comparison of gear sets.
4. Encounter variance (jittered add deaths and dispel times).
- Nothing since d55782e103 is committed yet.

## Working preferences
- Keep code organised: specific things in their own files, generic files stay generic.
- Don't create files that won't be committed; keep notes in this file, short and current (git history holds the past).
- Confirm before large or hard-to-reverse changes. Line endings in this repo are CRLF.
