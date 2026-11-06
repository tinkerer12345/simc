// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "class_modules/raid/sc_raid_member.hpp"

namespace raid
{
namespace
{
/* A stand-in raid member for healer sims: it has health and damage reduction, takes damage, can be healed
 * and can die, but has no rotation, gear or procs, so it costs almost nothing to simulate. The healer under
 * test is the only one who heals (stand-in healers are names only). Settings come from sim_t::raid_stand_in.
 */
struct raid_member_t final : public player_t
{
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
    resources.base[ RESOURCE_HEALTH ] = member_health( sim, role == ROLE_TANK );
  }

  // With raid_health_from_healer=1 the health is only known once the healer (earlier in the actor list) is set up.
  void init_finished() override
  {
    player_t::init_finished();
    resolve_health( sim );
    resources.base[ RESOURCE_HEALTH ] = member_health( sim, role == ROLE_TANK );
  }

  static void resolve_health( sim_t* sim );

  double composite_mitigation_multiplier( const action_state_t* s, school_e school, bool direct ) const override
  {
    double m = player_t::composite_mitigation_multiplier( s, school, direct );

    m *= 1.0 - ( role == ROLE_TANK ? sim->raid_stand_in.tank_damage_reduction : sim->raid_stand_in.damage_reduction );
    if ( role == ROLE_TANK && school == SCHOOL_PHYSICAL )
      m *= 1.0 - sim->raid_stand_in.tank_physical_reduction;
    if ( s->action && s->action->is_aoe() )
      m *= 1.0 - sim->raid_stand_in.avoidance;

    return m;
  }
};

// Reads the healer's max health once per sim, at init (before any worker thread starts). A child sim (a worker thread,
// or a tolerance step with another gear set) takes its parent's, so every gear set in a run faces the same raid and
// damage: more Stamina on an item doesn't make the fight hit harder.
void raid_member_t::resolve_health( sim_t* sim )
{
  auto& rs = sim->raid_stand_in;
  if ( !rs.health_from_healer || rs.resolved_health > 0 )
    return;

  for ( const sim_t* s = sim->parent; s; s = s->parent )
  {
    if ( s->raid_stand_in.resolved_health > 0 )
    {
      rs.resolved_health = s->raid_stand_in.resolved_health;
      return;
    }
  }

  // The healer: the first player that isn't a stand-in.
  auto it = range::find_if( sim->player_no_pet_list, []( player_t* p ) {
    return !p->is_enemy() && !dynamic_cast<raid_member_t*>( p );
  } );
  if ( it == sim->player_no_pet_list.end() )
    throw std::invalid_argument( "raid_health_from_healer=1 needs a player (the healer) besides the stand-ins." );

  // Max health from the healer's gear and talents, outside of combat buffs. Before the first reset its current stats
  // are still empty, so compute with its initial stats as reset() will (and put everything back).
  player_t* healer = *it;
  auto current     = healer->current;
  healer->current  = healer->initial;
  healer->cache.invalidate_all();
  healer->init_resources( true );
  rs.resolved_health = healer->resources.max[ RESOURCE_HEALTH ];
  healer->current = current;
  healer->cache.invalidate_all();
  sim->print_debug( "Raid stand-in health from {}: {}", *healer, rs.resolved_health );
}
}  // namespace

double member_health( const sim_t* sim, bool tank )
{
  const auto& rs = sim->raid_stand_in;
  if ( !rs.health_from_healer || rs.resolved_health <= 0 )
    return tank ? rs.tank_health : rs.health;
  return tank ? rs.resolved_health * rs.tank_health_factor : rs.resolved_health;
}

void create_raid_members( sim_t* sim, util::string_view kind, int count )
{
  // Stand-ins are named after a spec so raid frames read like a real raid (the report colours them by class).
  // Healers are names only: they take damage like everyone else but do not heal.
  struct kind_t
  {
    util::string_view name, position;
    role_e role;
    std::vector<util::string_view> specs;
  };
  static const kind_t kinds[] = {
    { "tank", "front", ROLE_TANK,
      { "Protection_Warrior", "Blood_Death_Knight", "Vengeance_Demon_Hunter", "Guardian_Druid", "Protection_Paladin",
        "Brewmaster_Monk" } },
    { "healer", "ranged_back", ROLE_HEAL,
      { "Holy_Priest", "Preservation_Evoker", "Restoration_Shaman", "Holy_Paladin", "Mistweaver_Monk",
        "Discipline_Priest" } },
    { "ranged", "ranged_back", ROLE_DPS,
      { "Balance_Druid", "Frost_Mage", "Devastation_Evoker", "Affliction_Warlock", "Beast_Mastery_Hunter",
        "Elemental_Shaman", "Shadow_Priest", "Fire_Mage", "Destruction_Warlock", "Augmentation_Evoker",
        "Marksmanship_Hunter", "Arcane_Mage", "Demonology_Warlock" } },
    { "melee", "back", ROLE_DPS,
      { "Havoc_Demon_Hunter", "Fury_Warrior", "Retribution_Paladin", "Unholy_Death_Knight", "Windwalker_Monk",
        "Outlaw_Rogue", "Enhancement_Shaman", "Feral_Druid", "Survival_Hunter", "Assassination_Rogue",
        "Frost_Death_Knight", "Arms_Warrior", "Subtlety_Rogue" } },
  };

  auto it = std::find_if( std::begin( kinds ), std::end( kinds ), [ kind ]( const kind_t& k ) { return k.name == kind; } );
  if ( it == std::end( kinds ) )
    throw std::invalid_argument( fmt::format( "Unknown raid member kind '{}'.", kind ) );

  for ( int i = 0; i < count; ++i )
  {
    // Past the end of the list, specs repeat with a number.
    const auto& spec = it->specs[ i % it->specs.size() ];
    std::string name = i < static_cast<int>( it->specs.size() )
                           ? std::string( spec )
                           : fmt::format( "{}_{}", spec, i / it->specs.size() + 1 );
    player_t* member = new raid_member_t( sim, name, it->role, it->position );
    member->create_options();
  }
  sim->raid_stand_in.members += count;
}
}  // namespace raid
