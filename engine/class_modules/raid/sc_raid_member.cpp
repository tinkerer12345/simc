// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "class_modules/raid/sc_raid_member.hpp"

namespace raid
{
namespace
{
// Healing from a stand-in healer: a fixed amount on the lowest-health living player.
struct stand_in_heal_t final : public heal_t
{
  stand_in_heal_t( player_t* p, double amount ) : heal_t( "raid_healing", p, spell_data_t::nil() )
  {
    school      = SCHOOL_HOLY;
    background  = true;
    may_crit    = false;
    may_miss    = false;
    trigger_gcd = 0_ms;
    base_dd_min = amount;
    base_dd_max = amount;
  }
};

/* A stand-in raid member for healer sims: it has health and damage reduction, takes damage, can be healed
 * and can die, but has no rotation, gear or procs, so it costs almost nothing to simulate. Stand-in healers
 * also heal the lowest-health player every second. Settings come from sim_t::raid_stand_in.
 */
struct raid_member_t final : public player_t
{
  stand_in_heal_t* healing = nullptr;

  // RACE_NONE like simplified_player_t: no racial passives (a race also crashes the borrowed base stat lookup).
  raid_member_t( sim_t* sim, util::string_view name, role_e member_role, util::string_view position )
    : player_t( sim, PLAYER_SIMPLIFIED, name, RACE_NONE )
  {
    resource_regeneration                          = regen_type::DISABLED;
    role                                           = member_role;
    _spec                                          = SPEC_PET;
    position_str                                   = std::string( position );
    resources.infinite_resource[ RESOURCE_HEALTH ] = false;  // raid members can die
    // Keep the default level: sim->max_player_level is still -1 while the profile is being parsed.
  }

  role_e primary_role() const override
  {
    return role;
  }

  // No consumables: these would otherwise be created with empty names and fail to initialise.
  std::string default_potion() const override { return "disabled"; }
  std::string default_flask() const override { return "disabled"; }
  std::string default_food() const override { return "disabled"; }
  std::string default_rune() const override { return "disabled"; }
  std::string default_temporary_enchant() const override { return "disabled"; }

  void init_base_stats() override
  {
    // PLAYER_SIMPLIFIED has no base stat data, so borrow a Priest's (as simplified_player_t does).
    type = PRIEST;
    player_t::init_base_stats();
    type = PLAYER_SIMPLIFIED;

    // Health comes only from the configured pool; stamina buffs from the raid still add on top.
    base.stats.attribute[ ATTR_STAMINA ] = 0;
    resources.base[ RESOURCE_HEALTH ] =
        role == ROLE_TANK ? sim->raid_stand_in.tank_health : sim->raid_stand_in.health;
  }

  void create_actions() override
  {
    player_t::create_actions();

    if ( role == ROLE_HEAL && sim->raid_stand_in.healer_hps > 0 )
      healing = new stand_in_heal_t( this, sim->raid_stand_in.healer_hps );
  }

  void combat_begin() override
  {
    player_t::combat_begin();

    if ( healing )
      schedule_healing();
  }

  void schedule_healing()
  {
    make_event( *sim, 1_s, [ this ] {
      if ( !is_sleeping() )
      {
        player_t* lowest = nullptr;
        for ( player_t* p : sim->player_no_pet_list )
        {
          if ( !p->is_sleeping() && p->health_percentage() < 100 &&
               ( !lowest || p->health_percentage() < lowest->health_percentage() ) )
            lowest = p;
        }
        if ( lowest )
          healing->execute_on_target( lowest );
      }
      schedule_healing();
    } );
  }

  double composite_mitigation_multiplier( const action_state_t* s, school_e school, bool direct ) const override
  {
    double m = player_t::composite_mitigation_multiplier( s, school, direct );

    m *= 1.0 - ( role == ROLE_TANK ? sim->raid_stand_in.tank_damage_reduction : sim->raid_stand_in.damage_reduction );
    if ( s->action && s->action->is_aoe() )
      m *= 1.0 - sim->raid_stand_in.avoidance;

    return m;
  }
};
}  // namespace

void create_raid_members( sim_t* sim, util::string_view kind, int count )
{
  struct kind_t
  {
    util::string_view name, label, position;
    role_e role;
  };
  static constexpr kind_t kinds[] = {
    { "tank", "Tank", "front", ROLE_TANK },
    { "healer", "Healer", "ranged_back", ROLE_HEAL },
    { "ranged", "Ranged", "ranged_back", ROLE_DPS },
    { "melee", "Melee", "back", ROLE_DPS },
  };

  auto it = std::find_if( std::begin( kinds ), std::end( kinds ), [ kind ]( const kind_t& k ) { return k.name == kind; } );
  if ( it == std::end( kinds ) )
    throw std::invalid_argument( fmt::format( "Unknown raid member kind '{}'.", kind ) );

  for ( int i = 1; i <= count; ++i )
  {
    player_t* member = new raid_member_t( sim, fmt::format( "{}_{}", it->label, i ), it->role, it->position );
    member->create_options();
  }
  sim->raid_stand_in.members += count;
}
}  // namespace raid
