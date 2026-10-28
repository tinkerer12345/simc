// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "class_modules/bosses/sc_vashnik.hpp"
#include "class_modules/bosses/sc_encounter_damage.hpp"
#include "class_modules/sc_enemy.hpp"

namespace bosses
{
namespace
{
// Deaths are scheduled from the observed kill order rather than modeled from add DPS.
constexpr double burning_venom_health = 1000000000;
constexpr int burning_venom_waves     = 4;
constexpr int burning_venoms_per_wave = 3;
constexpr int exploding_infection_waves   = 7;
constexpr int exploding_infection_players = 4;  // per wave
constexpr int exploding_infection_others  = 3;  // of those, dispelled by the other healers
constexpr int shrouded_venom_waves     = 4;
constexpr int shrouded_venoms_per_wave = 8;

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
  // Exploding Infection (spot): every wave infects exploding_infection_players players at once with a Fire DoT that
  // gains a stack every tick; removing it (dispel, expiry or death) makes them erupt in a Caustic Explosion (burst).
  // The log's 28 dispels are exactly its 28 Caustic Explosions. Application = 1 sec before the wave's first dispel
  // (that fits the log's 102 ticks). The other healers dispel exploding_infection_others of them at the log's
  // offsets (the wave's dispels without the druid's first); the last one is the healer under test's (a quarter of
  // the dispels, like the healing share) and expires after exploding_infection_duration (an assumption) if not
  // dispelled.
  timespan_t exploding_infection_times[ exploding_infection_waves ];
  timespan_t exploding_infection_other_dispels[ exploding_infection_waves ][ exploding_infection_others ];
  double exploding_infection_tick_damage;  // per stack
  timespan_t exploding_infection_tick_interval;
  timespan_t exploding_infection_duration;
  // Times "after Imbibe" count from the start of the Imbibe cast. Counts and rates are the Mythic log's.
  // Plague Froth (spot): a 6 sec debuff on a few players whose ticks hit everyone near them; the log averages 7 players
  // hit per tick. When it ends, Plague Waves erupt (avoidable; the log has 3 hits per Froth).
  timespan_t plague_froth_first;
  timespan_t plague_froth_after_imbibe[ 2 ];
  double plague_froth_damage;
  int plague_froth_ticks;  // every 1 sec
  int plague_froth_players;
  double plague_wave_damage;
  int plague_wave_hits;
  // Malignant Catalyst (burst) hits everyone; its Catalytic Bile (spot) lands on the 5 soakers 7 sec later.
  timespan_t malignant_catalyst_after_imbibe[ 2 ];
  double malignant_catalyst_damage;
  timespan_t catalytic_bile_delay;
  double catalytic_bile_damage;
  int catalytic_bile_hits;
  // Dripping Fangs (tank): a bite on the active tank plus a Nature DoT every 2 sec for 32 sec; the tanks swap after
  // each bite (the +200% physical damage taken debuff is gone before the next bite on the same tank, so it is not
  // modelled). Parried bites still apply the DoT.
  timespan_t dripping_fangs_first;
  timespan_t dripping_fangs_interval;
  double dripping_fangs_damage;
  double dripping_fangs_tick_damage;
  int dripping_fangs_ticks;
  double dripping_fangs_parry_chance;
  // Melee (tank) on the active tank: the log's swings average one every 3.75 sec (gaps during casts included).
  timespan_t melee_first;
  timespan_t melee_interval;
  double melee_damage;  // log unmitigated average 1,584,280
  double melee_parry_chance;
  // Shrouded Venoms (spot): 8 adds per Imbibe (with the Burning Venoms), each casting Congealing Bolt at a random
  // player until it dies; the lifetimes are each add's death in the log.
  timespan_t shrouded_venom_after_imbibe;
  timespan_t shrouded_venom_lifetimes[ shrouded_venom_waves ][ shrouded_venoms_per_wave ];
  timespan_t congealing_bolt_first;
  timespan_t congealing_bolt_interval;
  double congealing_bolt_damage;
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
  /* exploding_infection_times */
  { 43.245_s, 95.602_s, 127.896_s, 179.165_s, 210.985_s, 263.435_s, 296.152_s },
  /* exploding_infection_other_dispels */
  { { 1.000_s, 4.523_s, 10.314_s },
    { 1.000_s, 2.960_s, 12.913_s },
    { 1.000_s, 8.582_s, 10.613_s },
    { 1.000_s, 8.287_s, 9.003_s },
    { 1.446_s, 8.845_s, 10.614_s },
    { 1.000_s, 7.829_s, 15.046_s },
    { 1.000_s, 4.425_s, 14.711_s } },
  /* exploding_infection_tick_damage */ 25002,
  /* exploding_infection_tick_interval */ 1.5_s,
  /* exploding_infection_duration */ 30_s,
  /* plague_froth_first */ 14_s,
  /* plague_froth_after_imbibe */ { 35_s, 68_s },
  /* plague_froth_damage */ 166681,
  /* plague_froth_ticks */ 6,
  /* plague_froth_players */ 7,
  /* plague_wave_damage */ 666725,
  /* plague_wave_hits */ 3,
  /* malignant_catalyst_after_imbibe */ { 15_s, 54_s },
  /* malignant_catalyst_damage */ 320862,
  /* catalytic_bile_delay */ 7_s,
  /* catalytic_bile_damage */ 283224,
  /* catalytic_bile_hits */ 5,
  /* dripping_fangs_first */ 10_s,
  /* dripping_fangs_interval */ 28_s,
  /* dripping_fangs_damage */ 2083517,
  /* dripping_fangs_tick_damage */ 250022,
  /* dripping_fangs_ticks */ 16,
  /* dripping_fangs_parry_chance */ 0.23,  // 3 of 13 bites
  /* melee_first */ 3.6_s,
  /* melee_interval */ 3.75_s,
  /* melee_damage */ 1664160,
  /* melee_parry_chance */ 0.32,
  /* shrouded_venom_after_imbibe */ 6.7_s,
  /* shrouded_venom_lifetimes */
  { { 2.2_s, 2.7_s, 2.5_s, 0.3_s, 2.3_s, 2.3_s, 2.2_s, 0.9_s },
    { 8.3_s, 4.6_s, 6.5_s, 8.8_s, 7.2_s, 8.5_s, 7.4_s, 8.7_s },
    { 8.1_s, 8.7_s, 8.1_s, 12.8_s, 13.2_s, 11.1_s, 11.7_s, 12.2_s },
    { 15.6_s, 15.0_s, 10.0_s, 26.7_s, 14.2_s, 13.6_s, 10.9_s, 11.4_s } },
  /* congealing_bolt_first */ 0.3_s,
  /* congealing_bolt_interval */ 1.92_s,
  /* congealing_bolt_damage */ 175015,
};

const vashnik_tuning_t& vashnik_tuning( util::string_view difficulty )
{
  if ( util::str_compare_ci( difficulty, "mythic" ) )
    return mythic_tuning;

  throw std::invalid_argument(
      fmt::format( "Vashnik difficulty '{}' is not supported; only 'mythic' is implemented.", difficulty ) );
}

struct burning_presence_t final : public raid_damage_t
{
  burning_presence_t( player_t* p, double damage ) : raid_damage_t( "burning_presence", p, SCHOOL_FIRE, damage, damage_type_e::STEADY )
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
    : raid_damage_t( "toxic_vapor", p, SCHOOL_NATURE, damage, damage_type_e::STEADY ), stacks( stacks ), damage_per_stack( damage )
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
          new raid_damage_t( "conflagrating_expulsion", p, SCHOOL_FIRE, expulsion_damage_per_stack, damage_type_e::BURST ) ),
      // Wowhead only lists a summon for Gloom Expulsion; Shadow is assumed from the name and fountain.
      gloom_expulsion( new raid_damage_t( "gloom_expulsion", p, SCHOOL_SHADOW, expulsion_damage_per_stack, damage_type_e::BURST ) )
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
      caustic_surge = new raid_damage_t( "caustic_surge", this, SCHOOL_FIRE, tuning.caustic_surge_damage, damage_type_e::BURST );
    if ( !caustic_surge_tick )
      caustic_surge_tick =
          new raid_damage_t( "caustic_surge_tick", this, SCHOOL_FIRE, tuning.caustic_surge_tick_damage, damage_type_e::BURST );
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

// Shrouded Venom: an add that only casts Congealing Bolt at random players; Vashnik schedules its bolts and its death
// from the log (its Umbral Ejection on death is avoidable and too small in the log to model).
struct shrouded_venom_t final : public pet_t
{
  raid_damage_t* congealing_bolt = nullptr;
  double bolt_damage;

