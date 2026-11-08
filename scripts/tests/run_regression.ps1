<#
.SYNOPSIS
  Regression checks for the healer-sim fork. Run after building SimC (Release x64).

.DESCRIPTION
  1. Vashnik mechanics: a stand-in that cannot die and has no mitigation takes the whole scripted fight, so
     damage, hit counts and timings are compared with values derived by hand from the Mythic log.
  2. Healer profile: a deterministic run of scripts\restoration_druid.simc compared with baseline.json.
  3. Stock SimC: deterministic runs of stock profiles (Shadow Priest, Blood DK, and the Balance, Feral and Guardian
     druids) compared with baseline.json, to catch engine and druid-module changes leaking into normal sims.
  Exits with code 1 if any check fails.

.PARAMETER UpdateBaseline
  Rewrites baseline.json from the current build. Only use it after confirming a change is intended.

.EXAMPLE
  powershell -File scripts\tests\run_regression.ps1
#>
param([switch]$UpdateBaseline)

$ErrorActionPreference = 'Stop'
$repo     = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$simc     = Join-Path $repo 'bin\x64\Release\simc.exe'
$baseline = Join-Path $PSScriptRoot 'baseline.json'
$work     = Join-Path ([IO.Path]::GetTempPath()) 'simc-regression'
New-Item -ItemType Directory -Force $work | Out-Null

if (-not (Test-Path $simc)) { throw "simc.exe not found at $simc. Build Release x64 first." }

$results = New-Object System.Collections.Generic.List[object]
function Check([string]$group, [string]$name, $actual, $expected, [double]$tolerance = 0) {
  $ok = if ($expected -is [array] -or $actual -is [array]) {
    (@($actual) -join ',') -eq (@($expected) -join ',')
  } elseif ($null -eq $actual -or $null -eq $expected) {
    $actual -eq $expected
  } elseif ($expected -is [string]) {
    [string]$actual -eq $expected
  } else {
    [math]::Abs([double]$actual - [double]$expected) -le $tolerance * [math]::Max(1.0, [math]::Abs([double]$expected))
  }
  $results.Add([pscustomobject]@{ Result = $(if ($ok) { 'PASS' } else { 'FAIL' }); Group = $group; Check = $name;
    Actual = (@($actual) -join ','); Expected = (@($expected) -join ',') })
}

# Runs simc from the repo root, writing its console output to $work\<name>.txt. simc prints warnings on
# stderr, which Windows PowerShell would turn into terminating errors, so errors are relaxed around the call.
function Start-Simc([string]$name, [string[]]$simcArgs) {
  Push-Location $repo
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & $simc @simcArgs 2>&1 | Out-File -FilePath (Join-Path $work "$name.txt") -Encoding utf8
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $previous; Pop-Location }
  if ($code -ne 0) { throw "simc failed for '$name' (exit $code), see $work\$name.txt" }
}

# Runs simc and returns the parsed JSON (json2 output).
function Invoke-Simc([string]$name, [string[]]$simcArgs) {
  $json = Join-Path $work "$name.json"
  Start-Simc $name ($simcArgs + "json2=$json")
  return Get-Content $json -Raw | ConvertFrom-Json
}

# Damage dealt by the boss and its adds, per ability: hits, raw (before mitigation) and actual totals per fight.
function Get-BossDamage($json) {
  $boss = $json.sim.targets[0]
  $stats = @($boss.stats)
  foreach ($p in $boss.stats_pets.PSObject.Properties) { $stats += @($p.Value) }
  $agg = @{}
  foreach ($s in $stats) {
    if ($s.type -ne 'damage') { continue }
    if (-not $agg.ContainsKey($s.name)) {
      $agg[$s.name] = [pscustomobject]@{ hits = 0.0; raw = 0.0; actual = 0.0; hit_sum = 0.0; hit_n = 0.0 }
    }
    foreach ($r in $s.direct_results.PSObject.Properties) {
      $agg[$s.name].hits   += $r.Value.count.mean
      $agg[$s.name].raw    += $r.Value.fight_total_amount.mean
      $agg[$s.name].actual += $r.Value.fight_actual_amount.mean
      # every hit across all fights: exact per-hit values even when the hit count varies per fight (parries)
      $agg[$s.name].hit_sum += $r.Value.total_amount.sum
      $agg[$s.name].hit_n   += $r.Value.total_amount.count
    }
  }
  return $agg
}

