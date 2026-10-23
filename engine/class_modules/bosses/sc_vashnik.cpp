// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "class_modules/bosses/sc_vashnik.hpp"
#include "class_modules/sc_enemy.hpp"

namespace bosses
{
namespace
{
constexpr timespan_t fountain_activation_time = 27_s;
// Deaths are scheduled from the observed kill order rather than modeled from add DPS.
constexpr double burning_venom_health = 1000000000;

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

struct burning_presence_t final : public spell_t
{
  burning_presence_t( player_t* p )
    : spell_t( "burning_presence", p, spell_data_t::nil() )
  {
    school            = SCHOOL_FIRE;
    base_execute_time = 0_ms;
    base_dd_min       = 33336;
    aoe               = -1;
    may_crit          = false;
    may_miss          = false;
    trigger_gcd       = 0_ms;
  }

  void init() override
  {
    spell_t::init();
    cooldown->duration = 3_s;
  }

  size_t available_targets( std::vector<player_t*>& target_list ) const override
  {
    return available_player_targets( this, target_list );
  }
};

struct caustic_surge_t final : public spell_t
{
  caustic_surge_t( player_t* p, util::string_view name, double damage )
    : spell_t( name, p, spell_data_t::nil() )
  {
    school            = SCHOOL_FIRE;
    base_execute_time = 0_ms;
    base_dd_min       = damage;
    aoe               = -1;
    may_crit          = false;
    may_miss          = false;
    background        = true;
    trigger_gcd       = 0_ms;
  }

  size_t available_targets( std::vector<player_t*>& target_list ) const override
  {
    return available_player_targets( this, target_list );
  }
};
}  // namespace

struct toxic_vapor_t final : public spell_t
{
  int& stacks;
  double damage_per_stack;

  toxic_vapor_t( player_t* p, util::string_view options_str, int& stacks )
    : spell_t( "toxic_vapor", p, spell_data_t::nil() ), stacks( stacks ), damage_per_stack( 10000 )
  {
    school            = SCHOOL_NATURE;
    base_execute_time = 2_s;
    base_dd_min       = damage_per_stack;
    aoe               = -1;
    may_crit          = false;

    add_option( opt_float( "damage", damage_per_stack ) );
    add_option( opt_timespan( "attack_speed", base_execute_time.base ) );
    parse_options( options_str );
  }

  void init() override
  {
    base_dd_min = damage_per_stack;
    spell_t::init();
    base_dd_max = base_dd_min;
  }

  size_t available_targets( std::vector<player_t*>& target_list ) const override
  {
    return available_player_targets( this, target_list );
  }

  bool ready() override
  {
    return stacks > 0 && spell_t::ready();
  }

  void execute() override
  {
    base_dd_min = damage_per_stack * stacks;
    base_dd_max = base_dd_min;
    spell_t::execute();
  }
};

struct vashnik_t;

struct burning_venom_t final : public pet_t
{
  bool caustic_surge_applied;
  caustic_surge_t* caustic_surge_action;
  caustic_surge_t* caustic_surge_tick_action;

  burning_venom_t( sim_t* sim, player_t* owner, util::string_view name )
    : pet_t( sim, owner, name, PET_ENEMY ),
      caustic_surge_applied( false ),
      caustic_surge_action( nullptr ),
      caustic_surge_tick_action( nullptr )
  {
    initial.sleeping = true;
    true_level       = sim->max_player_level + 3;
  }

  void reset() override;
  void demise() override;

  void spawn()
  {
    initial.sleeping = false;
    arise();
  }

  resource_e primary_resource() const override
  {
    return RESOURCE_HEALTH;
  }

  void init_resources( bool force ) override
  {
    pet_t::init_resources( force );
    resources.base[ RESOURCE_HEALTH ] = burning_venom_health;
    resources.initial[ RESOURCE_HEALTH ] = burning_venom_health;
    resources.max[ RESOURCE_HEALTH ] = burning_venom_health;
    resources.current[ RESOURCE_HEALTH ] = burning_venom_health;
  }

  bool receives_raid_debuffs() const override
  {
    return false;
  }

  bool receives_buffs() const override
  {
    return false;
  }

  action_t* create_action( util::string_view name, util::string_view options_str ) override
  {
    if ( name == "burning_presence" )
      return new burning_presence_t( this );

    return pet_t::create_action( name, options_str );
  }

  void init_action_list() override
  {
    if ( action_list_str.empty() )
      action_list_str = "/burning_presence";

    pet_t::init_action_list();

    if ( !caustic_surge_action )
      caustic_surge_action = new caustic_surge_t( this, "caustic_surge", 100009 );
    if ( !caustic_surge_tick_action )
      caustic_surge_tick_action = new caustic_surge_t( this, "caustic_surge_tick", 83341 );
  }