  shrouded_venom_t( sim_t* sim, player_t* owner, util::string_view name, double bolt_damage )
    : pet_t( sim, owner, name, PET_ENEMY ), bolt_damage( bolt_damage )
  {
    initial.sleeping = true;
    true_level       = sim->max_player_level + 3;
  }

  void init_action_list() override
  {
    pet_t::init_action_list();

    if ( !congealing_bolt )
      congealing_bolt = ( new raid_damage_t( "congealing_bolt", this, SCHOOL_SHADOW, bolt_damage, damage_type_e::SPOT ) )
                           ->random_targets( 1 );
  }

  void spawn()
  {
    initial.sleeping = false;
    arise();
  }

  void reset() override
  {
    initial.sleeping = true;
    pet_t::reset();
  }

  bool receives_raid_debuffs() const override { return false; }
  bool receives_buffs() const override { return false; }
};

struct vashnik_t final : public enemy::enemy_t
{
  int toxic_vapor_stacks;
  imbibe_t* imbibe_action;
  raid_damage_t* caustic_explosion;
  std::vector<burning_venom_t*> burning_venoms;  // burning_venoms_per_wave adds per wave, in wave order
  std::vector<shrouded_venom_t*> shrouded_venoms;  // shrouded_venoms_per_wave adds per wave, in wave order
  raid_damage_t* plague_froth = nullptr;
  raid_damage_t* plague_wave = nullptr;
  raid_damage_t* malignant_catalyst = nullptr;
  raid_damage_t* catalytic_bile = nullptr;
  raid_damage_t* dripping_fangs = nullptr;
  raid_damage_t* dripping_fangs_tick = nullptr;
  raid_damage_t* melee = nullptr;
  std::vector<player_t*> tanks;  // the raid's tanks; the one at active_tank takes melee and the next bite
  size_t active_tank = 0;
  raid_damage_t* exploding_infection = nullptr;
  struct infection_t
  {
    player_t* target;
    int stacks;
    bool healer_dispels;  // the healer under test's to dispel, rather than the other healers'
    bool active;
  };
  std::vector<infection_t> infections;  // this fight's, in application order (indices stay valid)

