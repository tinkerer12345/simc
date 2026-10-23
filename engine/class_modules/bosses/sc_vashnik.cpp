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
// Deaths are scheduled from the observed kill order rather than modeled from add DPS.
constexpr double burning_venom_health = 1000000000;
constexpr int burning_venom_waves     = 4;
constexpr int burning_venoms_per_wave = 3;
constexpr int caustic_explosion_count = 28;

// Encounter values for one difficulty, selected with the boss option difficulty=<name>.
// Damage values are tooltip values. In the Mythic log every Vashnik spell landed at x0.952 (a damage-done
// debuff from the raid), so values only known from that log are the logged amount divided by 0.952.
struct vashnik_tuning_t
{
  timespan_t imbibe_first_cast;
  timespan_t imbibe_interval;
  // Vashnik stands in the fire and shadow fountains, so every Imbibe adds a Flame and a Shadow Infusion
  // stack (1.5 min, refreshed every Imbibe) and each Expulsion hits for this amount per stack.
  double expulsion_damage_per_stack;
  double toxic_vapor_damage_per_stack;
  int toxic_vapor_initial_stacks;  // Toxic Vapor is a room aura already active at the pull
  timespan_t burning_venom_spawn_delay;  // after each Imbibe completes
  timespan_t burning_venom_kill_delays[ burning_venom_waves ][ burning_venoms_per_wave ];
  double burning_presence_damage;
  double caustic_surge_damage;
  double caustic_surge_tick_damage;
  double caustic_explosion_damage;
  // Caustic Explosion is triggered by players, so its times are replayed from the log.
  timespan_t caustic_explosion_times[ caustic_explosion_count ];
};

constexpr vashnik_tuning_t mythic_tuning = {
  /* imbibe_first_cast */ 20_s,
  /* imbibe_interval */ 84_s,
  /* expulsion_damage_per_stack */ 108343,  // log 103,142.5 per stack
  /* toxic_vapor_damage_per_stack */ 10001,
  /* toxic_vapor_initial_stacks */ 1,
  /* burning_venom_spawn_delay */ 3_s,
  /* burning_venom_kill_delays */
  { { 7.683_s, 22.010_s, 37.181_s },
    { 11.669_s, 29.080_s, 39.275_s },
    { 13.422_s, 30.474_s, 46.334_s },
    { 24.551_s, 45.711_s, 61.269_s } },
  /* burning_presence_damage */ 33336,
  /* caustic_surge_damage */ 100009,
  /* caustic_surge_tick_damage */ 83341,
  /* caustic_explosion_damage */ 233354,  // log 222,153
  /* caustic_explosion_times */
  { 44.273_s,  47.767_s,  52.333_s,  53.568_s,  96.625_s,  98.595_s,  105.220_s, 108.555_s, 128.935_s, 130.393_s,
    136.497_s, 138.531_s, 180.169_s, 183.206_s, 187.453_s, 188.185_s, 211.996_s, 212.429_s, 219.866_s, 221.612_s,
    264.442_s, 269.396_s, 271.276_s, 278.486_s, 297.154_s, 300.585_s, 301.624_s, 310.855_s },
};