# ---------------------------------------------------------------- 1. Vashnik mechanics (hand-derived)
Write-Host 'Vashnik mechanics...'
$mechProfile = 'scripts\tests\vashnik_mechanics.simc'
$mech = Invoke-Simc 'mechanics' @($mechProfile, 'iterations=5', 'threads=1', 'deterministic=1')
$dmg = Get-BossDamage $mech
$g = 'mechanics'

Check $g 'fight length (s)' $mech.sim.statistics.simulation_length.mean 345
Check $g 'wipes' $mech.sim.statistics.encounter_wipes 0
foreach ($k in $dmg.Keys) {
  if ($dmg[$k].raw -gt 0) { Check $g "$k lands unmitigated" $dmg[$k].actual $dmg[$k].raw 1e-9 }  # Imbibe itself deals none
}

# Per-hit values are the tooltip values (Mythic log / 0.952 where only the log was known).
$perHit = [ordered]@{ burning_presence = 33336; caustic_surge = 100009; caustic_surge_tick = 83341; caustic_explosion = 233354
  plague_froth = 166681; plague_wave = 666725; malignant_catalyst = 320862; catalytic_bile = 283224
  dripping_fangs = 2083517; dripping_fangs_tick = 250022; melee = 1664160; congealing_bolt = 175015 }
foreach ($k in $perHit.Keys) { Check $g "$k per hit" ($dmg[$k].hit_sum / [math]::Max(1.0, $dmg[$k].hit_n)) $perHit[$k] 1e-9 }

# Two players. Burning Presence: each add hits on spawn and every 3s until its logged death, 12 adds over 4 waves =
# 130 per player. Caustic Surge: one per add death, 3 ticks each. Exploding Infection: 7 waves; with two players
# both infections are the other healers', dispelled at the log's offsets, each dispel a Caustic Explosion on both
# = 28. Their ticks (every 1.5s, +1 stack each) before the dispels: 0+3, 0+1, 0+5, 0+5, 0+5, 0+5, 0+2 = 26 ticks,
# 6+1+15+15+15+15+3 = 70 stack-ticks.
# Malignant Catalyst: Imbibe +15s and +54s, 8 casts. Catalytic Bile: 7s later, 5 random players (both here).
# Plague Froth: 9 casts (0:14 and Imbibe +35s/+68s), 6 ticks on 7 random players (both here); the last one's 6th tick
# falls at the kill (5:45), so 53 ticks. Plague Wave: as each Froth ends, 3 random players; the last is after the kill.
# Dripping Fangs: bites at 0:10 + 28s (12 by 5:45) with a 23% parry; 16 ticks every 2s each, the last bite's 13 by the
# kill = 189. Congealing Bolt: 8 adds per Imbibe from the log's lifetimes, one every 1.92s from 0.3s = 157 (the log has 158: the 0.3s add dies before its first bolt).
$hits = [ordered]@{ burning_presence = 260; caustic_surge = 24; caustic_surge_tick = 72; caustic_explosion = 28; exploding_infection = 26;
  toxic_vapor = 344; conflagrating_expulsion = 8; gloom_expulsion = 8; malignant_catalyst = 16; catalytic_bile = 16
  plague_froth = 106; plague_wave = 16; dripping_fangs_tick = 189; congealing_bolt = 157 }
foreach ($k in $hits.Keys) { Check $g "$k hits" $dmg[$k].hits $hits[$k] }
# Melee every 3.75s from 0:03.6 (92 swings) and the 12 bites lose ~32% / 23% to parries.
Check $g 'melee hits (92 swings, ~32% parried)' ($dmg['melee'].hits -ge 50 -and $dmg['melee'].hits -le 75) $true
Check $g 'dripping_fangs hits (12 bites, ~23% parried)' ($dmg['dripping_fangs'].hits -ge 6 -and $dmg['dripping_fangs'].hits -le 12) $true

