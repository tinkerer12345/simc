// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "class_modules/bosses/sc_patchwerk.hpp"
#include "class_modules/bosses/sc_encounter_damage.hpp"
#include "class_modules/raid/sc_raid_member.hpp"
#include "class_modules/sc_enemy.hpp"

namespace bosses
{
namespace
{
/* Patchwerk for healers: a generic, season-proof test fight with no boss script to keep up to date.
 *
 * Every hit is a share of a stand-in's max health (H, tanks T; see raid::member_health), so the fight grows with the
 * raid's gear: with raid_health_from_healer=1 everyone has the healer's own health and a new season needs no new
 * numbers. Like a boss module, the values are the whole raid's damage; the damage type multipliers (the tolerance
 * search: one healer's share x the level) scale it.
 *
 * Calibrated once to Mythic Vashnik (20 players, before mitigation, H = 900k, T = 1.3M): each player took 10.7% of H
 * per second (burst 4.98%, steady 3.21%, spot 2.28%, avoidable 0.26%), each of the 2 tanks 18.8% of T per second on
 * top (melee 61%, the tank buster 11%, its DoT 28%). Unlike Vashnik nothing escalates: the same load all fight.
 *
 * Damage per kind:
 * - steady: raid_pulse hits everyone every 2 sec.
 * - burst: raid_burst every burst_interval (30 sec) from 0:20, every big_burst_every-th one (4th) a big_raid_burst
 *   twice as large: 60% on impact, then 4 ticks of 10% one second apart. By default the bursts keep burst damage per
 *   second the same whatever the interval (fewer, bigger bursts); burst_scales_with_interval=0 keeps each burst the
 *   size it has at the defaults (every 30 sec, every 4th big), so a longer interval means less burst damage in total.
 * - spot: spot_dot on 3 random players every 8 sec (6 ticks, 1 sec), and every 30 sec from 0:35 a dispellable_dot on
 *   one random player, the healer's to dispel: it gains a stack every 1.5 sec tick and lasts 30 sec (lethal if
 *   ignored, like Vashnik's Exploding Infection).
 * - avoidable: avoidable_hit on 1 random player every 20 sec.
 * - tank: melee on the active tank every 2 sec, a tank_buster (physical hit + 16 tank_buster_dot ticks, 2 sec) every
 *   30 sec from 0:10, then the other tank taunts.
 * Spot and avoidable hits are sized for the raid's size: the same damage per player whatever the raid.
 *
 * APL timers: encounter.burst.remains / .ago (any burst), encounter.big_burst.remains / .ago (the big ones).
 *
 * Phases (opt-in, phases=...): a fight shaped like a real boss, one phase after another, each with its own damage mix
 * and burst rhythm. "<seconds>:key=value,key=value/<seconds>:..." with keys
 *   steady, burst, spot, avoidable, tank   multipliers on that kind's damage per second (default 1)
 *   interval, first                        seconds between bursts, and from the phase's start to its first burst
 *   waves                                  > 0: each burst lands as a wave on the whole raid, one tick a second for
 *                                          this many seconds, instead of 60% impact + 4 ticks
 *   growth                                 each burst in the phase this much bigger than the one before (0.12 = +12%),
 *                                          the phase's burst damage per second unchanged
 *   big_every                              every n-th burst of the phase is "big" for encounter.big_burst (0: none)
 * The last phase lasts until the fight ends. Without phases, nothing above changes.
 */
constexpr double steady_rate             = 0.0321;  // H per second, per player
constexpr timespan_t steady_interval     = 2_s;
constexpr double burst_rate              = 0.0498;
constexpr timespan_t burst_first         = 20_s;
constexpr timespan_t default_burst_interval = 30_s;
constexpr int default_big_burst_every    = 4;
constexpr double big_burst_size          = 2.0;  // x a normal burst
constexpr double burst_impact_share      = 0.6;
constexpr int burst_ticks                = 4;  // the rest, one second apart
constexpr double spot_rate               = 0.0228;
constexpr timespan_t spot_first          = 8_s;
constexpr timespan_t spot_interval       = 8_s;
constexpr int spot_targets               = 3;
constexpr int spot_ticks                 = 6;  // one per second
constexpr timespan_t dispellable_first    = 35_s;
constexpr timespan_t dispellable_interval = 30_s;
constexpr timespan_t dispellable_tick     = 1.5_s;
constexpr timespan_t dispellable_duration = 30_s;
constexpr double dispellable_stack_damage = 0.0278;  // H per stack per tick (Exploding Infection: 25k of 900k)
constexpr double avoidable_rate           = 0.0026;
constexpr timespan_t avoidable_first      = 15_s;
constexpr timespan_t avoidable_interval   = 20_s;
constexpr double melee_rate               = 0.229;  // T per second, all tanks together
constexpr timespan_t melee_first          = 2_s;
constexpr timespan_t melee_interval       = 2_s;
constexpr double buster_rate              = 0.0422;
constexpr double buster_dot_rate          = 0.1054;
constexpr timespan_t buster_first         = 10_s;
constexpr timespan_t buster_interval      = 30_s;
constexpr int buster_dot_ticks            = 16;  // every 2 sec

// One phase of a phased fight (phases=...).
struct phase_t
{
  double start = 0, duration = 0;  // seconds
  double steady = 1, burst = 1, spot = 1, avoidable = 1, tank = 1;
  double interval = 30, first = 20, waves = 0, growth = 0;
  int big_every = 0;
};

// A scheduled burst of a phased fight.
struct scheduled_burst_t
{
  double at;      // seconds into the fight
  double damage;  // per player, in H
  double waves;   // > 0: a wave over this many seconds
  bool big;
};

std::vector<phase_t> parse_phases( util::string_view str )
{
  std::vector<phase_t> out;
  double start = 0;
  for ( auto&& part : util::string_split<util::string_view>( str, "/" ) )
  {
    phase_t ph;
    auto colon = part.find( ':' );
    ph.duration = util::to_double( part.substr( 0, colon ) );
    if ( ph.duration <= 0 )
      throw std::invalid_argument( fmt::format( "Patchwerk phases: invalid phase '{}'.", part ) );
    ph.start = start;
    start += ph.duration;
    if ( colon != util::string_view::npos )
    {
      for ( auto&& kv : util::string_split<util::string_view>( part.substr( colon + 1 ), "," ) )
      {
        auto eq = kv.find( '=' );
        if ( eq == util::string_view::npos )
          throw std::invalid_argument( fmt::format( "Patchwerk phases: '{}' needs key=value.", kv ) );
        auto key   = kv.substr( 0, eq );
        double val = util::to_double( kv.substr( eq + 1 ) );
        if ( key == "steady" ) ph.steady = val;
        else if ( key == "burst" ) ph.burst = val;
        else if ( key == "spot" ) ph.spot = val;
        else if ( key == "avoidable" ) ph.avoidable = val;
        else if ( key == "tank" ) ph.tank = val;
        else if ( key == "interval" ) ph.interval = std::max( 1.0, val );
        else if ( key == "first" ) ph.first = val;
        else if ( key == "waves" ) ph.waves = val;
        else if ( key == "growth" ) ph.growth = val;
        else if ( key == "big_every" ) ph.big_every = as<int>( val );
        else
          throw std::invalid_argument( fmt::format( "Patchwerk phases: unknown key '{}'.", key ) );
      }
    }
    out.push_back( ph );
  }
  return out;
}

struct patchwerk_t final : public enemy::enemy_t
{
  timespan_t burst_interval = default_burst_interval;
  int big_burst_every       = default_big_burst_every;
  bool burst_scales_with_interval = true;
  std::string phases_str;
  std::vector<phase_t> phases;
  std::vector<scheduled_burst_t> burst_schedule;  // phased fights: every burst of the fight, in time order

