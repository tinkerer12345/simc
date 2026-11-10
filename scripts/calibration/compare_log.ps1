# Sim vs log: the healer's spells side by side, from a Warcraft Logs export and a sim of the same fight.
#
#   powershell -ExecutionPolicy Bypass -File scripts\calibration\compare_log.ps1 [-LogDir <folder>] [-Fight coiled_altar]
#
# Warcraft Logs exports (CSV button), put in scripts\calibration\wcl\ (git-ignored: logs name other players), either:
#  - event lists: the healer's healing events (Events tab, as many pages as it takes) and the cast events, all
#    "Events - *.csv". Pages may overlap (identical rows count once). Fight length = the last event, or -LogSeconds.
#  - or the summary tables: the Healing tab ("Healing *.csv") and the Casts tab ("Casts *.csv").
# Per spell (matched by name; Convoke's spells count under their own name, as in the log): share of effective healing,
# hits per minute, raw healing per hit (effective + overheal + absorbed), overheal, casts per minute. Rows more than
# -Tolerance apart are flagged.
# Note: the log counts free spreads (Power of the Archdruid) as Rejuvenation casts; the sim doesn't (about 30% fewer).
param(
  [string]$LogDir,
  [string]$HealerProfile = 'restoration_druid.simc',
  [string]$Fight = 'coiled_altar',
  [string]$Player = 'Silverblooms',
  # damage level as a multiple of one healer's share (0.2032); 1.2 = near the limit, where a Mythic attempt is played
  [double]$Level = 1.2,
  [int]$Iterations = 200,
  [double]$Tolerance = 0.15,
  [double]$LogSeconds = 0
)

$ErrorActionPreference = 'Stop'
$scripts = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $scripts
$simc = Join-Path $root 'bin\x64\Release\simc.exe'
$data = Join-Path $scripts 'reports\data'
if (-not (Test-Path $data)) { New-Item -ItemType Directory -Force $data | Out-Null }
$json = Join-Path $data 'calibration.json'
if (-not $LogDir) { $LogDir = Join-Path $PSScriptRoot 'wcl' }

# --- the sim ---------------------------------------------------------------------------------------------------------
Push-Location $scripts
try {
  $dmg = 0.2032 * $Level
  & cmd.exe /c "`"$simc`" $HealerProfile fights\$Fight.simc iterations=$Iterations threads=16 deterministic=1 encounter_damage_all=$dmg json=`"$json`" >nul 2>&1"
} finally { Pop-Location }
if (-not (Test-Path $json)) { throw 'The sim produced no JSON' }
$j = Get-Content -Raw $json | ConvertFrom-Json
$simMin = $j.sim.statistics.simulation_length.mean / 60
$pl = $j.sim.players | Where-Object { $_.name -eq $Player }
if (-not $pl) { throw "No player $Player in the sim" }

$sim = @{}
$simCasts = @{}
$stats = @($pl.stats) + @($pl.stats | ForEach-Object { $_.children }) | Where-Object { $_ }
foreach ($st in $stats) {
  $name = $st.spell_name
  if (-not $name) { $name = $st.name -replace ' \(absorb\)$', '' }
  if ($st.name -notmatch '_convoke$' -and $st.num_executes -and $st.type -ne 'damage') {
    if (-not $simCasts.ContainsKey($name)) { $simCasts[$name] = 0.0 }
    $simCasts[$name] += $st.num_executes.mean / $simMin
  }
  if ($st.type -ne 'heal' -and $st.type -ne 'absorb') { continue }
  $eff = 0.0; $raw = 0.0; $hits = 0.0
  foreach ($res in @($st.direct_results, $st.tick_results)) {
    if (-not $res) { continue }
    foreach ($kind in 'hit', 'crit') {
      $x = $res.$kind
      if (-not $x) { continue }
      $eff += $x.fight_actual_amount.mean
      $raw += $x.fight_total_amount.mean
      $hits += $x.count.mean
    }
  }
  if ($eff -eq 0 -and $st.compound_amount) { $eff = $st.compound_amount; $raw = $eff }
  if (-not $sim.ContainsKey($name)) { $sim[$name] = [pscustomobject]@{ eff = 0.0; raw = 0.0; hits = 0.0 } }
  $sim[$name].eff += $eff; $sim[$name].raw += $raw; $sim[$name].hits += $hits
}
$simEff = ($sim.Values | Measure-Object -Property eff -Sum).Sum