  explicit vashnik_t( sim_t* sim )
    : enemy::enemy_t( sim, "Vashnik" ), toxic_vapor_stacks( 0 ), imbibe_action( nullptr ), caustic_explosion( nullptr )
  {
    role = ROLE_NONE;
    // Stored on the sim so reports can show it; difficulty=<name> overrides this default.
    sim->encounter_difficulty_str = "mythic";

    for ( int i = 1; i <= burning_venom_waves * burning_venoms_per_wave; ++i )
      burning_venoms.push_back( new burning_venom_t( sim, this, fmt::format( "Burning_Venom_{}", i ),
                                                     sim->encounter_difficulty_str ) );

    // the bolt damage is the same on every difficulty implemented so far (mythic only)
    for ( int i = 1; i <= shrouded_venom_waves * shrouded_venoms_per_wave; ++i )
      shrouded_venoms.push_back( new shrouded_venom_t( sim, this, fmt::format( "Shrouded_Venom_{}", i ),
                                                       mythic_tuning.congealing_bolt_damage ) );
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
    infections.clear();
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
      caustic_explosion = new raid_damage_t( "caustic_explosion", this, SCHOOL_FIRE, tuning().caustic_explosion_damage, damage_type_e::BURST );

    const auto& t = tuning();
    if ( !plague_froth )
    {
      // Plague is Nature + Shadow, which SimC calls shadowstorm
      plague_froth = ( new raid_damage_t( "plague_froth", this, SCHOOL_SHADOWSTORM, t.plague_froth_damage,
                                          damage_type_e::SPOT ) )->single_target();
      plague_wave = ( new raid_damage_t( "plague_wave", this, SCHOOL_SHADOWSTORM, t.plague_wave_damage,
                                         damage_type_e::AVOIDABLE ) )->random_targets( t.plague_wave_hits );
      malignant_catalyst = new raid_damage_t( "malignant_catalyst", this, SCHOOL_NATURE, t.malignant_catalyst_damage,
                                              damage_type_e::BURST );
      catalytic_bile = ( new raid_damage_t( "catalytic_bile", this, SCHOOL_NATURE, t.catalytic_bile_damage,
                                            damage_type_e::SPOT ) )->random_targets( t.catalytic_bile_hits );
      dripping_fangs = ( new raid_damage_t( "dripping_fangs", this, SCHOOL_PHYSICAL, t.dripping_fangs_damage,
                                            damage_type_e::TANK ) )->single_target();
      dripping_fangs_tick = ( new raid_damage_t( "dripping_fangs_tick", this, SCHOOL_NATURE,
                                                 t.dripping_fangs_tick_damage, damage_type_e::TANK ) )->single_target();
      melee = ( new raid_damage_t( "melee", this, SCHOOL_PHYSICAL, t.melee_damage, damage_type_e::TANK ) )
                  ->single_target();
      exploding_infection = ( new raid_damage_t( "exploding_infection", this, SCHOOL_FIRE,
                                                 t.exploding_infection_tick_damage, damage_type_e::SPOT ) )
                                ->single_target();
    }

    sim->encounter_dispellable = [ this ]( const player_t* p ) {
      for ( const auto& inf : infections )
        if ( inf.active && inf.healer_dispels && inf.target == p )
          return as<double>( inf.stacks );
      return 0.0;
    };
    // Scripted damage the healer can plan for: "imbibe" = the Expulsions landing (end of the Imbibe cast), "catalyst"
    // = Malignant Catalyst, "burst" = whichever is next.
    sim->encounter_timer = [ this ]( util::string_view name, bool ago ) {
      bool imbibe = name == "imbibe" || name == "burst";
      bool catalyst = name == "catalyst" || name == "burst";
      if ( !imbibe && !catalyst )
        return -1.0;

      const auto& t   = tuning();
      timespan_t now  = sim->current_time();
      timespan_t cast = imbibe_action->base_execute_time;
      timespan_t next = sim->expected_iteration_time;
      timespan_t last = timespan_t::min();
      auto consider = [ & ]( timespan_t at ) {
        if ( at > now )
          next = std::min( next, at );
        else
          last = std::max( last, at );
      };
      for ( timespan_t start = t.imbibe_first_cast; start < sim->expected_iteration_time; start += t.imbibe_interval )
      {
        if ( imbibe )
          consider( start + cast );
        if ( catalyst )
          for ( timespan_t delay : t.malignant_catalyst_after_imbibe )
            consider( start + delay );
      }
      if ( ago )
        return last == timespan_t::min() ? 9999.0 : ( now - last ).total_seconds();
      return std::max( 0.0, ( next - now ).total_seconds() );
    };
    sim->encounter_dispel = [ this ]( player_t* p ) {
      for ( size_t i = 0; i < infections.size(); ++i )
      {
        if ( infections[ i ].active && infections[ i ].healer_dispels && infections[ i ].target == p )
        {
          remove_infection( i );
          return true;
        }
      }
      return false;
    };
  }