# Expulsions: 108,343 x cast number for 4 Imbibes = 108,343 x 10 each, per player.
Check $g 'conflagrating_expulsion total' $dmg['conflagrating_expulsion'].raw (108343 * 10 * 2) 1e-9
Check $g 'gloom_expulsion total' $dmg['gloom_expulsion'].raw (108343 * 10 * 2) 1e-9
# Toxic Vapor: 10,001 per stack every 2s from 0:02, starting at 1 stack and +1 per Imbibe (which lands before
# the tick at the same second): 11x1 + 42x2 + 42x3 + 42x4 + 35x5 = 564 stack-ticks per player.
Check $g 'toxic_vapor total' $dmg['toxic_vapor'].raw (10001 * 564 * 2) 1e-9
Check $g 'exploding_infection total' $dmg['exploding_infection'].raw (25002 * 70) 1e-9

# Damage-type multipliers, each on top of all x1.5: steady x0.5, burst x2, spot x0.25, tank x4, avoidable x0.5. Each
# spell scales by its type only.
$scaled = Invoke-Simc 'mechanics_scaled' @($mechProfile, 'iterations=5', 'threads=1', 'deterministic=1',
  'encounter_damage_steady=0.5', 'encounter_damage_burst=2', 'encounter_damage_spot=0.25', 'encounter_damage_tank=4',
  'encounter_damage_avoidable=0.5', 'encounter_damage_all=1.5')
$scaledDmg = Get-BossDamage $scaled
$types = [ordered]@{ toxic_vapor = 0.75; burning_presence = 0.75; conflagrating_expulsion = 3; gloom_expulsion = 3;
  caustic_surge = 3; caustic_surge_tick = 3; caustic_explosion = 3; malignant_catalyst = 3; plague_froth = 0.375
  catalytic_bile = 0.375; congealing_bolt = 0.375; exploding_infection = 0.375; dripping_fangs = 6; dripping_fangs_tick = 6; melee = 6; plague_wave = 0.75 }
foreach ($k in $types.Keys) {
  Check $g "$k scaled by its damage type (x$($types[$k]))" $scaledDmg[$k].raw ($dmg[$k].raw * $types[$k]) 1e-9
  Check $g "$k hits unchanged when scaled" $scaledDmg[$k].hits $dmg[$k].hits
}

# Timings, from one logged fight.
$logFile = Join-Path $work 'mechanics_log.txt'
Start-Simc 'mechanics_logged' @($mechProfile, 'iterations=1', 'threads=1', 'log=1', "output=$logFile")
$log = Get-Content $logFile
$timeOf = { param($pattern) @($log | Select-String -Pattern $pattern | ForEach-Object { [double]($_.Line -split ' ')[0] }) }
Check $g 'Imbibe completes at' (& $timeOf "performs Action 'imbibe'") @(24, 108, 192, 276)
Check $g 'add waves spawn at' (@(& $timeOf "Burning_Venom_\d+' arises") | Sort-Object -Unique) @(27, 111, 195, 279)
Check $g 'Shrouded Venom waves spawn at' (@(& $timeOf "Shrouded_Venom_\d+' arises") | Sort-Object -Unique) @(26.7, 110.7, 194.7, 278.7)
Check $g 'Malignant Catalyst at' (@(& $timeOf "Action 'malignant_catalyst' \(0\) hits Player 'Balance_Druid'") | Sort-Object -Unique) @(35, 74, 119, 158, 203, 242, 287, 326)
Check $g 'first Plague Froth tick' ((& $timeOf "Action 'plague_froth' \(0\) hits")[0]) 14
Check $g 'first Dripping Fangs tick' ((& $timeOf "Action 'dripping_fangs_tick' \(0\) hits")[0]) 12
$firstVapor = $log | Select-String -Pattern "toxic_vapor' \(0\) hits" | Select-Object -First 1
Check $g 'first Toxic Vapor hit (time)' ([double]($firstVapor.Line -split ' ')[0]) 2
Check $g 'first Toxic Vapor hit (1 stack)' ([double]([regex]::Match($firstVapor.Line, 'for ([\d.]+)').Groups[1].Value)) 10001
$addDeaths = @($log | Select-String -Pattern "Burning_Venom_\d+ demises" | ForEach-Object { [double]($_.Line -split ' ')[0] }) | Sort-Object
$expectedDeaths = @(7.683, 22.010, 37.181 | ForEach-Object { 27 + $_ }) + @(11.669, 29.080, 39.275 | ForEach-Object { 111 + $_ }) +
  @(13.422, 30.474, 46.334 | ForEach-Object { 195 + $_ }) + @(24.551, 45.711, 61.269 | ForEach-Object { 279 + $_ })