const vashnik_tuning_t& vashnik_tuning( util::string_view difficulty )
{
  if ( util::str_compare_ci( difficulty, "mythic" ) )
    return mythic_tuning;

  throw std::invalid_argument(
      fmt::format( "Vashnik difficulty '{}' is not supported; only 'mythic' is implemented.", difficulty ) );
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

// A hand-coded spell that hits every living player for a fixed amount.
struct raid_damage_t : public spell_t
{
  raid_damage_t( util::string_view name, player_t* p, school_e damage_school, double damage )
    : spell_t( name, p, spell_data_t::nil() )
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

  // The target list is cached, and enemy actions are never activate()d, so nothing refreshes it when a
  // player dies; dead players would keep taking damage. Rebuild it on every cast instead.
  void execute() override
  {
    target_cache.is_valid = false;
    spell_t::execute();
  }

  // SimC rolls between min and max (max defaults to 0), so both must be set.
  void set_damage( double damage )
  {
    base_dd_min = damage;
    base_dd_max = damage;
  }

  size_t available_targets( std::vector<player_t*>& target_list ) const override
  {
    return available_player_targets( this, target_list );
  }
};

struct burning_presence_t final : public raid_damage_t
{
  burning_presence_t( player_t* p, double damage ) : raid_damage_t( "burning_presence", p, SCHOOL_FIRE, damage )
  {
    background = false;  // pulses from the add's action list
  }

  void init() override
  {
    raid_damage_t::init();
    cooldown->duration = 3_s;
  }
};

struct toxic_vapor_t final : public raid_damage_t
{
  int& stacks;
  double damage_per_stack;

  toxic_vapor_t( player_t* p, util::string_view options_str, int& stacks, double damage )
    : raid_damage_t( "toxic_vapor", p, SCHOOL_NATURE, damage ), stacks( stacks ), damage_per_stack( damage )
  {
    background        = false;  // pulses from Vashnik's action list
    base_execute_time = 2_s;

    add_option( opt_float( "damage", damage_per_stack ) );
    add_option( opt_timespan( "attack_speed", base_execute_time.base ) );
    parse_options( options_str );
  }

  bool ready() override
  {
    return stacks > 0 && raid_damage_t::ready();
  }

  void execute() override
  {
    set_damage( damage_per_stack * stacks );
    raid_damage_t::execute();
  }
};

// Imbibe activates the nearest fountains. It deals no damage itself; the Expulsions do.
struct imbibe_t final : public spell_t
{
  int& toxic_vapor_stacks;
  int cast_count;
  double expulsion_damage_per_stack;
  raid_damage_t* conflagrating_expulsion;
  raid_damage_t* gloom_expulsion;
  std::function<void( int )> on_cast;  // called with the cast number when the cast completes

  imbibe_t( player_t* p, int& stacks, double expulsion_damage_per_stack )
    : spell_t( "imbibe", p, spell_data_t::nil() ),
      toxic_vapor_stacks( stacks ),
      cast_count( 0 ),
      expulsion_damage_per_stack( expulsion_damage_per_stack ),
      // Created with non-zero damage: SimC only applies target mitigation to actions that have direct
      // damage when they are initialised.
      conflagrating_expulsion(
          new raid_damage_t( "conflagrating_expulsion", p, SCHOOL_FIRE, expulsion_damage_per_stack ) ),
      // Wowhead only lists a summon for Gloom Expulsion; Shadow is assumed from the name and fountain.
      gloom_expulsion( new raid_damage_t( "gloom_expulsion", p, SCHOOL_SHADOW, expulsion_damage_per_stack ) )
  {
    base_execute_time = 4_s;
    may_crit          = false;
    may_miss          = false;
    background        = true;
  }

  void execute() override
  {
    spell_t::execute();

    ++cast_count;
    ++toxic_vapor_stacks;

    double damage = expulsion_damage_per_stack * cast_count;
    conflagrating_expulsion->set_damage( damage );
    conflagrating_expulsion->execute();
    gloom_expulsion->set_damage( damage );
    gloom_expulsion->execute();

    if ( on_cast )
      on_cast( cast_count );
  }

  void reset() override
  {
    spell_t::reset();
    cast_count = 0;
  }
};

struct burning_venom_t final : public pet_t
{
  const std::string& difficulty;  // the sim's encounter difficulty
  bool caustic_surge_applied;
  raid_damage_t* caustic_surge;
  raid_damage_t* caustic_surge_tick;

  burning_venom_t( sim_t* sim, player_t* owner, util::string_view name, const std::string& difficulty )
    : pet_t( sim, owner, name, PET_ENEMY ),
      difficulty( difficulty ),
      caustic_surge_applied( false ),
      caustic_surge( nullptr ),
      caustic_surge_tick( nullptr )
  {
    initial.sleeping = true;
    true_level       = sim->max_player_level + 3;
  }

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
    resources.base[ RESOURCE_HEALTH ]    = burning_venom_health;
    resources.initial[ RESOURCE_HEALTH ] = burning_venom_health;
    resources.max[ RESOURCE_HEALTH ]     = burning_venom_health;
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
      return new burning_presence_t( this, vashnik_tuning( difficulty ).burning_presence_damage );

    return pet_t::create_action( name, options_str );
  }

  void init_action_list() override
  {
    if ( action_list_str.empty() )
      action_list_str = "/burning_presence";

    pet_t::init_action_list();

    const auto& tuning = vashnik_tuning( difficulty );
    if ( !caustic_surge )
      caustic_surge = new raid_damage_t( "caustic_surge", this, SCHOOL_FIRE, tuning.caustic_surge_damage );
    if ( !caustic_surge_tick )
      caustic_surge_tick =
          new raid_damage_t( "caustic_surge_tick", this, SCHOOL_FIRE, tuning.caustic_surge_tick_damage );
  }

  void trigger_caustic_surge()
  {
    if ( !caustic_surge || !caustic_surge_tick )
      throw sc_runtime_error( fmt::format( "{}'s Caustic Surge actions were not initialized.", *this ) );

    caustic_surge->execute();
    for ( int tick = 1; tick <= 3; ++tick )
    {
      make_event( *sim, timespan_t::from_seconds( tick ), [ this ] { caustic_surge_tick->execute(); } );
    }
  }

  void reset() override
  {
    initial.sleeping      = true;
    caustic_surge_applied = false;
    pet_t::reset();
  }

  void demise() override
  {
    if ( !is_sleeping() && !caustic_surge_applied )
    {
      caustic_surge_applied = true;
      trigger_caustic_surge();
    }

    pet_t::demise();
  }
};

