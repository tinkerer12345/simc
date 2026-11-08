// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#pragma once

#include "config.hpp"
#include "util/timespan.hpp"

#include <map>
#include <string>

struct player_t;
struct sim_t;

// Measurements at a fixed damage level (tolerance's level runs, tolerance_stats=1): how the raid's health moves around
// the encounter's bursts (its encounter.burst timer), healing and damage in burst windows vs quiet time, and what
// killed people. Each sim collects its own fights; thread sims are merged into their parent.
namespace tolerance
{
// Sums over bursts.
struct burst_totals_t
{
  double count         = 0;
  double raid_before   = 0;  // the living raid's average health (fraction of max) just before the burst
  double raid_low      = 0;  // its lowest average health in the burst window
  double player_low    = 0;  // the lowest living player's health
  double recovered     = 0;  // bursts after which the living raid got back to within recovery_margin of raid_before
                             // before the next one
  double recovery_time = 0;  // seconds from the burst to that point (recovered bursts only)
  double deaths        = 0;  // players who died in the burst window

  void merge( const burst_totals_t& o );
};

// Sums over the fights' burst windows (burst_window seconds from each burst) or the rest of the fight.
struct window_totals_t
{
  double seconds      = 0;
  double damage_taken = 0;  // by the whole raid
  double healing      = 0;  // received by the raid (heals + absorbs, effective): the healer under test's

  void merge( const window_totals_t& o );
};

struct fight_stats_t
{
  // Options (tolerance_stats, tolerance_recovery_margin, tolerance_burst_window)
  bool enabled              = false;
  double recovery_margin    = 0.05;
  double burst_window       = 10;  // seconds

  // Totals over the collected fights
  double fights = 0;
  double deaths = 0;
  // Raid health kept: the raid's average health (a dead player counts as 0) over time, summed with the seconds it
  // covers; the gear check's score.
  double health_time    = 0;
  double health_seconds = 0;
  burst_totals_t normal, big;
  window_totals_t in_burst, quiet;
  std::map<std::string, double> death_causes;  // "burst", "spot", "spot during a burst", ...

  void combat_begin( sim_t* sim );
  void combat_end( sim_t* sim, bool collect );
  // Called by encounter damage when its hit kills a player.
  // Called by encounter damage when its hit takes a player to 0 health (the death itself is an event later).
  void record_death( sim_t* sim, const player_t* victim, const std::string& damage_type );
  void merge( const fight_stats_t& o );

private:
  // This fight's
  struct open_burst_t
  {
    bool open           = false;
    bool big            = false;
    timespan_t start    = timespan_t::zero();
    double raid_before  = 1;
    double raid_low     = 1;
    double player_low   = 1;
    bool dipped         = false;  // the living raid fell below raid_before - recovery_margin
    timespan_t recovered_at = timespan_t::min();
    double deaths       = 0;
  };
  open_burst_t burst;
  burst_totals_t fight_normal, fight_big;
  window_totals_t fight_burst_window, fight_quiet;
  std::map<std::string, double> fight_causes;
  double fight_deaths     = 0;
  double fight_health_time    = 0;
  double fight_health_seconds = 0;
  double last_ago         = -1;
  double last_avg         = 1;  // the living raid's average health at the last sample
  double last_damage      = 0;
  double last_healing     = 0;
  timespan_t last_sample  = timespan_t::zero();

  void sample( sim_t* sim );
  void close_burst( bool at_fight_end, timespan_t now );
  void schedule_sample( sim_t* sim );
  bool in_burst_window( sim_t* sim ) const;
};
}  // namespace tolerance