  raid_damage_t* raid_pulse      = nullptr;
  raid_damage_t* raid_burst      = nullptr;
  raid_damage_t* big_raid_burst  = nullptr;
  raid_damage_t* spot_dot        = nullptr;
  raid_damage_t* dispellable_dot = nullptr;
  raid_damage_t* avoidable_hit   = nullptr;
  raid_damage_t* melee           = nullptr;
  raid_damage_t* tank_buster     = nullptr;
  raid_damage_t* tank_buster_dot = nullptr;

  size_t raid_size = 0;  // players at the pull
  std::vector<player_t*> tanks;
  size_t active_tank = 0;
  int bursts         = 0;  // bursts so far this fight
  struct affliction_t
  {
    player_t* target;
    int stacks;
    bool active;
  };
  std::vector<affliction_t> afflictions;  // this fight's, in application order (indices stay valid)

  explicit patchwerk_t( sim_t* sim, util::string_view name = "Patchwerk", util::string_view default_phases = "" )
    : enemy::enemy_t( sim, name ), phases_str( default_phases )
  {
    role = ROLE_NONE;
  }

  void create_options() override
  {
    add_option( opt_timespan( "burst_interval", burst_interval, 5_s, 600_s ) );
    add_option( opt_int( "big_burst_every", big_burst_every, 1, 100 ) );
    add_option( opt_bool( "burst_scales_with_interval", burst_scales_with_interval ) );
    add_option( opt_string( "phases", phases_str ) );
    enemy::enemy_t::create_options();
  }