# --- the log ---------------------------------------------------------------------------------------------------------
function Num([string]$s) { [double]($s -replace '[^0-9.]', '') }
function Seconds([string]$t) { $p = $t -split ':'; [double]$p[0] * 60 + [double]$p[1] }

$log = @{}
$logCasts = @{}
$minutes = 0
$eventFiles = @(Get-ChildItem -LiteralPath $LogDir -Filter 'Events*.csv' -ErrorAction SilentlyContinue)
if ($eventFiles.Count -gt 0) {
  # spell names, longest first, so "Rejuvenation (Germination)" wins over "Rejuvenation"
  $known = @($sim.Keys) + @('Rune of Lingering', 'Rune of Unleashed Fire', 'Matted Fur', 'Frenzied Regeneration',
    "Aessina's Renewal", 'Flower Walk', 'Healthstone', 'Leech', 'Renewal', "Nature's Bounty", 'Everbloom') |
    Sort-Object -Unique | Sort-Object { - $_.Length }
  $seen = New-Object 'System.Collections.Generic.HashSet[string]'
  $unknown = @{}
  $last = 0.0
  foreach ($f in $eventFiles) {
    $rows = Import-Csv -LiteralPath $f.FullName
    foreach ($r in $rows) {
      $key = ($r.PSObject.Properties | ForEach-Object { $_.Value }) -join '|'
      if (-not $seen.Add($key)) { continue }
      $t = Seconds $r.Time
      if ($t -gt $last) { $last = $t }
      if ($r.PSObject.Properties.Name -contains 'Type') {
        # cast events: "Time","Type","Ability","Source -> Target"
        if ($r.Type -ne 'Cast') { continue }
        $ab = $r.Ability -replace ' [0-9.]+ sec$', ''
        if (-not $logCasts.ContainsKey($ab)) { $logCasts[$ab] = 0.0 }
        $logCasts[$ab] += 1
        continue
      }
      # healing events: "<healer> <spell> <target> [Tick] +<effective> [(O: <overheal>)] [(A: <absorbed>)]"
      $text = $r.Event
      if (-not $text.StartsWith("$Player ")) { continue }
      $rest = $text.Substring($Player.Length + 1)
      $spell = $known | Where-Object { $rest.StartsWith("$_ ") } | Select-Object -First 1
      if (-not $spell) {
        $w = ($rest -split ' ')[0..1] -join ' '
        if (-not $unknown.ContainsKey($w)) { $unknown[$w] = 0 }
        $unknown[$w]++
        continue
      }
      if ($text -notmatch '\+\*?([0-9,]+)\*?') { continue }
      $eff = Num $Matches[1]
      $over = 0.0; $abs = 0.0
      if ($text -match '\(O: ([0-9,]+)\)') { $over = Num $Matches[1] }
      if ($text -match '\(A: ([0-9,]+)\)') { $abs = Num $Matches[1] }
      if (-not $log.ContainsKey($spell)) { $log[$spell] = [pscustomobject]@{ eff = 0.0; raw = 0.0; hits = 0.0 } }
      $log[$spell].eff += $eff; $log[$spell].raw += $eff + $over + $abs; $log[$spell].hits += 1
    }
  }
  $minutes = $(if ($LogSeconds -gt 0) { $LogSeconds } else { $last }) / 60
  foreach ($k in @($logCasts.Keys)) { $logCasts[$k] /= $minutes }
  "Log: $($eventFiles.Count) event exports, $($seen.Count) events, {0:N1} min" -f $minutes
  if ($unknown.Count) { 'Not matched to a spell (first words): ' + (($unknown.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 8 | ForEach-Object { "$($_.Key) x$($_.Value)" }) -join '; ') }
} else {
  # summary tables: "Healing *.csv" (Name, Amount, Hits, Overheal) and "Casts *.csv" (Name, Amount, CPM)
  function Newest([string]$prefix) {
    $f = Get-ChildItem -LiteralPath $LogDir -Filter "$prefix *.csv" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $f) { throw "No event exports and no `"$prefix *.csv`" in $LogDir" }
    $f.FullName
  }
  function Count([string]$cell) { Num (($cell -split '\$')[0]) }
  $castRows = Import-Csv -LiteralPath (Newest 'Casts')
  $minutes = @($castRows | Where-Object { (Num $_.CPM) -gt 0 } | ForEach-Object { (Count $_.Amount) / (Num $_.CPM) }) |
    Sort-Object | Select-Object -Last 1
  if ($LogSeconds -gt 0) { $minutes = $LogSeconds / 60 }
  foreach ($r in $castRows) { if (-not $logCasts.ContainsKey($r.Name)) { $logCasts[$r.Name] = (Count $r.Amount) / $minutes } }
  foreach ($r in Import-Csv -LiteralPath (Newest 'Healing')) {
    $eff = Count $r.Amount
    $oh = $(if ($r.Overheal -match '([0-9.]+)%') { [double]$Matches[1] / 100 } else { 0.0 })
    $log[$r.Name] = [pscustomobject]@{ eff = $eff; raw = $(if ($oh -lt 1) { $eff / (1 - $oh) } else { $eff }); hits = (Num $r.Hits) }
  }
  "Log: summary tables, {0:N1} min" -f $minutes
}
$logEff = ($log.Values | Measure-Object -Property eff -Sum).Sum

# --- side by side ----------------------------------------------------------------------------------------------------
function Off([double]$a, [double]$b) { if ($a -le 0 -or $b -le 0) { return $false }; [math]::Abs($b / $a - 1) -gt $Tolerance }
function Fmt([double]$v, [string]$f) { if ($v -le 0) { '-' } else { $v.ToString($f) } }

"Sim: $Fight at {0:P0} of one healer's share ({1} fights, {2:N1} min)" -f $Level, $Iterations, $simMin
''
$fmt = '{0,-28} {1,7} {2,7} | {3,7} {4,7} | {5,8} {6,8} | {7,5} {8,5} | {9,6} {10,6}  {11}'
$fmt -f 'Spell', 'share', '', 'hits/m', '', 'raw/hit', '', 'oh', '', 'cast/m', '', ''
$fmt -f '', 'log', 'sim', 'log', 'sim', 'log', 'sim', 'log', 'sim', 'log', 'sim', 'off by more than ' + $Tolerance.ToString('P0')
$names = @($log.Keys) + @($sim.Keys) | Sort-Object -Unique
$rows = foreach ($n in $names) {
  $l = $log[$n]; $s = $sim[$n]
  [pscustomobject]@{
    name = $n
    lShare = $(if ($l) { $l.eff / $logEff } else { 0 }); sShare = $(if ($s) { $s.eff / $simEff } else { 0 })
    lHits = $(if ($l) { $l.hits / $minutes } else { 0 }); sHits = $(if ($s) { $s.hits / $simMin } else { 0 })
    lRaw = $(if ($l -and $l.hits) { $l.raw / $l.hits } else { 0 }); sRaw = $(if ($s -and $s.hits) { $s.raw / $s.hits } else { 0 })
    lOh = $(if ($l -and $l.raw) { 1 - $l.eff / $l.raw } else { -1 }); sOh = $(if ($s -and $s.raw) { 1 - $s.eff / $s.raw } else { -1 })
    lCast = $(if ($logCasts.ContainsKey($n)) { $logCasts[$n] } else { 0 }); sCast = $(if ($simCasts.ContainsKey($n)) { $simCasts[$n] } else { 0 })
  }
}
foreach ($r in $rows | Sort-Object { - [math]::Max($_.lShare, $_.sShare) }) {
  if ($r.lShare -lt 0.001 -and $r.sShare -lt 0.001) { continue }
  $flags = @()
  if (-not $log.ContainsKey($r.name)) { $flags += 'not in the log' } elseif (-not $sim.ContainsKey($r.name)) { $flags += 'not in the sim' }
  else {
    if (Off $r.lShare $r.sShare) { $flags += 'share' }
    if (Off $r.lHits $r.sHits) { $flags += 'hits' }
    if (Off $r.lRaw $r.sRaw) { $flags += 'size' }
    if ($r.lCast -gt 0 -and (Off $r.lCast $r.sCast)) { $flags += 'casts' }
  }
  $fmt -f $r.name, (Fmt $r.lShare 'P1'), (Fmt $r.sShare 'P1'), (Fmt $r.lHits 'N1'), (Fmt $r.sHits 'N1'), (Fmt $r.lRaw 'N0'), (Fmt $r.sRaw 'N0'),
    $(if ($r.lOh -ge 0) { $r.lOh.ToString('P0') } else { '-' }), $(if ($r.sOh -ge 0) { $r.sOh.ToString('P0') } else { '-' }),
    (Fmt $r.lCast 'N1'), $(if ($r.lCast -gt 0) { Fmt $r.sCast 'N1' } else { '-' }), ($flags -join ', ')
}