Check $g 'add deaths at' ($addDeaths | ForEach-Object { [math]::Round($_, 3) }) ($expectedDeaths | Sort-Object | ForEach-Object { [math]::Round($_, 3) })

# ---------------------------------------------------------------- Patchwerk (the generic healer test fight)
# Every hit is a share of the stand-ins' health: H = 1M (tanks T = 1.44M), x0.001 (encounter_damage_all), 4 players.
Write-Host 'Patchwerk mechanics...'
$pwProfile = 'scripts\tests\patchwerk_mechanics.simc'
$pw = Invoke-Simc 'patchwerk' @($pwProfile, 'iterations=5', 'threads=1', 'deterministic=1')
$pwDmg = Get-BossDamage $pw
$g = 'patchwerk'
$H = 1000000 * 0.001; $T = 1440000 * 0.001; $N = 4
Check $g 'fight length (s)' $pw.sim.statistics.simulation_length.mean 300
Check $g 'wipes' $pw.sim.statistics.encounter_wipes 0
Check $g 'raid health in the JSON' $pw.sim.statistics.raid_member_health 1000000
$burst = 0.0498 * $H * 30 * 4 / 5  # a normal burst per player; a big one is twice that
$pwHit = [ordered]@{ raid_pulse = 0.0321 * 2 * $H; avoidable_hit = 0.0026 * 20 * $N * $H; spot_dot = 0.0228 * 8 * $N / 3 / 6 * $H
  melee = 0.229 * 2 * $T; tank_buster = 0.0422 * 30 * $T; tank_buster_dot = 0.1054 * 30 / 16 * $T }
foreach ($k in $pwHit.Keys) { Check $g "$k per hit" ($pwDmg[$k].hit_sum / [math]::Max(1.0, $pwDmg[$k].hit_n)) $pwHit[$k] 1e-4 }  # hits are rounded
# 8 normal bursts (0:20 + 30 sec steps, every 4th big) and 2 big ones, each 60% on impact + 4 x 10%, on all 4 players
Check $g 'raid_burst total' $pwDmg['raid_burst'].raw (8 * $burst * $N) 1e-6
Check $g 'big_raid_burst total' $pwDmg['big_raid_burst'].raw (2 * 2 * $burst * $N) 1e-6
Check $g 'raid_burst hits' $pwDmg['raid_burst'].hits (8 * 5 * $N)
$pwLogFile = Join-Path $work 'patchwerk_log.txt'
Start-Simc 'patchwerk_logged' @($pwProfile, 'iterations=1', 'threads=1', 'log=1', "output=$pwLogFile")
$pwLog = Get-Content $pwLogFile
$pwTime = { param($pattern) @($pwLog | Select-String -Pattern $pattern | ForEach-Object { [double]($_.Line -split ' ')[0] }) }
Check $g 'big bursts land at' (@(& $pwTime "Action 'big_raid_burst' \(0\) hits Player 'Balance_Druid'") | Sort-Object -Unique) @(110, 111, 112, 113, 114, 230, 231, 232, 233, 234)
Check $g 'first tank buster' ((& $pwTime "Action 'tank_buster' \(0\) hits")[0]) 10
Check $g 'first dispellable_dot tick' ((& $pwTime "Action 'dispellable_dot' \(0\) hits")[0]) 36.5
# Health from the healer: the stand-ins take the healer's health (tanks x1.44).
$pwHealer = Invoke-Simc 'patchwerk_healer' @('scripts\restoration_druid.simc', 'scripts\fights\patchwerk.simc', 'iterations=20', 'threads=1',
  'deterministic=1', 'encounter_damage_all=0.2032')