  void combat_begin() override
  {
    enemy::enemy_t::combat_begin();
    const auto& t = tuning();
    schedule_imbibe( t.imbibe_first_cast );

    for ( int wave = 0; wave < exploding_infection_waves; ++wave )
      make_event( *sim, t.exploding_infection_times[ wave ], [ this, wave ] { apply_exploding_infections( wave ); } );

    tanks.clear();
    for ( auto p : sim->player_no_pet_list )
      if ( p->role == ROLE_TANK )
        tanks.push_back( p );
    active_tank = 0;

    make_event( *sim, t.plague_froth_first, [ this ] { cast_plague_froth(); } );
    schedule_dripping_fangs( t.dripping_fangs_first );
    schedule_melee( t.melee_first );
  }

  void schedule_imbibe( timespan_t delay )
  {
    make_event( *sim, delay, [ this ] {
      // Imbibe is a background action, so log its cast start explicitly for the report timeline.
      sequence_add( imbibe_action, imbibe_action->target );
      imbibe_action->schedule_execute();
      schedule_imbibe_cycle();
      schedule_imbibe( tuning().imbibe_interval );
    } );
  }

  // What follows each Imbibe cast start: Shrouded Venoms, two Malignant Catalysts and two Plague Froths.
  void schedule_imbibe_cycle()
  {
    const auto& t = tuning();
    int wave = imbibe_action->cast_count;  // casts completed so far = this cycle's index

    make_event( *sim, t.shrouded_venom_after_imbibe, [ this, wave ] { spawn_shrouded_venoms( wave ); } );

    for ( timespan_t delay : t.malignant_catalyst_after_imbibe )
      make_event( *sim, delay, [ this ] { cast_malignant_catalyst(); } );

    for ( timespan_t delay : t.plague_froth_after_imbibe )
      make_event( *sim, delay, [ this ] { cast_plague_froth(); } );
  }