  // The phase at a time (phased fights; the last one lasts until the end).
  const phase_t& phase_at( timespan_t t ) const
  {
    double s = t.total_seconds();
    for ( size_t i = 0; i + 1 < phases.size(); ++i )
      if ( s < phases[ i ].start + phases[ i ].duration )
        return phases[ i ];
    return phases.back();
  }
  // A kind's multiplier now: 1 without phases.
  double mult( double phase_t::*kind ) const
  {
    return phases.empty() ? 1.0 : phase_at( sim->current_time() ).*kind;
  }

  // Every burst of a phased fight: each phase's bursts at first + k x interval while the phase lasts, sized so that
  // the phase's burst damage per second is burst_rate x its burst multiplier, growing by its growth.
  void build_burst_schedule()
  {
    burst_schedule.clear();
    double end = sim->expected_iteration_time.total_seconds();
    for ( size_t p = 0; p < phases.size(); ++p )
    {
      const auto& ph = phases[ p ];
      double until   = p + 1 < phases.size() ? ph.start + ph.duration : end;
      std::vector<double> times;
      for ( double at = ph.start + ph.first; at < until && ph.burst > 0; at += ph.interval )
        times.push_back( at );
      if ( times.empty() )
        continue;
      double weights = 0;
      for ( size_t k = 0; k < times.size(); ++k )
        weights += std::pow( 1.0 + ph.growth, as<double>( k ) );
      double mean = weights / as<double>( times.size() );
      for ( size_t k = 0; k < times.size(); ++k )
      {
        double damage = burst_rate * ph.burst * ph.interval * std::pow( 1.0 + ph.growth, as<double>( k ) ) / mean;
        bool big      = ph.big_every > 0 && ( k + 1 ) % ph.big_every == 0;
        burst_schedule.push_back( { times[ k ], damage, ph.waves, big } );
      }
    }
  }

  role_e primary_role() const override { return ROLE_NONE; }
  bool receives_raid_debuffs() const override { return false; }
  bool receives_buffs() const override { return false; }

  double health() const { return raid::member_health( sim, false ); }
  double tank_health() const { return raid::member_health( sim, true ); }

  // A normal burst's total damage per player; big ones are big_burst_size times as large, and the average over a cycle
  // of big_burst_every bursts is burst_rate (at the default interval and big_burst_every, unless
  // burst_scales_with_interval).
  double burst_damage() const
  {
    timespan_t interval = burst_scales_with_interval ? burst_interval : default_burst_interval;
    int every           = burst_scales_with_interval ? big_burst_every : default_big_burst_every;
    double cycle        = interval.total_seconds() * every;
    return burst_rate * health() * cycle / ( every - 1 + big_burst_size );
  }

  bool is_big( int burst_index ) const
  {
    return ( burst_index + 1 ) % big_burst_every == 0;
  }

  void reset() override
  {
    enemy::enemy_t::reset();
    afflictions.clear();
    bursts = 0;
  }