$pwH = $pwHealer.sim.statistics.raid_member_health
Check $g 'raid health from the healer (not the 900k default)' ([int]($pwH -gt 500000 -and $pwH -ne 900000)) 1
Check $g 'tank health = raid health x 1.44' $pwHealer.sim.statistics.raid_tank_health ($pwH * 1.44) 1e-9
# ---------------------------------------------------------------- Damage tolerance search (engine, tolerance=1)
# A small, coarse search: the healer profile plus one gear set (tests\tolerance_sets.simc) and one noise rerun. Checks
# the results land in the JSON, the gear set swaps the healer's item (not the boss's) and the profileset is not also
# run as a normal profileset.
Write-Host 'Damage tolerance search...'
$g = 'tolerance'
$tol = Invoke-Simc 'tolerance' @('scripts\restoration_druid.simc', 'scripts\fights\vashnik.simc', 'scripts\tests\tolerance_sets.simc', 'threads=4',
  'iterations=20', 'tolerance=1', 'tolerance_share=0.2032', 'tolerance_iterations=60', 'tolerance_type_iterations=60',
  'tolerance_precision=0.02', 'tolerance_type_precision=0.05', 'tolerance_tests=steady', 'tolerance_noise_runs=1',
  'tolerance_level_iterations=60')
$ts = $tol.sim.tolerance
Check $g 'version' $ts.version 5
Check $g 'setups (main, gear set, noise)' (@($ts.setups | ForEach-Object { $_.role }) -join ',') 'main,set,noise'
$tmain = $ts.setups | Where-Object { $_.role -eq 'main' }
$tset = $ts.setups | Where-Object { $_.role -eq 'set' }
$score = ($tmain.tests | Where-Object { $_.test -eq 'combined' }).score
Check $g 'combined score found (50-300%)' ($score -gt 0.5 -and $score -lt 3) $true
Check $g 'tests (combined + steady)' (@($tmain.tests | ForEach-Object { $_.test }) -join ',') 'combined,steady'
# A single type is raised from the all-damage limit, with the rest held there: it can never end below that limit.
Check $g 'single types never below the limit' (@($tmain.tests | Where-Object { $_.test -ne 'combined' -and $_.score -lt $score }).Count) 0
Check $g 'probes ascending' ((($tmain.tests[0].probes | ForEach-Object { $_.pct }) -join ',') -eq (($tmain.tests[0].probes | ForEach-Object { $_.pct } | Sort-Object) -join ',')) $true
Check $g 'main keeps its trinket1' ($tmain.gear.trinket1.item -match '\bid=270169\b') $true
Check $g 'gear set swaps trinket1' ($tset.gear.trinket1.item -match '\bid=280835\b') $true
Check $g 'gear set item level 334' $tset.gear.trinket1.ilevel 334
Check $g 'no normal profileset run' ($null -eq $tol.sim.profilesets) $true
# Fixed damage levels (tolerance_levels, default 100% and 120%): every setup is measured at each, with the burst
# measurements (engine\sim\tolerance_stats.cpp). Every death is attributed to a kind of damage.
Check $g 'levels' (@($ts.levels) -join ',') '1.0,1.2'
Check $g 'every setup measured at each level' (@($ts.setups | ForEach-Object { @($_.levels | ForEach-Object { $_.pct }) -join '/' }) -join ',') '1.0/1.2,1.0/1.2,1.0/1.2'
$lv = $tmain.levels[1].stats
Check $g 'level fights' $lv.fights 60 0.1
Check $g 'bursts measured' ([int]($null -ne $lv.bursts.big -or $null -ne $lv.bursts.normal)) 1
$bs = @($lv.bursts.normal, $lv.bursts.big) | Where-Object { $_ }
Check $g 'raid low below raid before' (@($bs | Where-Object { $_.raid_low -ge $_.raid_before }).Count) 0
Check $g 'recovered share 0-1' (@($bs | Where-Object { $_.recovered -lt 0 -or $_.recovered -gt 1 }).Count) 0
$causes = 0.0; foreach ($c in $lv.death_causes.PSObject.Properties) { $causes += $c.Value }
Check $g 'death causes add up to the deaths' $causes $lv.deaths_per_fight 1e-9
Check $g 'raid health kept 0-100%' ([int]($lv.raid_health -gt 0 -and $lv.raid_health -le 1)) 1
Check $g 'burst windows heal and hurt more' ([int]($lv.windows.burst.damage_taken_per_sec -gt $lv.windows.quiet.damage_taken_per_sec -and $lv.windows.burst.healing_per_sec -gt 0)) 1
# The gear check (tolerance_search=0, what the .bat files run): no limit search, every setup only at the fixed level.
$chk = Invoke-Simc 'gear_check' @('scripts\restoration_druid.simc', 'scripts\fights\patchwerk.simc', 'scripts\tests\tolerance_sets.simc',
  'threads=4', 'iterations=20', 'tolerance=1', 'tolerance_search=0', 'tolerance_levels=1.2', 'tolerance_share=0.2032',
  'tolerance_iterations=60', 'tolerance_noise_runs=1')