  std::vector<player_t*> living_players() const
  {
    std::vector<player_t*> l;
    for ( auto p : sim->player_no_pet_list )
      if ( !p->is_sleeping() )
        l.push_back( p );
    return l;
  }

  // Plague Froth: the same few players are hit every tick, then Plague Waves erupt.
  void cast_plague_froth()
  {
    const auto& t = tuning();
    auto victims = living_players();
    rng().shuffle( victims.begin(), victims.end() );
    if ( victims.size() > as<size_t>( t.plague_froth_players ) )
      victims.resize( t.plague_froth_players );
    if ( victims.empty() )
      return;

    sequence_add( plague_froth, victims.front() );
    for ( int tick = 0; tick < t.plague_froth_ticks; ++tick )
    {
      make_event( *sim, timespan_t::from_seconds( tick ), [ this, victims ] {
        for ( auto v : victims )
          if ( !v->is_sleeping() )
            plague_froth->execute_on_target( v );
      } );
    }
    make_event( *sim, timespan_t::from_seconds( t.plague_froth_ticks ), [ this ] { plague_wave->execute(); } );
  }

  void cast_malignant_catalyst()
  {
    sequence_add( malignant_catalyst, malignant_catalyst->target );
    malignant_catalyst->execute();
    make_event( *sim, tuning().catalytic_bile_delay, [ this ] { catalytic_bile->execute(); } );
  }

  // The tank at active_tank if alive, else any living tank, else nullptr (an encounter without tanks has no tank
  // damage).
  player_t* current_tank()
  {
    if ( tanks.empty() )
      return nullptr;
    if ( !tanks[ active_tank ]->is_sleeping() )
      return tanks[ active_tank ];
    for ( size_t i = 0; i < tanks.size(); ++i )
    {
      if ( !tanks[ i ]->is_sleeping() )
      {
        active_tank = i;
        return tanks[ i ];
      }
    }
    return nullptr;
  }