  void init_action_list() override
  {
    enemy::enemy_t::init_action_list();

    if ( !raid_pulse )
    {
      // Created with non-zero damage (SimC only applies mitigation to actions with direct damage at init); the
      // amounts are set from the raid's health on every hit.
      raid_pulse     = new raid_damage_t( "raid_pulse", this, SCHOOL_NATURE, 1, damage_type_e::STEADY );
      raid_burst     = new raid_damage_t( "raid_burst", this, SCHOOL_FIRE, 1, damage_type_e::BURST );
      big_raid_burst = new raid_damage_t( "big_raid_burst", this, SCHOOL_FIRE, 1, damage_type_e::BURST );
      spot_dot = ( new raid_damage_t( "spot_dot", this, SCHOOL_SHADOW, 1, damage_type_e::SPOT ) )->single_target();
      dispellable_dot =
          ( new raid_damage_t( "dispellable_dot", this, SCHOOL_SHADOW, 1, damage_type_e::SPOT ) )->single_target();
      avoidable_hit =
          ( new raid_damage_t( "avoidable_hit", this, SCHOOL_FIRE, 1, damage_type_e::AVOIDABLE ) )->single_target();
      melee = ( new raid_damage_t( "melee", this, SCHOOL_PHYSICAL, 1, damage_type_e::TANK ) )->single_target();
      tank_buster =
          ( new raid_damage_t( "tank_buster", this, SCHOOL_PHYSICAL, 1, damage_type_e::TANK ) )->single_target();
      tank_buster_dot =
          ( new raid_damage_t( "tank_buster_dot", this, SCHOOL_NATURE, 1, damage_type_e::TANK ) )->single_target();
    }

    sim->encounter_dispellable = [ this ]( const player_t* p ) {
      for ( const auto& a : afflictions )
        if ( a.active && a.target == p )
          return as<double>( a.stacks );
      return 0.0;
    };
    sim->encounter_dispel = [ this ]( player_t* p ) {
      for ( size_t i = 0; i < afflictions.size(); ++i )
      {
        if ( afflictions[ i ].active && afflictions[ i ].target == p )
        {
          afflictions[ i ].active = false;
          return true;
        }
      }
      return false;
    };
    if ( !phases_str.empty() && phases.empty() )
      phases = parse_phases( phases_str );

    sim->encounter_timer = [ this ]( util::string_view name, bool ago ) {
      bool big_only = name == "big_burst";
      if ( !big_only && name != "burst" )
        return -1.0;

      if ( !phases.empty() )
      {
        double now = sim->current_time().total_seconds(), last = -1, next = sim->expected_iteration_time.total_seconds();
        for ( const auto& b : burst_schedule )
        {
          if ( big_only && !b.big )
            continue;
          if ( b.at > now )
          {
            next = b.at;
            break;
          }
          last = b.at;
        }
        if ( ago )
          return last < 0 ? 9999.0 : now - last;
        return std::max( 0.0, next - now );
      }

      timespan_t now  = sim->current_time();
      timespan_t next = sim->expected_iteration_time;
      timespan_t last = timespan_t::min();
      int i           = 0;
      for ( timespan_t at = burst_first; at < sim->expected_iteration_time; at += burst_interval, ++i )
      {
        if ( big_only && !is_big( i ) )
          continue;
        if ( at > now )
        {
          next = std::min( next, at );
          break;
        }
        last = at;
      }
      if ( ago )
        return last == timespan_t::min() ? 9999.0 : ( now - last ).total_seconds();
      return std::max( 0.0, ( next - now ).total_seconds() );
    };
  }

  void combat_begin() override
  {
    enemy::enemy_t::combat_begin();

    raid_size = sim->player_no_pet_list.size();
    tanks.clear();
    for ( auto p : sim->player_no_pet_list )
      if ( p->role == ROLE_TANK )
        tanks.push_back( p );
    active_tank = 0;

    every( steady_interval, steady_interval, [ this ] { cast_raid_pulse(); } );
    if ( phases.empty() )
      every( burst_first, burst_interval, [ this ] { cast_burst(); } );
    else
    {
      build_burst_schedule();
      for ( size_t i = 0; i < burst_schedule.size(); ++i )
        make_event( *sim, timespan_t::from_seconds( burst_schedule[ i ].at ), [ this, i ] { cast_scheduled_burst( i ); } );
    }
    every( spot_first, spot_interval, [ this ] { cast_spot_dots(); } );
    every( dispellable_first, dispellable_interval, [ this ] { apply_affliction(); } );
    every( avoidable_first, avoidable_interval, [ this ] { cast_avoidable(); } );
    every( melee_first, melee_interval, [ this ] { swing(); } );
    every( buster_first, buster_interval, [ this ] { cast_tank_buster(); } );
  }

  // Runs fn after first, then every interval until the fight ends.
  void every( timespan_t first, timespan_t interval, std::function<void()> fn )
  {
    make_event( *sim, first, [ this, interval, fn ] {
      fn();
      every( interval, interval, fn );
    } );
  }

