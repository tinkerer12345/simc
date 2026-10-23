// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#pragma once

#include "config.hpp"

#include "util/timespan.hpp"

#include <deque>
#include <string>
#include <utility>
#include <vector>

struct action_t;
struct action_state_t;
struct player_t;

/* Opt-in record of what happened to a player in the seconds before each death (option death_recap=<seconds>).
 * While enabled, recent damage taken, healing received and waiting time are kept in a rolling window; when the
 * player dies the window is summarised per second and stored. Disabled (the default) it records nothing.
 */
struct death_recap_t
{
  // Damage taken from one ability during the window.
  struct ability_t
  {
    std::string source;
    std::string ability;
    std::string school;
    double amount   = 0;  // after mitigation and absorbs
    double absorbed = 0;
    int hits        = 0;
  };

  // One second of the window. Health and mana are the values at the end of the second.
  struct second_t
  {
    double damage   = 0;
    double absorbed = 0;
    double healing  = 0;
    double overheal = 0;
    double health   = 0;
    double mana     = 0;
    double idle     = 0;  // seconds spent waiting with nothing to do
  };

  struct death_t
  {
    double time       = 0;
    double max_health = 0;
    double max_mana   = 0;
    std::vector<second_t> seconds;     // oldest first; the last second ends at the death
    std::vector<ability_t> abilities;  // largest first
    ability_t killing_blow;
  };

  // Detailed deaths kept across all iterations; deaths beyond this are still counted by collected_data.deaths.
  static constexpr size_t max_deaths = 1000;

  timespan_t window = timespan_t::zero();  // 0 disables recording
  std::vector<death_t> deaths;

  bool enabled() const
  {
    return window > timespan_t::zero();
  }

  void reset();
  void merge( const death_recap_t& other );

  void record_damage( const player_t& p, const action_state_t& s );
  void record_heal( const player_t& p, const action_state_t& s );
  void record_wait( timespan_t start, timespan_t end );
  void record_death( const player_t& p, const action_state_t* killing_blow, timespan_t waiting_since );

private:
  struct event_t
  {
    timespan_t time;
    const action_t* action;
    bool heal;
    double amount;    // damage landed, or effective healing
    double absorbed;  // damage only
    double overheal;  // healing only
    double health;    // after the event
    double mana;      // after the event
  };

  std::deque<event_t> events;
  std::deque<std::pair<timespan_t, timespan_t>> waits;
  // Health and mana just before the oldest kept event; negative until something has been pruned.
  double baseline_health = -1;
  double baseline_mana   = -1;

  void record( const player_t& p, event_t e );
  void prune( timespan_t now );
};