$cs = $chk.sim.tolerance
Check $g 'gear check: no search' ([int]($cs.search -eq $false)) 1
Check $g 'gear check: setups per pattern' (@($cs.setups | ForEach-Object { $_.role }) -join ',') 'main,set,noise,main,set,noise'
Check $g 'gear check: no limit tests' (@($cs.setups | Where-Object { @($_.tests).Count -gt 0 }).Count) 0
Check $g 'gear check: one level with raid health' (@($cs.setups | Where-Object { @($_.levels).Count -eq 1 -and $_.levels[0].stats.raid_health -gt 0 }).Count) 6
# Your limit with the fast search (tolerance_search_sets=0, what the .bat files run): current gear only, a rough pass
# then aimed full tests. Close to the full search's 130.1% for the druid (bursts every 30 sec) in far fewer tests.
$fast = Invoke-Simc 'fast_limit' @('scripts\restoration_druid.simc', 'scripts\fights\patchwerk.simc', 'scripts\tests\tolerance_sets.simc',
  'iterations=1', 'tolerance=1', 'tolerance_search_sets=0', 'tolerance_tests=none', 'tolerance_share=0.2032', 'tolerance_levels=none',
  'tolerance_noise_runs=0')
$fs = $fast.sim.tolerance
$fmain = @($fs.setups | Where-Object { $_.role -eq 'main' })[0]
$flim = ($fmain.tests | Where-Object { $_.test -eq 'combined' }).score
Check $g 'fast limit near the full search (125-135%)' ([int]($flim -gt 1.25 -and $flim -lt 1.35)) 1
Check $g 'fast limit: at most 8 full tests' ([int](@($fmain.tests[0].probes).Count -le 8)) 1
Check $g 'fast limit: gear sets not searched' (@($fs.setups | Where-Object { $_.role -eq 'set' -and @($_.tests).Count -gt 0 }).Count) 0
# Damage patterns (fights\patchwerk.simc: bursts every 30 sec, and the same bursts every 1 min): the search runs once
# per pattern and the pattern's options reach the boss. Half the burst damage is clearly easier (130% vs 154% at full
# precision), so an ignored pattern would show as equal limits.
$tolp = Invoke-Simc 'tolerance_patterns' @('scripts\restoration_druid.simc', 'scripts\fights\patchwerk.simc', 'threads=4',
  'iterations=20', 'tolerance=1', 'tolerance_share=0.2032', 'tolerance_iterations=100', 'tolerance_precision=0.02',
  'tolerance_tests=none', 'tolerance_noise_runs=0', 'tolerance_levels=none')
$tps = $tolp.sim.tolerance
Check $g 'patterns' (@($tps.patterns | ForEach-Object { $_.name }) -join ',') 'Bursts every 30 sec,Bursts every 1 min'
Check $g 'one main setup per pattern' (@($tps.setups | ForEach-Object { "$($_.role):$($_.pattern)" }) -join ',') 'main:Bursts every 30 sec,main:Bursts every 1 min'
$p30 = $tps.setups[0].tests[0].score
$p60 = $tps.setups[1].tests[0].score
Check $g 'the same bursts every 1 min raise the limit (pattern reaches the boss)' ($p60 -gt $p30 + 0.04) $true

# ---------------------------------------------------------------- 2 + 3. Golden values (baseline.json)
Write-Host 'Healer profile and stock SimC...'
$golden = [ordered]@{}