  std::vector<player_t*> living_players() const
  {
    std::vector<player_t*> l;
    for ( auto p : sim->player_no_pet_list )
      if ( !p->is_sleeping() )
        l.push_back( p );
    return l;
  }

  std::vector<player_t*> random_players( size_t n )
  {
    auto l = living_players();
    rng().shuffle( l.begin(), l.end() );
    if ( l.size() > n )
      l.resize( n );
    return l;
  }

  // Hits a share of the players for the damage of the whole raid's share of rate x interval (in H).
  double per_target( double rate, timespan_t interval, size_t targets ) const
  {
    return rate * health() * interval.total_seconds() * as<double>( raid_size ) / as<double>( targets );
  }

  void cast_raid_pulse()
  {
    raid_pulse->set_damage( steady_rate * mult( &phase_t::steady ) * health() * steady_interval.total_seconds() );
    raid_pulse->execute();
  }

  // A burst of a phased fight: an instant burst like cast_burst, or a wave ticking on the raid once a second.
  void cast_scheduled_burst( size_t i )
  {
    const auto& b    = burst_schedule[ i ];
    raid_damage_t* a = b.big ? big_raid_burst : raid_burst;
    double total     = b.damage * health();
    sequence_add( a, a->target );
    if ( b.waves > 0 )
    {
      int ticks = std::max( 1, as<int>( std::lround( b.waves ) ) );
      for ( int tick = 0; tick < ticks; ++tick )
      {
        make_event( *sim, timespan_t::from_seconds( tick ), [ a, total, ticks ] {
          a->set_damage( total / ticks );
          a->execute();
        } );
      }
      return;
    }
    a->set_damage( total * burst_impact_share );
    a->execute();
    for ( int tick = 1; tick <= burst_ticks; ++tick )
    {
      make_event( *sim, timespan_t::from_seconds( tick ), [ a, total ] {
        a->set_damage( total * ( 1.0 - burst_impact_share ) / burst_ticks );
        a->execute();
      } );
    }
  }

  void cast_burst()
  {
    bool big          = is_big( bursts++ );
    raid_damage_t* a  = big ? big_raid_burst : raid_burst;
    double total      = burst_damage() * ( big ? big_burst_size : 1.0 );
    sequence_add( a, a->target );
    a->set_damage( total * burst_impact_share );
    a->execute();
    for ( int tick = 1; tick <= burst_ticks; ++tick )
    {
      make_event( *sim, timespan_t::from_seconds( tick ), [ a, total ] {
        a->set_damage( total * ( 1.0 - burst_impact_share ) / burst_ticks );
        a->execute();
      } );
    }
  }

  void cast_spot_dots()
  {
    auto victims = random_players( spot_targets );
    if ( victims.empty() )
      return;
    double tick_damage = per_target( spot_rate, spot_interval, spot_targets ) * mult( &phase_t::spot ) / spot_ticks;
    if ( tick_damage <= 0 )
      return;
    sequence_add( spot_dot, victims.front() );
    for ( int tick = 0; tick < spot_ticks; ++tick )
    {
      make_event( *sim, timespan_t::from_seconds( tick ), [ this, victims, tick_damage ] {
        spot_dot->set_damage( tick_damage );
        for ( auto v : victims )
          if ( !v->is_sleeping() )
            spot_dot->execute_on_target( v );
      } );
    }
  }

  void apply_affliction()
  {
    if ( mult( &phase_t::spot ) <= 0 )
      return;
    auto victims = random_players( 1 );
    if ( victims.empty() )
      return;
    sequence_add( dispellable_dot, victims.front() );
    size_t i = afflictions.size();
    afflictions.push_back( { victims.front(), 1, true } );
    schedule_affliction_tick( i );
    make_event( *sim, dispellable_duration, [ this, i ] { afflictions[ i ].active = false; } );
  }

  // Each tick hits for the current stacks, then adds one.
  void schedule_affliction_tick( size_t i )
  {
    make_event( *sim, dispellable_tick, [ this, i ] {
      auto& a = afflictions[ i ];
      if ( !a.active )
        return;
      if ( a.target->is_sleeping() )
      {
        a.active = false;
        return;
      }
      dispellable_dot->set_damage( dispellable_stack_damage * mult( &phase_t::spot ) * health() * a.stacks );
      dispellable_dot->execute_on_target( a.target );
      ++a.stacks;
      schedule_affliction_tick( i );
    } );
  }