struct vashnik_t final : public enemy::enemy_t
{
  int toxic_vapor_stacks;
  imbibe_t* imbibe_action;
  raid_damage_t* caustic_explosion;
  std::vector<burning_venom_t*> burning_venoms;  // burning_venoms_per_wave adds per wave, in wave order

  explicit vashnik_t( sim_t* sim )
    : enemy::enemy_t( sim, "Vashnik" ), toxic_vapor_stacks( 0 ), imbibe_action( nullptr ), caustic_explosion( nullptr )
  {
    role = ROLE_NONE;
    // Stored on the sim so reports can show it; difficulty=<name> overrides this default.
    sim->encounter_difficulty_str = "mythic";

    for ( int i = 1; i <= burning_venom_waves * burning_venoms_per_wave; ++i )
      burning_venoms.push_back( new burning_venom_t( sim, this, fmt::format( "Burning_Venom_{}", i ),
                                                     sim->encounter_difficulty_str ) );
  }

  const vashnik_tuning_t& tuning() const
  {
    return vashnik_tuning( sim->encounter_difficulty_str );
  }

  void create_options() override
  {
    add_option( opt_string( "difficulty", sim->encounter_difficulty_str ) );
    enemy::enemy_t::create_options();
  }

  void init_base_stats() override
  {
    tuning();  // rejects an unsupported difficulty before anything else is set up
    enemy::enemy_t::init_base_stats();
  }

  void reset() override
  {
    enemy::enemy_t::reset();
    toxic_vapor_stacks = tuning().toxic_vapor_initial_stacks;
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
      return new toxic_vapor_t( this, options_str, toxic_vapor_stacks, tuning().toxic_vapor_damage_per_stack );

    return enemy::enemy_t::create_action( name, options_str );
  }

  void init_action_list() override
  {
    if ( action_list_str.empty() )
      action_list_str = "/toxic_vapor,attack_speed=2";

    enemy::enemy_t::init_action_list();

    if ( !imbibe_action )
    {
      imbibe_action          = new imbibe_t( this, toxic_vapor_stacks, tuning().expulsion_damage_per_stack );
      imbibe_action->on_cast = [ this ]( int cast_number ) { schedule_burning_venom_wave( cast_number - 1 ); };
    }
    if ( !caustic_explosion )
      caustic_explosion = new raid_damage_t( "caustic_explosion", this, SCHOOL_FIRE, tuning().caustic_explosion_damage );
  }

  void combat_begin() override
  {
    enemy::enemy_t::combat_begin();
    schedule_imbibe( tuning().imbibe_first_cast );

    for ( timespan_t time : tuning().caustic_explosion_times )
      make_event( *sim, time, [ this ] { caustic_explosion->execute(); } );
  }

  void schedule_imbibe( timespan_t delay )
  {
    make_event( *sim, delay, [ this ] {
      // Imbibe is a background action, so log its cast start explicitly for the report timeline.
      sequence_add( imbibe_action, imbibe_action->target );
      imbibe_action->schedule_execute();
      schedule_imbibe( tuning().imbibe_interval );
    } );
  }

  // Each Imbibe spawns a wave of Burning Venoms. Later waves have no log data and are not modelled.
  void schedule_burning_venom_wave( int wave )
  {
    if ( wave >= burning_venom_waves )
      return;

    make_event( *sim, tuning().burning_venom_spawn_delay, [ this, wave ] {
      // These staggered delays reproduce the deaths in the supplied log.
      const auto& kill_delays = tuning().burning_venom_kill_delays[ wave ];

      for ( int i = 0; i < burning_venoms_per_wave; ++i )
      {
        burning_venom_t* add = burning_venoms[ wave * burning_venoms_per_wave + i ];
        add->spawn();
        make_event( *sim, kill_delays[ i ], [ add ] {
          if ( !add->is_sleeping() )
            add->demise();
        } );
      }
    } );
  }
};
}  // namespace

player_t* create_vashnik( sim_t* sim )
{
  return new vashnik_t( sim );
}
}  // namespace bosses