  void trigger_caustic_surge()
  {
    if ( !caustic_surge_action || !caustic_surge_tick_action )
      throw sc_runtime_error( fmt::format( "{}'s Caustic Surge actions were not initialized.", *this ) );

    caustic_surge_action->execute();
    for ( int tick = 1; tick <= 3; ++tick )
    {
      make_event( *sim, timespan_t::from_seconds( tick ), [ this ] {
        caustic_surge_tick_action->execute();
      } );
    }
  }
};

struct imbibe_t final : public spell_t
{
  int& toxic_vapor_stacks;
  int cast_count;
  double damage_per_cast;

  imbibe_t( player_t* p, util::string_view options_str, int& stacks )
    : spell_t( "imbibe", p, spell_data_t::nil() ),
      toxic_vapor_stacks( stacks ),
      cast_count( 0 ),
      damage_per_cast( 100000 )
  {
    school            = SCHOOL_SHADOWFLAME;
    base_execute_time = 4_s;
    base_dd_min       = damage_per_cast;
    aoe               = -1;
    may_crit          = false;
    background        = true;

    add_option( opt_float( "damage", damage_per_cast ) );
    parse_options( options_str );
  }

  void init() override
  {
    base_dd_min = damage_per_cast;
    spell_t::init();
    base_dd_max = base_dd_min;
  }

  size_t available_targets( std::vector<player_t*>& target_list ) const override
  {
    return available_player_targets( this, target_list );
  }

  void execute() override
  {
    ++cast_count;
    ++toxic_vapor_stacks;
    base_dd_min = damage_per_cast * cast_count;
    base_dd_max = base_dd_min;
    spell_t::execute();
  }

  void reset() override
  {
    spell_t::reset();
    cast_count = 0;
  }
};

struct vashnik_t final : public enemy::enemy_t
{
  int toxic_vapor_stacks;
  imbibe_t* imbibe_action;
  std::vector<burning_venom_t*> burning_venoms;

  explicit vashnik_t( sim_t* sim )
    : enemy::enemy_t( sim, "Vashnik" ),
      toxic_vapor_stacks( 0 ),
      imbibe_action( nullptr )
  {
    role = ROLE_NONE;

    for ( int i = 1; i <= 3; ++i )
      burning_venoms.push_back( new burning_venom_t( sim, this, fmt::format( "Burning_Venom_{}", i ) ) );
  }

  void reset() override
  {
    enemy::enemy_t::reset();
    toxic_vapor_stacks = 0;
  }

  role_e primary_role() const override
  {
    return ROLE_NONE;
  }

  bool receives_raid_debuffs() const override
  {
    return false;
  }

  bool receives_buffs() const override
  {
    return false;
  }

  action_t* create_action( util::string_view name, util::string_view options_str ) override
  {
    if ( name == "toxic_vapor" )
      return new toxic_vapor_t( this, options_str, toxic_vapor_stacks );
    if ( name == "imbibe" )
    {
      imbibe_action = new imbibe_t( this, options_str, toxic_vapor_stacks );
      return imbibe_action;
    }

    return enemy::enemy_t::create_action( name, options_str );
  }

  void init_action_list() override
  {
    if ( action_list_str.empty() )
      // Prototype encounter values until verified spell data is available.
      action_list_str = "/imbibe/toxic_vapor,damage=10000,attack_speed=2";

    enemy::enemy_t::init_action_list();

    if ( !imbibe_action )
      imbibe_action = new imbibe_t( this, {}, toxic_vapor_stacks );
  }

  void combat_begin() override
  {
    enemy::enemy_t::combat_begin();
    schedule_imbibe( 20_s );
    schedule_burning_venoms();
  }

  void schedule_imbibe( timespan_t delay )
  {
    make_event( *sim, delay, [ this ] {
      if ( !imbibe_action )
        throw sc_runtime_error( "Vashnik's Imbibe action was not initialized." );

      imbibe_action->schedule_execute();
      schedule_imbibe( 80_s );
    } );
  }

  void schedule_burning_venoms()
  {
    make_event( *sim, fountain_activation_time, [ this ] {
      // These staggered delays reproduce the non-overlapping deaths in the supplied log.
      constexpr timespan_t kill_delays[] = { 37.264_s, 7.776_s, 22.105_s };

      for ( size_t i = 0; i < burning_venoms.size(); ++i )
      {
        burning_venom_t* add = burning_venoms[ i ];
        add->spawn();
        make_event( *sim, kill_delays[ i ], [ add ] {
          if ( !add->is_sleeping() )
            add->demise();
        } );
      }
    } );
  }

};

void burning_venom_t::reset()
{
  initial.sleeping       = true;
  caustic_surge_applied = false;
  pet_t::reset();
}

void burning_venom_t::demise()
{
  if ( !is_sleeping() && !caustic_surge_applied )
  {
    caustic_surge_applied = true;
    trigger_caustic_surge();
  }

  pet_t::demise();
}

player_t* create_vashnik( sim_t* sim )
{
  return new vashnik_t( sim );
}
}  // namespace bosses
