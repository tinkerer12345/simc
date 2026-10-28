// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "class_modules/bosses/sc_encounter_damage.hpp"

namespace bosses
{
const char* damage_type_string( damage_type_e type )
{
  switch ( type )
  {
    case damage_type_e::STEADY: return "steady";
    case damage_type_e::BURST: return "burst";
    case damage_type_e::SPOT: return "spot";
    case damage_type_e::TANK: return "tank";
    case damage_type_e::AVOIDABLE: return "avoidable";
  }
  return "unknown";
}

double damage_type_multiplier( const sim_t& sim, damage_type_e type )
{
  double m = sim.encounter_damage_all;
  switch ( type )
  {
    case damage_type_e::STEADY: m *= sim.encounter_damage_steady; break;
    case damage_type_e::BURST: m *= sim.encounter_damage_burst; break;
    case damage_type_e::SPOT: m *= sim.encounter_damage_spot; break;
    case damage_type_e::TANK: m *= sim.encounter_damage_tank; break;
    case damage_type_e::AVOIDABLE: m *= sim.encounter_damage_avoidable; break;
  }
  return m;
}

size_t available_player_targets( const action_t* action, std::vector<player_t*>& target_list )
{
  target_list.clear();

  if ( action->target && !action->target->is_sleeping() && !action->target->is_enemy() &&
       !action->target->is_pet() )
    target_list.push_back( action->target );

  auto add_target = [ action, &target_list ]( player_t* actor ) {
    if ( !actor->is_sleeping() && !actor->is_enemy() && !actor->is_pet() && actor != action->target )
      target_list.push_back( actor );
  };

  if ( action->sim->single_actor_batch )
  {
    add_target( action->sim->player_no_pet_list[ action->sim->current_index ] );
  }
  else
  {
    for ( player_t* actor : action->sim->player_no_pet_list )
      add_target( actor );
  }

  return target_list.size();
}

raid_damage_t::raid_damage_t( util::string_view name, player_t* p, school_e damage_school, double damage,
                              damage_type_e type )
  : spell_t( name, p, spell_data_t::nil() ), damage_type( type )
{
  school            = damage_school;
  base_execute_time = 0_ms;
  aoe               = -1;
  may_crit          = false;
  may_miss          = false;
  background        = true;
  trigger_gcd       = 0_ms;
  set_damage( damage );
}

void raid_damage_t::set_damage( double damage )
{
  base_dd_min = damage;
  base_dd_max = damage;
}

raid_damage_t* raid_damage_t::random_targets( int n )
{
  aoe    = n;
  random = true;
  return this;
}

raid_damage_t* raid_damage_t::single_target()
{
  aoe = 0;
  return this;
}

void raid_damage_t::execute()
{
  target_cache.is_valid = false;
  spell_t::execute();
}

size_t raid_damage_t::available_targets( std::vector<player_t*>& target_list ) const
{
  available_player_targets( this, target_list );

  // n random players: shuffle, and aoe takes the first n
  if ( random )
    rng().shuffle( target_list.begin(), target_list.end() );

  return target_list.size();
}

double raid_damage_t::composite_da_multiplier( const action_state_t* s ) const
{
  return spell_t::composite_da_multiplier( s ) * damage_type_multiplier( *sim, damage_type );
}
}  // namespace bosses
