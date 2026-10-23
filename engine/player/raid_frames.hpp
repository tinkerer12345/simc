// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#pragma once

#include "config.hpp"

#include <string>
#include <vector>

struct action_state_t;
struct player_t;

/* Opt-in raid frames for a healer (option raid_frames=1). In the sample fight (the same iteration as the
 * action sequence) it records every player's health, absorbs and alive state once per second and at each
 * death, together with the healer's HoTs on them. Disabled (the default) it records nothing.
 */
struct raid_frames_t
{
  struct unit_t
  {
    std::string name;
    std::string role;      // tank / heal / dps ...
    std::string position;  // front / back / ranged_back ...
    double max_health = 0;
  };

  // A HoT the owner has on a unit.
  struct effect_t
  {
    std::string name;
    double remains = 0;  // seconds; negative for no duration
    int stacks     = 0;
    bool hot       = false;
  };

  struct snapshot_t
  {
    double time = 0;
    std::vector<double> health;   // per unit, same order as units
    std::vector<double> absorb;
    std::vector<bool> alive;
    std::vector<std::vector<effect_t>> effects;
  };

  struct death_t
  {
    double time = 0;
    std::string name;
    std::string ability;  // killing blow
    std::string source;
    double amount   = 0;
    size_t snapshot = 0;  // index of the snapshot taken at the death
  };

  bool enabled = false;
  std::vector<unit_t> units;
  std::vector<snapshot_t> snapshots;
  std::vector<death_t> deaths;

  void combat_begin( player_t& owner );
  void record_death( player_t& owner, const player_t& dead, const action_state_t* killing_blow );
  void merge( const raid_frames_t& other );

private:
  bool recording( const player_t& owner ) const;
  void take_snapshot( const player_t& owner );
  void schedule_snapshot( player_t& owner );
};