  void cast_avoidable()
  {
    if ( mult( &phase_t::avoidable ) <= 0 )
      return;
    auto victims = random_players( 1 );
    if ( victims.empty() )
      return;
    avoidable_hit->set_damage( per_target( avoidable_rate, avoidable_interval, 1 ) * mult( &phase_t::avoidable ) );
    avoidable_hit->execute_on_target( victims.front() );
  }

  // The tank at active_tank if alive, else any living tank, else nullptr (no tanks: no tank damage).
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

  // Tank damage is the calibration's (2 tanks) whatever the number of tanks: one boss, one active tank.
  void swing()
  {
    if ( auto tank = current_tank() )
    {
      melee->set_damage( melee_rate * mult( &phase_t::tank ) * tank_health() * melee_interval.total_seconds() );
      melee->execute_on_target( tank );
    }
  }

  void cast_tank_buster()
  {
    auto tank = current_tank();
    if ( !tank )
      return;
    double interval = buster_interval.total_seconds();
    double m        = mult( &phase_t::tank );
    sequence_add( tank_buster, tank );
    tank_buster->set_damage( buster_rate * m * tank_health() * interval );
    tank_buster->execute_on_target( tank );
    double tick_damage = buster_dot_rate * m * tank_health() * interval / buster_dot_ticks;
    for ( int tick = 1; tick <= buster_dot_ticks; ++tick )
    {
      make_event( *sim, timespan_t::from_seconds( 2.0 * tick ), [ this, tank, tick_damage ] {
        if ( tank->is_sleeping() )
          return;
        tank_buster_dot->set_damage( tick_damage );
        tank_buster_dot->execute_on_target( tank );
      } );
    }
    // the other tank taunts
    active_tank = ( active_tank + 1 ) % tanks.size();
  }
};
}  // namespace

player_t* create_patchwerk( sim_t* sim )
{
  return new patchwerk_t( sim );
}

/* Coiled Altar (Mythic), simulated: Patchwerk with phases shaped like the user's Mythic kill (8:21, damage taken by
 * ability per phase from Warcraft Logs, 2026-11-08). The same calibration as Patchwerk (shares of the raid's health,
 * one healer's share = 100%); each phase's multipliers give its intensity (~0.94 / 0.93 / 1.08 / 1.08 of the fight's
 * average) and mix (shares of the raid's damage without the tanks):
 * - P1 (2:05): orb waves (Venom Rupture, Fangs) 53%, steady (Coalesced Venom) 43%, avoidable 3%: a wave every ~33 sec
 *   lasting ~17 sec, each +12%, the 3rd the biggest.
 * - P2 (2:40): steady (Dreadful Presence aura) 78%, slow waves (Retaliatory Malice ghosts) 13%: 2 ramps of ~50 sec;
 *   spot (Gloombomb, Dread Bolt, ghosts) 8%.
 * - Intermission (0:38): Spirit Erasure soaks, ~95% steady.
 * - P3 (3:00): steady 56%, waves 27% (Venom Rupture, Retaliatory Malice) every ~45 sec, spot 14%.
 * Tanks take ~11% of the damage (Patchwerk's calibration: 20%), so tank x0.55. Left out: the 122M Gravebound hit at
 * 8:05 (a kill-time event, not healing pressure). Fight length: the kill's, set in fights\coiled_altar.simc.
 */
constexpr util::string_view coiled_altar_phases =
    "125:steady=1.35,burst=1.07,spot=0.04,avoidable=1.16,tank=0.55,interval=33,first=18,waves=17,growth=0.12,big_every=3/"
    "160:steady=2.42,burst=0.25,spot=0.35,avoidable=0.4,tank=0.55,interval=80,first=25,waves=50/"
    "38:steady=3.43,burst=0,spot=0.25,avoidable=0,tank=0.5/"
    "180:steady=2.02,burst=0.63,spot=0.71,avoidable=0,tank=0.55,interval=45,first=20,waves=16,growth=0.1,big_every=3";

player_t* create_coiled_altar( sim_t* sim )
{
  return new patchwerk_t( sim, "Coiled Altar", coiled_altar_phases );
}
}  // namespace bosses