  void schedule_dripping_fangs( timespan_t delay )
  {
    make_event( *sim, delay, [ this ] {
      const auto& t = tuning();
      if ( auto tank = current_tank() )
      {
        sequence_add( dripping_fangs, tank );
        if ( !rng().roll( t.dripping_fangs_parry_chance ) )
          dripping_fangs->execute_on_target( tank );
        for ( int tick = 1; tick <= t.dripping_fangs_ticks; ++tick )
        {
          make_event( *sim, timespan_t::from_seconds( 2.0 * tick ), [ this, tank ] {
            if ( !tank->is_sleeping() )
              dripping_fangs_tick->execute_on_target( tank );
          } );
        }
        // the other tank taunts
        active_tank = ( active_tank + 1 ) % tanks.size();
      }
      schedule_dripping_fangs( t.dripping_fangs_interval );
    } );
  }

  void schedule_melee( timespan_t delay )
  {
    make_event( *sim, delay, [ this ] {
      const auto& t = tuning();
      if ( auto tank = current_tank(); tank && !rng().roll( t.melee_parry_chance ) )
        melee->execute_on_target( tank );
      schedule_melee( t.melee_interval );
    } );
  }

  void spawn_shrouded_venoms( int wave )
  {
    if ( wave >= shrouded_venom_waves )
      return;

    const auto& t = tuning();
    for ( int i = 0; i < shrouded_venoms_per_wave; ++i )
    {
      auto add = shrouded_venoms[ wave * shrouded_venoms_per_wave + i ];
      timespan_t life = t.shrouded_venom_lifetimes[ wave ][ i ];
      add->spawn();
      for ( timespan_t at = t.congealing_bolt_first; at < life; at += t.congealing_bolt_interval )
      {
        make_event( *sim, at, [ add ] {
          if ( !add->is_sleeping() )
            add->congealing_bolt->execute();
        } );
      }
      make_event( *sim, life, [ add ] {
        if ( !add->is_sleeping() )
          add->demise();
      } );
    }
  }

  // Infects up to exploding_infection_players living players; the first exploding_infection_others are the other
  // healers' to dispel.
  void apply_exploding_infections( int wave )
  {
    const auto& t = tuning();
    auto victims  = living_players();
    rng().shuffle( victims.begin(), victims.end() );
    if ( victims.size() > as<size_t>( exploding_infection_players ) )
      victims.resize( exploding_infection_players );
    if ( victims.empty() )
      return;

    sequence_add( exploding_infection, victims.front() );
    for ( size_t v = 0; v < victims.size(); ++v )
    {
      bool healer = v >= as<size_t>( exploding_infection_others );
      size_t i    = infections.size();
      infections.push_back( { victims[ v ], 1, healer, true } );
      schedule_infection_tick( i );

      timespan_t removal = healer ? t.exploding_infection_duration : t.exploding_infection_other_dispels[ wave ][ v ];
      make_event( *sim, removal, [ this, i ] { remove_infection( i ); } );
    }
  }

  // Each tick hits for the current stacks, then adds one. A dead target loses the debuff, which also explodes.
  void schedule_infection_tick( size_t i )
  {
    make_event( *sim, tuning().exploding_infection_tick_interval, [ this, i ] {
      auto& inf = infections[ i ];
      if ( !inf.active )
        return;
      if ( inf.target->is_sleeping() )
      {
        remove_infection( i );
        return;
      }
      exploding_infection->set_damage( tuning().exploding_infection_tick_damage * inf.stacks );
      exploding_infection->execute_on_target( inf.target );
      ++inf.stacks;
      schedule_infection_tick( i );
    } );
  }

  void remove_infection( size_t i )
  {
    auto& inf = infections[ i ];
    if ( !inf.active )
      return;
    inf.active = false;
    caustic_explosion->execute();
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
