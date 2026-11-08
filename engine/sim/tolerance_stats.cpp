// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "tolerance_stats.hpp"

#include "player/player.hpp"
#include "sim/event.hpp"
#include "sim/sim.hpp"

#include <algorithm>
#include <cmath>

namespace tolerance
{
namespace
{
constexpr timespan_t sample_interval = 250_ms;
// A burst that starts this close to the fight's end and hasn't recovered yet is left out: the fight ended, not the
// healer.
constexpr timespan_t min_burst_at_end = 20_s;

bool is_raid_member( const player_t* p )
{
  return !p->is_enemy() && !p->is_pet();
}

double health_fraction( const player_t* p )
{
  double max = p->resources.max[ RESOURCE_HEALTH ];
  return max > 0 ? p->resources.current[ RESOURCE_HEALTH ] / max : 1.0;
}

// Seconds since the encounter's last event of that name, -1 if it has none (or none yet).
double ago( sim_t* sim, util::string_view name )
{
  if ( !sim->encounter_timer )
    return -1;
  double a = sim->encounter_timer( name, true );
  return a < 0 || a >= 9999 ? -1 : a;
}

double raid_damage_taken( sim_t* sim )
{
  double d = 0;
  for ( const player_t* p : sim->player_no_pet_list )
    if ( is_raid_member( p ) )
      d += p->iteration_dmg_taken;
  return d;
}

// Healing the raid received: the healer under test is the only one healing (SimC adds up the healer's own healing
// only at the end of a fight).
double raid_healing_taken( sim_t* sim )
{
  double h = 0;
  for ( const player_t* p : sim->player_no_pet_list )
    if ( is_raid_member( p ) )
      h += p->iteration_heal_taken + p->iteration_absorb_taken;
  return h;
}

void add( std::map<std::string, double>& to, const std::map<std::string, double>& from )
{
  for ( const auto& [ k, v ] : from )
    to[ k ] += v;
}
}  // namespace

void burst_totals_t::merge( const burst_totals_t& o )
{
  count += o.count;
  raid_before += o.raid_before;
  raid_low += o.raid_low;
  player_low += o.player_low;
  recovered += o.recovered;
  recovery_time += o.recovery_time;
  deaths += o.deaths;
}

void window_totals_t::merge( const window_totals_t& o )
{
  seconds += o.seconds;
  damage_taken += o.damage_taken;
  healing += o.healing;
}

void fight_stats_t::merge( const fight_stats_t& o )
{
  fights += o.fights;
  deaths += o.deaths;
  health_time += o.health_time;
  health_seconds += o.health_seconds;
  normal.merge( o.normal );
  big.merge( o.big );
  in_burst.merge( o.in_burst );
  quiet.merge( o.quiet );
  add( death_causes, o.death_causes );
}

void fight_stats_t::combat_begin( sim_t* sim )
{
  if ( !enabled )
    return;

  burst              = open_burst_t();
  fight_normal       = burst_totals_t();
  fight_big          = burst_totals_t();
  fight_burst_window = window_totals_t();
  fight_quiet        = window_totals_t();
  fight_causes.clear();
  fight_deaths = 0;
  fight_health_time    = 0;
  fight_health_seconds = 0;
  last_ago     = -1;
  last_avg     = 1;
  last_damage  = raid_damage_taken( sim );
  last_healing = raid_healing_taken( sim );
  last_sample  = sim->current_time();
  schedule_sample( sim );
}

void fight_stats_t::schedule_sample( sim_t* sim )
{
  make_event( *sim, sample_interval, [ this, sim ] {
    sample( sim );
    schedule_sample( sim );
  } );
}

bool fight_stats_t::in_burst_window( sim_t* sim ) const
{
  return burst.open && sim->current_time() - burst.start < timespan_t::from_seconds( burst_window );
}

void fight_stats_t::sample( sim_t* sim )
{
  timespan_t now = sim->current_time();

  double sum = 0, low = 1, all = 0;
  int living = 0, members = 0;
  for ( const player_t* p : sim->player_no_pet_list )
  {
    if ( !is_raid_member( p ) )
      continue;
    members++;
    if ( p->is_sleeping() )
      continue;
    double h = std::max( 0.0, health_fraction( p ) );
    all += h;
    sum += h;
    low = std::min( low, h );
    living++;
  }
  double avg = living ? sum / living : 0;

  // A new burst: the time since the last one went back down. It landed since the last sample, so the raid's health
  // then is the health before it.
  double a = ago( sim, "burst" );
  if ( a >= 0 && ( last_ago < 0 || a < last_ago ) )
  {
    close_burst( false, now );
    burst.open        = true;
    burst.start       = now - timespan_t::from_seconds( a );
    burst.raid_before = last_avg;
    double b          = ago( sim, "big_burst" );
    burst.big         = b >= 0 && std::abs( b - a ) < 0.01;
  }
  last_ago = a;
  last_avg = avg;

  // Damage and healing since the last sample, booked to the window the sample is in (a burst that landed since the
  // last sample opened its window above).
  double damage  = raid_damage_taken( sim );
  double healing = raid_healing_taken( sim );
  auto& w        = in_burst_window( sim ) ? fight_burst_window : fight_quiet;
  double dt      = ( now - last_sample ).total_seconds();
  w.seconds += dt;
  if ( members )
  {
    fight_health_time += dt * all / members;
    fight_health_seconds += dt;
  }
  w.damage_taken += damage - last_damage;
  w.healing += healing - last_healing;
  last_damage  = damage;
  last_healing = healing;
  last_sample  = now;

  if ( !burst.open || living == 0 )
    return;

  // The low points and the dips count within the burst window; after it, only getting back up counts.
  double target = burst.raid_before - recovery_margin;
  if ( in_burst_window( sim ) )
  {
    burst.player_low = std::min( burst.player_low, low );
    burst.raid_low   = std::min( burst.raid_low, avg );
    if ( avg < target )
    {
      burst.dipped       = true;
      burst.recovered_at = timespan_t::min();
      return;
    }
  }
  if ( burst.dipped && burst.recovered_at == timespan_t::min() && avg >= target )
    burst.recovered_at = now;
}

void fight_stats_t::close_burst( bool at_fight_end, timespan_t now )
{
  if ( !burst.open )
    return;
  bool recovered = !burst.dipped || burst.recovered_at != timespan_t::min();
  if ( !( at_fight_end && !recovered && now - burst.start < min_burst_at_end ) )
  {
    auto& t = burst.big ? fight_big : fight_normal;
    t.count++;
    t.raid_before += burst.raid_before;
    t.raid_low += burst.raid_low;
    t.player_low += burst.player_low;
    t.deaths += burst.deaths;
    if ( recovered )
    {
      t.recovered++;
      if ( burst.dipped )
        t.recovery_time += ( burst.recovered_at - burst.start ).total_seconds();
    }
  }
  burst = open_burst_t();
}

void fight_stats_t::record_death( sim_t* sim, const player_t* victim, const std::string& damage_type )
{
  if ( !enabled || !is_raid_member( victim ) )
    return;
  fight_deaths++;
  bool during_burst = in_burst_window( sim );
  if ( during_burst )
    burst.deaths++;
  std::string cause = damage_type;
  if ( during_burst && damage_type != "burst" )
    cause += " during a burst";
  fight_causes[ cause ]++;
}

void fight_stats_t::combat_end( sim_t* sim, bool collect )
{
  if ( !enabled )
    return;
  sample( sim );
  close_burst( true, sim->current_time() );
  // A wipe ends the fight early: the rest of it counts as a dead raid.
  double rest = ( sim->expected_iteration_time - sim->current_time() ).total_seconds();
  if ( rest > 0 )
    fight_health_seconds += rest;
  if ( !collect )
    return;

  fights++;
  deaths += fight_deaths;
  health_time += fight_health_time;
  health_seconds += fight_health_seconds;
  normal.merge( fight_normal );
  big.merge( fight_big );
  in_burst.merge( fight_burst_window );
  quiet.merge( fight_quiet );
  add( death_causes, fight_causes );
}
}  // namespace tolerance