$druid = Invoke-Simc 'healer_profile' @('scripts\restoration_druid.simc', 'scripts\fights\vashnik.simc', 'iterations=200', 'threads=1', 'deterministic=1')
$healer = $druid.sim.players | Where-Object { $_.name -eq 'Silverblooms' }
$others = @($druid.sim.players | Where-Object { $_.name -ne 'Silverblooms' })
$frames = $healer.collected_data.raid_frames
$golden['healer_profile'] = [ordered]@{
  players                  = $druid.sim.players.Count
  fights                   = $druid.sim.statistics.encounter_fights
  wipes                    = $druid.sim.statistics.encounter_wipes
  simulation_length        = $druid.sim.statistics.simulation_length.mean
  healer_hps               = $healer.collected_data.hps.mean
  healer_damage_taken      = $healer.collected_data.dtps.mean
  healer_fight_length      = $healer.collected_data.fight_length.mean
  raid_damage_taken        = ($others | ForEach-Object { $_.collected_data.dtps.mean } | Measure-Object -Sum).Sum
  raid_frames_units        = @($frames.units).Count
  raid_frames_deaths       = @($frames.deaths).Count
  death_recaps             = @($healer.collected_data.death_recaps).Count
}

$golden['patchwerk_healer_profile'] = [ordered]@{
  raid_member_health  = $pwHealer.sim.statistics.raid_member_health
  healer_hps          = ($pwHealer.sim.players | Where-Object { $_.name -eq 'Silverblooms' }).collected_data.hps.mean
  raid_damage_taken   = ($pwHealer.sim.players | ForEach-Object { $_.collected_data.dtps.mean } | Measure-Object -Sum).Sum
}
$evoker = Invoke-Simc 'evoker_healer_profile' @('scripts\preservation_evoker.simc', 'scripts\fights\vashnik.simc', 'iterations=200', 'threads=1',
  'deterministic=1', 'encounter_damage_all=0.2032')
$evokerHealer = $evoker.sim.players | Where-Object { $_.name -eq 'Duskryth' }
$golden['evoker_healer_profile'] = [ordered]@{
  wipes               = $evoker.sim.statistics.encounter_wipes
  healer_hps          = $evokerHealer.collected_data.hps.mean
  healer_damage_taken = $evokerHealer.collected_data.dtps.mean
  raid_damage_taken   = (@($evoker.sim.players | Where-Object { $_.name -ne 'Duskryth' }) |
                         ForEach-Object { $_.collected_data.dtps.mean } | Measure-Object -Sum).Sum
}

# The druid and evoker profiles guard the restoration and preservation work in sc_druid.cpp and sc_evoker.cpp: the
# other specs must stay identical (Devastation Flameshaper runs the Flameshaper code Preservation shares).
foreach ($profile in 'MID1_Priest_Shadow', 'MID1_Death_Knight_Blood', 'MID1_Druid_Balance', 'MID1_Druid_Feral',
                     'MID1_Druid_Guardian', 'MID2_Druid_Feral', 'MID1_Evoker_Devastation_FS') {
  $tier = $profile.Substring(0, 4)
  $stock = Invoke-Simc $profile @("profiles\$tier\$profile.simc", 'iterations=300', 'threads=4', 'deterministic=1')
  $c = $stock.sim.players[0].collected_data
  $golden[$profile] = [ordered]@{ dps = $c.dps.mean; hps = $c.hps.mean; dtps = $c.dtps.mean }
}

if ($UpdateBaseline) {
  $golden | ConvertTo-Json -Depth 5 | Set-Content -Path $baseline -Encoding UTF8
  Write-Host "Baseline written to $baseline"
} elseif (-not (Test-Path $baseline)) {
  Write-Warning "No baseline.json yet; run with -UpdateBaseline to create it. Skipping golden checks."
} else {
  $expected = Get-Content $baseline -Raw | ConvertFrom-Json
  foreach ($group in $golden.Keys) {
    foreach ($metric in $golden[$group].Keys) {
      # Deterministic runs reproduce exactly; the tiny tolerance only absorbs float formatting.
      Check $group $metric $golden[$group][$metric] $expected.$group.$metric 1e-9
    }
  }
}

# ---------------------------------------------------------------- summary
$results | Format-Table -AutoSize Result, Group, Check, Actual, Expected | Out-String -Width 220 | Write-Host
$failed = @($results | Where-Object Result -eq 'FAIL').Count
Write-Host ("{0} checks, {1} failed" -f $results.Count, $failed) -ForegroundColor $(if ($failed) { 'Red' } else { 'Green' })
exit $(if ($failed) { 1 } else { 0 })
