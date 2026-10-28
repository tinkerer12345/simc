// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
#ifndef SC_ENCOUNTER_DAMAGE_HPP
#define SC_ENCOUNTER_DAMAGE_HPP

#include "simulationcraft.hpp"

namespace bosses
{
/* Shared building blocks for boss damage in healer sims.
 *
 * Every boss ability carries a damage type, so healers can be tested against each kind of damage separately:
 * steady (constant raid-wide rot), burst (big raid-wide spikes), spot (hits on a few raiders), tank (tank
 * busters and melee) and avoidable (damage a clean raid would not take, at the rate the log shows).
 * Each type has a damage multiplier option (encounter_damage_steady / _burst / _spot / _tank / _avoidable, and
 * _all on top), all 1.0 by default.
 */
enum class damage_type_e
{
  STEADY,
  BURST,
  SPOT,
  TANK,
  AVOIDABLE,
};

const char* damage_type_string( damage_type_e type );

// The damage multiplier the sim options set for a damage type (its own option times encounter_damage_all).
double damage_type_multiplier( const sim_t& sim, damage_type_e type );

// Living, non-pet players, the action's target first.
size_t available_player_targets( const action_t* action, std::vector<player_t*>& target_list );

// A hand-coded spell for a fixed amount, scaled by its damage type's multiplier. By default it hits every living
// player; random_targets( n ) makes it hit n random living players instead, and single_target() only the target
// it is executed on (e.g. the active tank).
struct raid_damage_t : public spell_t
{
  damage_type_e damage_type;
  bool random = false;

  // Create with non-zero damage: SimC only applies target mitigation to actions that have direct damage at init.
  raid_damage_t( util::string_view name, player_t* p, school_e damage_school, double damage, damage_type_e type );

  // SimC rolls between min and max (max defaults to 0), so both must be set.
  void set_damage( double damage );

  raid_damage_t* random_targets( int n );
  raid_damage_t* single_target();

  // The target list is cached, and enemy actions are never activate()d, so nothing refreshes it when a
  // player dies; dead players would keep taking damage. Rebuild it on every cast instead.
  void execute() override;

  size_t available_targets( std::vector<player_t*>& target_list ) const override;

  // The damage type's multiplier applies before mitigation, like a damage buff on the boss.
  double composite_da_multiplier( const action_state_t* s ) const override;
};
}  // namespace bosses

#endif
