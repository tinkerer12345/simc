# Healer simulation fork of SimulationCraft

Personal fork of SimC aimed at simulating **healers** against scripted boss encounters.
Healer under test: Silverblooms, Restoration Druid (EU/Silvermoon).

## Build and run
- Build (PowerShell, repo root). Only needed after changes under `engine\`:
  `& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' simc_vs2026.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal`
- Run: `scripts\restoration_druid_sim.bat` -> writes SimC HTML/JSON/txt to `scripts\reports\`, then builds and opens the healer report.
- Profile: `scripts\restoration_druid.simc` (`role=heal`, `max_time=345`, `encounter=Vashnik`, `difficulty=mythic`). Its APL is only self-Regrowth: a test harness, not a real rotation.

## Encounters (boss modules)
- `encounter=<name>` profile option -> `bosses::create_encounter()` (registered in `engine/sim/sim.cpp`).
- `engine/class_modules/bosses/sc_bosses.cpp/.hpp`: dispatcher ONLY. Never put boss-specific code here.
- One file pair per boss, e.g. `sc_vashnik.cpp/.hpp`. New source files must be added to `source_files/engine_make`, `source_files/VS_engine.props`, `source_files/cmake_engine.txt` and `source_files/QT_engine.pri`.
- Reusable enemy base: `enemy::enemy_t` in `engine/class_modules/sc_enemy.hpp`. Adds are enemy pets (`pet_t`, `PET_ENEMY`).
- Boss rules: `ROLE_NONE`, reported as class "Boss", keep race, no player buffs (`receives_buffs()` false), no raid debuffs (`receives_raid_debuffs()` false). Generic `enemy=` behaviour must stay unchanged.
- Boss spells hit living, non-pet players only. Background boss actions must call `sequence_add(...)` to appear in the report timeline.
- Hand-coded damage must set `base_dd_max = base_dd_min` too: SimC rolls between min and max (max defaults to 0), so setting only min silently halves the damage.

## Vashnik (Mythic, validated against a Mythic WCL log; more damage events still to be added)
- Simulating **Mythic** only. `difficulty=mythic` (boss option, line after `encounter=`) selects a `vashnik_tuning_t` table; other difficulties are rejected until their values are filled in. All encounter numbers live in that table. Fight length 345s to match the log.
- Damage values are tooltip values (no raid debuff). In the log every Vashnik spell landed at x0.952 (a damage-done debuff the raid put on enemies; some adds had it too), so log-only values are divided by 0.952.
- Hand-coded "hits every living player" spells derive from `raid_damage_t`. Create them with non-zero damage: SimC only applies target mitigation to actions that have direct damage at init.
- Imbibe (ID 1284663): 4s cast at 20s then every 84s; activates the fountains. Vashnik stands in fire and shadow, gaining Flame/Shadow Infusion (1293971/1293968, 1.5 min, so stacks never drop). Each Imbibe deals no damage itself but triggers Conflagrating Expulsion (1298587, fire) and Gloom Expulsion (1284785, shadow assumed) for 108,343 x cast number each (log 103,142.5), adds a Toxic Vapor stack and spawns a Burning Venom wave.
- Toxic Vapor (ID 1284561, "School Damage", 300yd): room aura active from the pull with 1 stack; every 2s, 10,001 Nature per stack (tooltips: Mythic 10,001, Heroic 9,269, Normal 6,592).
- Burning Venom waves: 3 adds, 3s after each Imbibe (4 waves, `Burning_Venom_1..12`). Burning Presence 33,336 fire every 3s (in the log the druid is not always in range). Deaths per wave from the log; on death Caustic Surge 100,009 + 3 ticks of 83,341 fire, 1s apart.
- Caustic Explosion (ID 1295209, fire, 300yd): 233,354 (log 222,153); player-triggered, so its 28 times are replayed from the log (groups of 4, ~20s and ~72s after each Imbibe).
- Spell IDs are not in the bundled spell data, so damage is hand-coded.
- Mitigation (verified against in-game tooltips and the log): versatility (4.92% with Mark of the Wild) x Thick Hide (4%) x avoidance (1.77%). Avoidance applies to every Vashnik spell, ticks included: the log shows identical mitigation for ticks and hits at the same moment.
- SimC players have infinite health by default; the profile sets `infinite_health=0` so the healer can die. In encounter sims (`encounter=` set) the iteration ends when every non-pet player has died (`player_t::demise` -> `sim->cancel_iteration()`) and counts as a wipe (`sim->encounter_wipes`, merged across threads, only for data-collected iterations); other sims are unchanged (verified with deterministic stock profiles). JSON: `sim.statistics.encounter_fights` / `encounter_wipes`; the report shows a KILL/WIPE badge (kill = survived to the end, since boss health is a placeholder and fight length is the real kill time).
- SimC runs one warm-up iteration per thread that is not data-collected, but `collected_data.deaths` still counts its deaths (deaths.count can exceed the fight count). Sample-fight charts are still cut at the healer's last action.
- Enemy actions are never `activate()`d (enemies are not in `player_list`), so their cached target lists are not refreshed when a player dies. `raid_damage_t::execute` rebuilds the list on every cast; before this, dead players kept taking damage (it inflated damage taken ~3x).
- Why the sim dies but the real player didn't: in the log ~17.7M damage reached Silverblooms and the raid's other healers and shields kept them up; the sim has no other healing yet (see Next steps 1). Sim damage is also ~16% above the log (0.952 debuff removed, lower mitigation, Burning Presence always in range).

## Raid members (stand-ins, engine)
- Sim options `raid_tanks` / `raid_healers` / `raid_ranged` / `raid_melee=<n>` (max 39 total) create `Tank_1`, `Healer_1`, `Ranged_1`, `Melee_1`... (`engine/class_modules/raid/sc_raid_member.cpp/.hpp`, settings in `sim_t::raid_stand_in`). Profile uses 2/3/7/7 + the druid = 20. Settings (placeholders): `raid_member_health` 900000, `raid_member_damage_reduction` 0.10, `raid_member_avoidance` 0, `raid_tank_health` 1300000, `raid_tank_damage_reduction` 0.15, `raid_healer_hps` 0 (each stand-in healer heals the lowest-health living player every 1s; set from log data). Put them after the druid section, before `encounter=`.
- Roles: tanks ROLE_TANK/position front, healers ROLE_HEAL/ranged_back, ranged ROLE_DPS/ranged_back, melee ROLE_DPS/back. Melee vs ranged is only a label so far (no positional damage).
- A raid member has health, damage reduction, no rotation/gear/consumables/racials, can die. It reuses `PLAYER_SIMPLIFIED` (SimC's Evoker stand-in type: no gear or item effects) and borrows Priest base stats like `simplified_player_t`. Must use `RACE_NONE`, the default level (`sim->max_player_level` is -1 while parsing: level -1 crashed init) and "disabled" consumables. Stand-ins show in JSON with race "none".
- Boss spells already hit every living player and the wipe check covers them. In encounter sims every player's JSON gets dtps/deaths/heal_taken. The druid test APL heals the lowest-health player (`target_if=min:target.health.pct`).
- With `raid_healer_hps=0` the whole raid wipes at ~0:38-0:45.

## Raid frames (engine, opt-in)
- Player option `raid_frames=1` (on the druid). `engine/player/raid_frames.cpp/.hpp`, member `player_t::raid_frames`. In the sample fight (same rule as `sequence_add`: `nth_iteration() == 1`) it snapshots every player each second and at each death: health, absorbs (`absorb_buff_list`), alive, and the owner's HoTs on them (`action->find_dot( target )`), only from class spells (`data().class_family() != 0`, so item/enchant/rune procs like Rune of Unleashed Fire are skipped); buffs are deliberately not recorded. The report shows each HoT as a square in its colour (`HOT_COLORS`, legend above the frames) with the seconds left rounded down. Deaths store the killing blow. JSON: `collected_data.raid_frames` {units, snapshots, deaths}.
- Units use `p->role` (profile role), not `primary_role()` (a Resto Druid reports "attack").
- Report "The raid": cross-fight summary, then raid frames grouped Tanks/Healers/Melee/Ranged, a strip of death squares (click to jump), a timeline scrubber, HoT timer squares, hover tooltips. Frames are built once and `render()` updates them in place (rebuilding them on every slider step caused jitter); the slider row and status line have fixed sizes. Deaths on the same displayed second share one square (role-coloured strip per death, hover lists each with its killing blow); clicking shows the snapshot after the last of them and highlights all of them (red outline, DIED tag) with a cause summary.
- In encounter sims, when a player dies, every actor's HoTs/DoTs on them are cancelled (`player_t::demise`; SimC only cancels the dead actor's own). The recorder stores no effects or absorbs for dead units.
- Verify the report without a browser: Edge headless `--dump-dom` / `--screenshot` (URL-encode spaces in the file path, use a scratch `--user-data-dir`).
- Debugging crashes: debug/log output is buffered and lost on a crash; use temporary `fprintf( stderr, ... )` traces (e.g. in `sim_t::init_actor`, which runs each actor's init steps in order). No cdb on this machine.

## Death recap (engine, opt-in)
- Player option `death_recap=<seconds>` (0 = off, the default; max 120). `engine/player/death_recap.cpp/.hpp`, member `player_t::death_recap`.
- While on, keeps a rolling window of damage taken, healing received and waiting (idle) time; on death stores a per-second summary (damage, absorbed, healing, overheal, health, mana, idle), damage by ability and the killing blow. Up to 1000 deaths kept; merged across threads. Exported as `collected_data.death_recaps`.
- Hooks (all guarded by `enabled()`): `player_t::do_damage` (after health loss, and at the death check), `assess_heal`, the three waiting-time sites, `reset`, `merge`. Mana is only sampled at damage/heal events.
- Verified harmless: `deterministic=1` runs with it off and on give identical results (druid profile, stock Shadow Priest and Blood DK).
- Command-line options apply to the LAST actor in the profile (Vashnik here), not the druid; put player options in the druid section.

## Healer report (our own, separate from SimC's HTML report)
- "Why you died" section (only when death recaps exist): cause split across deaths, a recap chart of the last seconds (earliest / typical / latest death), and what hit you. Causes come from `DEATH_RULES` in the template (burst, out of mana, idle while dropping, not enough healing): interpretation, tunable without a rebuild.
- `scripts/healer_report/healer_report_template.html`: all layout and logic (vanilla JS + SVG, dark by default). Template changes never need a rebuild.
- `scripts/healer_report/build_healer_report.ps1 -Json <file> [-Out <file>] [-Open]` injects SimC JSON into the template.
- Difficulty is stored on the sim (`sim->encounter_difficulty_str`) and written to JSON as `sim.options.encounter` / `encounter_difficulty` (only when an encounter is set); the report shows it as a badge, in the top bar and in Simulation settings.
- `engine/report/json/report_json.cpp` was changed so healers also get dtps / timeline_dmg_taken / deaths / heal_taken (SimC only did this for tanks).
- SimC JSON quirks: `collected_data.deaths` holds death TIMES (count = fights ending in death, mean = average time); real fight count is `fight_length.count`; `collected_data.dtps` is damage taken PER FIGHT; buff `uptime` is 0-100; `buffed_stats` is empty (use gear ratings and `action_sequence[].resources_max`); `timeline_healing_taken` is empty for self-healing; background actions are not in `action_sequence` unless logged.
- Do not restyle SimC's built-in HTML report; that attempt was reverted.
- Report sections: summary (with casting/idle bar), timeline, activity (donut + casting-per-time-slice columns from `waiting_time` and per-spell `total_execute_time`), healing, damage taken (school colours, mitigated = `total_amount` - `actual_amount`), mana and buffs, details.
- `build_healer_report.ps1` needs absolute paths for `-Out` when run from another directory.

## Next steps
1. Healing from the raid's other healers onto everyone (needs a WCL "Healing Taken by source" export), and a per-player damage-taken-by-ability breakdown (the report's "What hurt" is raid-wide). Burning Venom `true_level` is set from `max_player_level` (-1) at construction too, so the adds are level 2: check whether that matters.
2. A real Resto Druid APL (the current one is out of mana by ~1:30 with full Vashnik damage).
3. Remaining Vashnik damage sources, encounter variance (e.g. jittered add deaths, so overlaps can happen), regression tests.

## Working preferences
- Keep code organised: specific things in their own files, generic files stay generic.
- Confirm before large or hard-to-reverse changes. Line endings in this repo are CRLF.
