// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "tolerance.hpp"

#include "action/action.hpp"
#include "item/item.hpp"
#include "player/player.hpp"
#include "sim/sim.hpp"
#include "sim/sim_control.hpp"
#include "util/io.hpp"
#include "util/util.hpp"

#include "rapidjson/document.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

namespace tolerance
{
double raid_health_of( const setup_result_t& s );  // below: raid health kept at the highest fixed level
namespace
{
constexpr int default_seed = 31459;  // SimC's deterministic seed

// Options that start a new actor's section (as in profileset.cpp), and options that make another actor the active
// one: a set's lines must go into the healer's section, before any of these.
bool ends_actor_section( const option_tuple_t& opt )
{
  static constexpr std::array<util::string_view, 27> names{ {
    "deathknight", "demonhunter", "druid", "evoker", "hunter", "mage", "monk", "paladin", "priest", "rogue", "shaman",
    "warlock", "warrior", "player_simplified", "enemy", "tank_dummy", "pet", "guardian", "copy", "armory", "local_json",
    "guild", "encounter", "raid_tanks", "raid_healers", "raid_ranged", "raid_melee" } };
  return range::any_of( names, [ &opt ]( util::string_view n ) { return util::str_compare_ci( opt.name, n ); } );
}

bool starts_actor( const option_tuple_t& opt )
{
  return ends_actor_section( opt ) && !util::str_compare_ci( opt.name, "encounter" ) &&
         !util::str_prefix_ci( opt.name, "raid_" );
}

// A set's lines replace the healer's options of the same name (the last one in its section), the rest are added at
// the end of the section.
void apply_set( sim_control_t& c, const std::vector<std::string>& lines, unsigned main_actor )
{
  option_db_t set_opts;
  set_opts.parse_args( lines );

  auto& opts   = c.options;
  size_t start = opts.size();
  unsigned seen = 0;
  for ( size_t i = 0; i < opts.size(); i++ )
  {
    if ( starts_actor( opts[ i ] ) && seen++ == main_actor )
    {
      start = i;
      break;
    }
  }
  if ( start == opts.size() )
    throw std::invalid_argument( fmt::format( "tolerance: no actor #{} for the gear sets.", main_actor ) );

  for ( const auto& t : set_opts )
  {
    size_t end = start + 1;
    while ( end < opts.size() && !ends_actor_section( opts[ end ] ) )
      end++;

    bool replaced = false;
    for ( size_t i = end; i-- > start + 1; )
    {
      if ( opts[ i ].name == t.name )
      {
        opts[ i ].value = t.value;
        replaced = true;
        break;
      }
    }
    if ( !replaced )
      opts.insert( opts.begin() + end, t );
  }
}

// A damage pattern's lines are boss options. The boss (encounter=) is the last actor, so they go at the end of its
// section, after the fight file's own boss options (a pattern's phases=... must win over the fight's).
void apply_pattern( sim_control_t& c, const std::vector<std::string>& lines )
{
  if ( lines.empty() )
    return;
  option_db_t pattern_opts;
  pattern_opts.parse_args( lines );

  auto& opts = c.options;
  auto it    = std::find_if( opts.rbegin(), opts.rend(),
                             []( const option_tuple_t& o ) { return util::str_compare_ci( o.name, "encounter" ); } );
  if ( it == opts.rend() )
    throw std::invalid_argument( "tolerance_pattern needs encounter=<name> in the profile." );
  opts.insert( opts.end(), pattern_opts.begin(), pattern_opts.end() );
}

const char* test_label( const std::string& test )
{
  return test == "combined" ? "all damage" : test.c_str();
}
}  // namespace

tolerance_t::tolerance_t( sim_t* s ) : sim( s )
{
  create_options();
}

void tolerance_t::create_options()
{
  sim->add_option( opt_bool( "tolerance", enabled ) );
  sim->add_option( opt_bool( "tolerance_search", search_enabled ) );
  sim->add_option( opt_bool( "tolerance_search_sets", search_sets ) );
  sim->add_option( opt_uint( "tolerance_rough_iterations", rough_iterations ) );
  sim->add_option( opt_float( "tolerance_share", share, 0.001, 10 ) );
  sim->add_option( opt_string( "tolerance_share_source", share_source ) );
  sim->add_option( opt_string( "tolerance_milestones", milestones_str ) );
  sim->add_option( opt_string( "tolerance_tests", tests_str ) );
  sim->add_option( opt_uint( "tolerance_iterations", iterations ) );
  sim->add_option( opt_uint( "tolerance_type_iterations", type_iterations ) );
  sim->add_option( opt_float( "tolerance_precision", precision, 0.0001, 1 ) );
  sim->add_option( opt_float( "tolerance_type_precision", type_precision, 0.0001, 1 ) );
  sim->add_option( opt_float( "tolerance_max_death_rate", max_death_rate, 0, 1 ) );
  sim->add_option( opt_float( "tolerance_oom_threshold", oom_threshold, 0, 1 ) );
  sim->add_option( opt_int( "tolerance_noise_runs", noise_runs, 0, 10 ) );
  sim->add_option( opt_string( "tolerance_pattern_name", pattern_name ) );
  sim->add_option( opt_map_list( "tolerance_pattern.", pattern_map ) );
  sim->add_option( opt_string( "tolerance_levels", levels_str ) );
  sim->add_option( opt_uint( "tolerance_level_iterations", level_iterations ) );
  sim->add_option( opt_uint( "tolerance_screen_iterations", screen_iterations ) );
  sim->add_option( opt_float( "tolerance_screen_margin", screen_margin, 0, 1 ) );
  sim->add_option( opt_string( "tolerance_cache", cache_file ) );
  sim->add_option( opt_string( "tolerance_keep", keep_file ) );
  sim->add_option( opt_string( "tolerance_keep_except", keep_except ) );
  sim->add_option( opt_bool( "tolerance_reuse", reuse ) );
  sim->add_option( opt_bool( "tolerance_upgrade_limits", upgrade_limits ) );
  sim->add_option( opt_bool( "tolerance_stats", stats.enabled ) );
  sim->add_option( opt_float( "tolerance_recovery_margin", stats.recovery_margin, 0, 1 ) );
  sim->add_option( opt_float( "tolerance_burst_window", stats.burst_window, 1, 120 ) );
  sim->add_option( opt_string( "healer_html", healer_html ) );
  sim->add_option( opt_string( "healer_html_template", healer_html_template ) );
}

void tolerance_t::initialize()
{
  if ( !enabled || sim->parent || sim->thread_index > 0 || sim->profileset_enabled )
    return;
  if ( sim->encounter_str.empty() )
    throw sc_invalid_sim_argument( "tolerance=1 needs an encounter (encounter=<name>)." );

  for ( auto&& l : util::string_split<util::string_view>( levels_str, "/:," ) )
  {
    if ( l == "none" )
      continue;
    // "limit": every setup is checked at current gear's limit, searched first on each damage pattern.
    if ( l == "limit" )
    {
      levels_at_limit = true;
      continue;
    }
    double v = util::to_double( l );
    if ( v <= 0 )
      throw sc_invalid_sim_argument( fmt::format( "tolerance_levels: invalid level '{}'.", l ) );
    levels.push_back( v );
  }

  // When this run started, stamped on its setups (the report says how old kept results are).
  {
    std::time_t now = std::time( nullptr );
    char buf[ 32 ];
    if ( std::strftime( buf, sizeof buf, "%Y-%m-%d %H:%M", std::localtime( &now ) ) )
      ran = buf;
  }
  // tolerance_keep: read the earlier report now, before this run's report can overwrite it. A missing file is fine
  // (the first run).
  if ( !keep_file.empty() )
  {
    std::ifstream in( keep_file, std::ios::binary );
    if ( in )
    {
      std::stringstream buf;
      buf << in.rdbuf();
      keep_data = buf.str();
    }
  }
  if ( reuse && !keep_data.empty() )
  {
    rapidjson::Document old;
    old.Parse( keep_data.c_str() );
    if ( !old.HasParseError() && old.IsObject() && old.HasMember( "sim" ) && old[ "sim" ].HasMember( "tolerance" ) &&
         old[ "sim" ][ "tolerance" ].HasMember( "setups" ) && old[ "sim" ][ "tolerance" ][ "setups" ].IsArray() )
    {
      for ( const auto& o : old[ "sim" ][ "tolerance" ][ "setups" ].GetArray() )
      {
        if ( !o.HasMember( "role" ) || !o.HasMember( "pattern" ) || !o.HasMember( "levels" ) || !o[ "levels" ].IsArray() ||
             o[ "levels" ].Empty() )
          continue;
        const auto& last = o[ "levels" ][ o[ "levels" ].Size() - 1 ];
        if ( !last.HasMember( "stats" ) || !last[ "stats" ].HasMember( "raid_health" ) )
          continue;
        std::string role = o[ "role" ].GetString(), pattern = o[ "pattern" ].GetString();
        if ( role == "main" )
          keep_main[ pattern ] = last[ "stats" ][ "raid_health" ].GetDouble();
        else if ( role == "set" && o.HasMember( "name" ) && o.HasMember( "options" ) && !o.HasMember( "stale" ) )
        {
          std::string key = std::string( o[ "name" ].GetString() ) + "\x1f" + pattern;
          for ( const auto& l : o[ "options" ].GetArray() )
            key += std::string( "\x1f" ) + l.GetString();
          keep_sets.insert( key );
        }
      }
    }
  }

  // Profilesets become gear sets: sorted by name so runs are reproducible.
  for ( auto& [ name, lines ] : sim->profileset_map )
    sets.emplace_back( name, lines );
  range::sort( sets, []( const auto& a, const auto& b ) { return a.first < b.first; } );
  sim->profileset_map.clear();

  // The fight as set up first, then the extra patterns by name.
  patterns.emplace_back( pattern_name, std::vector<std::string>() );
  size_t extra = patterns.size();
  for ( const auto& [ name, lines ] : pattern_map )
    patterns.emplace_back( name, lines );
  std::sort( patterns.begin() + extra, patterns.end(),
             []( const auto& a, const auto& b ) { return a.first < b.first; } );
}

probe_t tolerance_t::probe( const sim_control_t& base, const std::string& test, double pct, double held,
                            unsigned iters, int seed, setup_result_t* setup, bool with_stats )
{
  auto control = std::make_unique<sim_control_t>( base );
  std::vector<std::string> args = { fmt::format( "iterations={}", iters ), "deterministic=1", "target_error=0",
                                    "tolerance=0", "encounter_damage_all=1",
                                    fmt::format( "tolerance_stats={}", with_stats ? 1 : 0 ) };
  for ( const char* type : { "steady", "burst", "spot", "tank", "avoidable" } )
  {
    double level = ( test == "combined" || test == type ) ? pct : held;
    args.push_back( fmt::format( "encounter_damage_{}={}", type, level * share ) );
  }
  control->options.parse_args( args );

  auto child = std::make_unique<sim_t>( sim, 0, control.get() );
  child->seed            = seed ? seed : default_seed;
  child->report_progress = false;
  // free the worker threads' sims after the run (as profilesets do): without it every child sim kept one copy of the
  // whole raid per thread, and a long run (the talent values: 150+ child sims) ran out of memory
  child->cleanup_threads = true;
  if ( !child->execute() || child->is_canceled() )
    throw std::runtime_error( fmt::format( "tolerance: simulation failed at {} {:.1f}%", test, pct * 100 ) );

  probe_t p;
  p.pct  = pct;
  p.held = held;
  double fights = std::max( 1.0, static_cast<double>( child->simulation_length.count() ) );

  std::vector<std::pair<std::string, double>> deaths;
  for ( const player_t* pl : child->player_no_pet_list )
  {
    // SimC counts deaths in its per-thread warm-up fights too, so a rate can slightly exceed 1.
    double rate = std::min( 1.0, pl->collected_data.deaths.count() / fights );
    if ( rate > 0 )
      deaths.emplace_back( pl->name_str, rate );
  }
  range::sort( deaths, []( const auto& a, const auto& b ) { return a.second > b.second; } );
  p.players_dying = as<int>( range::count_if( deaths, [ this ]( const auto& d ) { return d.second > max_death_rate; } ) );
  p.pass          = p.players_dying == 0;
  if ( deaths.size() > 8 )
    deaths.resize( 8 );
  p.deaths = std::move( deaths );

  const player_t* healer = child->player_no_pet_list[ sim->profileset_report_player_index ];
  double max_mana        = healer->resources.max[ RESOURCE_MANA ];
  if ( max_mana > 0 )
  {
    p.mana_end = healer->collected_data.combat_end_resource[ RESOURCE_MANA ].mean() / max_mana;
    for ( const auto& rt : healer->collected_data.resource_timelines )
    {
      if ( rt.type != RESOURCE_MANA )
        continue;
      const auto& data = rt.timeline.data();
      for ( size_t i = 5; i < data.size(); i++ )
      {
        if ( data[ i ] < oom_threshold * max_mana )
        {
          p.oom_at = as<double>( i );
          break;
        }
      }
    }
  }
  p.hps          = healer->collected_data.hps.mean();
  p.wipe_rate    = child->encounter_wipes / fights;
  p.fight_length = child->simulation_length.mean();
  if ( with_stats )
  {
    p.has_stats = true;
    p.stats     = child->tolerance->stats;
  }

  // Current gear's first level run (the limit): keep one of its fights for the report's decision timeline.
  if ( with_stats && setup && setup->role == "main" && setup->sample_casts.empty() )
  {
    setup->sample_level = pct;
    for ( const auto& e : healer->collected_data.action_sequence )
    {
      if ( !e.action )
        continue;
      sample_cast_t c;
      c.time          = e.time.total_seconds();
      c.id            = e.action->id;
      c.name          = e.action->name();
      c.spell         = e.action->data_reporting().name_cstr();
      c.apl           = e.action->signature_str;
      c.target_health = e.target_health;
      c.raid_health   = e.raid_health;
      if ( e.target && !e.target->is_enemy() )
        c.ally = e.target->name();
      if ( e.resource_max_snapshot[ RESOURCE_MANA ] > 0 )
        c.mana = e.resource_snapshot[ RESOURCE_MANA ] / e.resource_max_snapshot[ RESOURCE_MANA ];
      setup->sample_casts.push_back( std::move( c ) );
    }
    for ( const auto* t : child->target_list )
    {
      for ( const auto& e : t->collected_data.action_sequence )
      {
        if ( !e.action )
          continue;
        setup->sample_events.push_back( { e.time.total_seconds(), e.action->name(),
                                          e.target && !e.target->is_enemy() ? e.target->name() : std::string() } );
      }
    }
  }

  if ( setup && setup->gear.empty() )
  {
    for ( const auto& item : healer->items )
    {
      if ( item.active() && item.parsed.data.id )
        setup->gear.emplace_back( util::slot_type_string( item.slot ),
                                  std::make_pair( item.encoded_item(), item.item_level() ) );
    }
  }

  fmt::print( "  {:<24} {:<10} {:6.1f}%  {}  worst death rate {:.0f}%{}\n", setup ? setup->name : "",
              with_stats ? "level" : test_label( test ), pct * 100, p.pass ? "pass" : "FAIL",
              p.deaths.empty() ? 0.0 : p.deaths.front().second * 100,
              with_stats && p.stats.fights > 0
                  ? fmt::format( "  raid health {:.2f}%  deaths per 100 fights {:.1f}",
                                 p.stats.health_seconds > 0 ? 100 * p.stats.health_time / p.stats.health_seconds : 0.0,
                                 100 * p.stats.deaths / p.stats.fights )
                  : "" );
  std::fflush( stdout );
  return p;
}

test_result_t tolerance_t::fast_search( const sim_control_t& base, setup_result_t* setup )
{
  test_result_t r;
  r.test = "combined";
  // A bracket end: the level, the worst player's death rate there, and whether a full test (iterations fights)
  // measured it; pct < 0 = none yet.
  struct end_t
  {
    double pct = -1, rate = 0;
    bool full  = false;
  } lo, hi;
  auto worst = []( const probe_t& p ) { return p.deaths.empty() ? 0.0 : p.deaths.front().second; };

  // 1. The rough pass: steps of x1.25 from 100% with few fights, until a pass and a fail bracket the limit.
  if ( rough_iterations > 0 && rough_iterations < iterations )
  {
    double x = 1.0;
    for ( int n = 0; n < 40 && !sim->is_canceled(); n++ )
    {
      probe_t p = probe( base, "combined", x, 0, rough_iterations, 0, setup );
      end_t e;
      e.pct  = x;
      e.rate = worst( p );
      ( p.pass ? lo : hi ) = e;
      if ( lo.pct >= 0 && hi.pct >= 0 )
        break;
      if ( lo.pct < 0 && x < 0.02 )
        break;
      if ( hi.pct < 0 && x > 16 )
        break;
      x = p.pass ? x * 1.25 : x / 1.25;
    }
  }

  // 2. Full tests. Inside a bracket, aim where the death rate crosses the fail line: the rate grows roughly
  // exponentially with damage, so interpolate its log (kept off the ends). An aimed test landing close to the line is
  // followed by one the precision's width beyond it, which closes the bracket if the aim was good. Two aimed tests on
  // the same side in a row, or 4 full tests in, and it halves instead (aiming can stall on a curved rate). Without an
  // end yet, step by 5%. Every end the rough pass set is checked with a full test before it counts.
  double next = -1;
  int side = 0, streak = 0, full_tests = 0;
  for ( int n = 0; n < 40 && !sim->is_canceled(); n++ )
  {
    double x;
    bool aimed = false;
    if ( lo.pct < 0 && hi.pct < 0 )
      x = 1.0;
    else if ( lo.pct < 0 )
      x = hi.pct / 1.05;
    else if ( hi.pct < 0 )
      x = lo.pct * 1.05;
    else if ( hi.pct - lo.pct <= precision )
    {
      if ( !lo.full )
        x = lo.pct;
      else if ( !hi.full )
        x = hi.pct;
      else
        break;
    }
    else if ( next > lo.pct && next < hi.pct )
      x = next;
    else
    {
      double f = 0.5;
      if ( streak >= 2 || full_tests >= 4 )
        streak = 0;
      else
      {
        double a = std::log( std::max( lo.rate, 1e-3 ) ), b = std::log( std::max( hi.rate, 1e-3 ) );
        double t = std::log( std::max( max_death_rate, 1e-3 ) );
        if ( b > a )
          f = std::clamp( ( t - a ) / ( b - a ), 0.1, 0.9 );
      }
      x     = lo.pct + f * ( hi.pct - lo.pct );
      aimed = true;
    }
    next = -1;
    if ( x < 0.02 || x > 16 )
      break;

    r.probes.push_back( probe( base, "combined", x, 0, iterations, 0, setup ) );
    full_tests++;
    const probe_t& p = r.probes.back();
    double rate      = worst( p );
    if ( p.pass )
    {
      if ( hi.pct >= 0 && x >= hi.pct )
        hi = end_t();
      lo.pct = x, lo.rate = rate, lo.full = true;
    }
    else
    {
      if ( lo.pct >= 0 && x <= lo.pct )
        lo = end_t();
      hi.pct = x, hi.rate = rate, hi.full = true;
    }
    if ( aimed )
    {
      int s  = p.pass ? 1 : -1;
      streak = s == side ? streak + 1 : 1;
      side   = s;
      if ( std::abs( rate - max_death_rate ) < 0.012 && full_tests < 4 )
        next = x + s * 0.9 * precision;
    }
  }

  range::sort( r.probes, []( const probe_t& a, const probe_t& b ) { return a.pct < b.pct; } );
  r.score      = lo.full ? lo.pct : -1;
  r.first_fail = hi.full ? hi.pct : -1;
  return r;
}

// FNV-1a, 64 bit: a stable name for a cache entry (std::hash isn't guaranteed stable between builds).
static std::string fingerprint_hash( const std::string& s )
{
  uint64_t h = 1469598103934665603ULL;
  for ( unsigned char c : s )
  {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return fmt::format( "{:016x}", h );
}

// Cache file: one entry per limit search.
//   E <hash> <score> <first_fail> <probes>
//   P <pct> <pass> <players_dying> <mana_end> <oom_at> <hps> <wipe_rate> <fight_length> <deaths> [<name> <rate>]...
void tolerance_t::load_cache()
{
  static bool loaded = false;
  if ( loaded )
    return;
  loaded = true;

  std::ifstream in( cache_file );
  std::string line, key;
  test_result_t* cur = nullptr;
  while ( std::getline( in, line ) )
  {
    std::istringstream ls( line );
    std::string tag;
    ls >> tag;
    if ( tag == "E" )
    {
      size_t n;
      test_result_t r;
      r.test = "combined";
      ls >> key >> r.score >> r.first_fail >> n;
      cur = ls ? &( cache[ key ] = r ) : nullptr;
    }
    else if ( tag == "P" && cur )
    {
      probe_t p;
      size_t nd;
      ls >> p.pct >> p.pass >> p.players_dying >> p.mana_end >> p.oom_at >> p.hps >> p.wipe_rate >> p.fight_length >> nd;
      for ( size_t i = 0; i < nd && ls; i++ )
      {
        std::string name;
        double rate;
        ls >> name >> rate;
        p.deaths.emplace_back( name, rate );
      }
      if ( ls )
        cur->probes.push_back( p );
    }
  }
}

void tolerance_t::save_cache() const
{
  std::ofstream out( cache_file );
  for ( const auto& [ key, r ] : cache )
  {
    out << fmt::format( "E {} {:.17g} {:.17g} {}\n", key, r.score, r.first_fail, r.probes.size() );
    for ( const auto& p : r.probes )
    {
      out << fmt::format( "P {:.17g} {} {} {:.17g} {:.17g} {:.17g} {:.17g} {:.17g} {}", p.pct, p.pass ? 1 : 0, p.players_dying,
                          p.mana_end, p.oom_at, p.hps, p.wipe_rate, p.fight_length, p.deaths.size() );
      for ( const auto& [ name, rate ] : p.deaths )
        out << fmt::format( " {} {:.17g}", name, rate );
      out << "\n";
    }
  }
}

test_result_t tolerance_t::cached_fast_search( const sim_control_t& base, const std::vector<std::string>& set_lines,
                                               setup_result_t* setup )
{
  if ( cache_file.empty() )
    return fast_search( base, setup );
  load_cache();

  // The fingerprint: every option of this run (profile, fight, pattern, the set's lines), the search settings, and a
  // short sample of this exact setup: any change to the engine, the character or the fight changes the sample.
  probe_t sample = probe( base, "combined", 1.0, 0, 100, 0, nullptr );
  std::string key;
  static const std::vector<std::string> outputs_only = { "json",        "json2",          "output",      "html",
                                                         "healer_html", "healer_html_template", "tolerance_cache",
                                                         "tolerance_keep", "tolerance_keep_except", "tolerance_reuse",
                                                         "tolerance_share_source", "xml", "log", "report_details" };
  for ( const auto& o : base.options )
    if ( range::find( outputs_only, o.name ) == outputs_only.end() )
      key += o.scope + "\x1f" + o.name + "=" + o.value + "\n";
  for ( const auto& l : set_lines )
    key += "set:" + l + "\n";
  key += fmt::format( "{}|{}|{}|{}|{}|{}|{:.6f}|{:.6f}|{:.6f}|{}", share, iterations, rough_iterations, precision,
                      max_death_rate, sim->threads, sample.hps, sample.fight_length, sample.mana_end,
                      sample.deaths.empty() ? 0.0 : sample.deaths.front().second );
  std::string hash = fingerprint_hash( key );

  if ( auto it = cache.find( hash ); it != cache.end() )
  {
    fmt::print( "  {:<24} limit {:.1f}% (remembered: nothing changed since it was searched)\n",
                setup ? setup->name : "", it->second.score * 100 );
    return it->second;
  }

  auto r        = fast_search( base, setup );
  cache[ hash ] = r;
  save_cache();
  return r;
}

// The setup at each fixed damage level, all damage together, with the measurements.
void tolerance_t::run_levels( const sim_control_t& base, int seed, setup_result_t& setup )
{
  unsigned iters = level_iterations ? level_iterations : iterations;
  for ( double level : levels )
  {
    if ( sim->is_canceled() )
      return;
    setup.levels.push_back( probe( base, "combined", level, 0, iters, seed, &setup, true ) );
  }
}

// Steps up (or down) by the factor step until the breaking point is bracketed, then halves the bracket until it is
// narrower than prec.
test_result_t tolerance_t::search( const sim_control_t& base, const std::string& test, double held, double start,
                                   double step, unsigned iters, double prec, int seed, setup_result_t* setup,
                                   bool start_passes )
{
  test_result_t r;
  r.test = test;
  r.held = held;
  double lo = -1, hi = -1;

  auto run = [ & ]( double pct ) {
    r.probes.push_back( probe( base, test, pct, held, iters, seed, setup ) );
    ( r.probes.back().pass ? lo : hi ) = pct;
  };

  // A single-type search starts at the all-damage limit, which is known to pass (on more fights): searching from a
  // re-probe there could fail by chance and then walk that one type down while the others stay at the limit.
  if ( start_passes )
    lo = start;
  else
    run( start );
  while ( hi < 0 && lo < 16 && !sim->is_canceled() )
    run( lo * step );
  while ( lo < 0 && hi > 0.02 && !sim->is_canceled() )
    run( hi / step );
  while ( lo >= 0 && hi >= 0 && hi - lo > prec && !sim->is_canceled() )
    run( ( lo + hi ) / 2 );

  range::sort( r.probes, []( const probe_t& a, const probe_t& b ) { return a.pct < b.pct; } );
  r.score      = lo;
  r.first_fail = hi;
  return r;
}

// Raid health kept on a level run, -1 without measurements.
static double probe_health( const probe_t& p )
{
  if ( !p.has_stats || p.stats.health_seconds <= 0 )
    return -1;
  return p.stats.health_time / p.stats.health_seconds;
}

void tolerance_t::analyze()
{
  if ( !enabled || sim->parent || sim->is_canceled() )
    return;

  if ( search_enabled && search_sets )
    fmt::print( "\nDamage tolerance ({} fights per step{}{})...\n", iterations,
                sets.empty() ? "" : fmt::format( ", {} gear sets", sets.size() ),
                patterns.size() > 1 ? fmt::format( ", {} damage patterns", patterns.size() ) : "" );
  else
    fmt::print( "\n{}Gear check ({} gear sets{})...\n", search_enabled ? "Your limit and the " : "", sets.size(),
                patterns.size() > 1 ? fmt::format( ", {} damage patterns", patterns.size() ) : "" );

  for ( size_t i = 0; i < patterns.size() && !sim->is_canceled(); i++ )
  {
    const auto& [ pattern, lines ] = patterns[ i ];
    if ( patterns.size() > 1 )
      fmt::print( "Damage pattern: {}\n", pattern.empty() ? "as set up" : pattern );
    sim_control_t base( *sim->control );
    apply_pattern( base, lines );

    // The profile: all damage raised together (the score), then (on the first pattern) each damage type with the
    // others at one healer's share. Without the search (the gear check) only the fixed levels.
    setup_result_t main;
    main.name    = "Current gear";
    main.role    = "main";
    main.pattern = pattern;
    double score = 0;
    if ( search_enabled )
    {
      main.tests.push_back( search_sets ? search( base, "combined", 0, 1.0, 1.25, iterations, precision, 0, &main )
                                        : cached_fast_search( base, {}, &main ) );
      score = main.tests.front().score;
      if ( score > 0 && i == 0 )
      {
        for ( auto&& type : util::string_split<util::string_view>( tests_str, "/:," ) )
        {
          if ( type == "none" || sim->is_canceled() )
            continue;
          // The others are held at one healer's share (100%), clearly survivable: held at the limit itself, any
          // extra damage tips the edge over by chance and every type reads "no room". Starts at the limit (it passes
          // with everything there, so with the rest lower too).
          main.tests.push_back( search( base, std::string( type ), 1.0, score, 1.1, type_iterations, type_precision,
                                        0, &main, true ) );
        }
      }
    }
    // Gear checked at the limit: the level is this pattern's limit of current gear (the edge, where healing matters
    // most), so every healer and pattern is checked under the same pressure relative to what they can heal.
    if ( levels_at_limit )
    {
      if ( !search_enabled )
        throw sc_invalid_sim_argument( "tolerance_levels=limit needs the limit search (tolerance_search=1)." );
      levels = { score > 0 ? score : 1.0 };
    }
    run_levels( base, 0, main );
    size_t first = results.size();  // this pattern's setups start here: current gear, the sets, the noise runs
    results.push_back( std::move( main ) );
    if ( sets.empty() || ( search_enabled && search_sets && score <= 0 ) )
      continue;

    // The quick screen (gear check only): current gear on a few fights at the level, the sets on the same fights next.
    double screen_base = -1;
    bool screening     = screen_iterations > 0 && !( search_enabled && search_sets ) && !levels.empty();
    if ( screening )
    {
      fmt::print( "Quick screen: {} fights per gear set\n", screen_iterations );
      screen_base = probe_health( probe( base, "combined", levels.back(), 0, screen_iterations, 0, nullptr, true ) );
    }

    // tolerance_reuse: the profile's result on this pattern equals the earlier report's, so its sets would replay the
    // same fights: those already there are not played again.
    bool reuse_ok = false;
    if ( reuse && keep_main.count( pattern ) )
    {
      double h = raid_health_of( results[ first ] );
      reuse_ok = h >= 0 && std::abs( h - keep_main[ pattern ] ) < 1e-12;
      if ( reuse_ok )
        fmt::print( "Same result as the earlier report: sets already simulated are reused\n" );
    }

    // Gear sets: the same search on the same random numbers, starting at the profile's score with small steps.
    for ( const auto& [ name, set_lines ] : sets )
    {
      if ( sim->is_canceled() )
        return;
      if ( reuse_ok )
      {
        std::string key = name + "\x1f" + pattern;
        for ( const auto& l : set_lines )
          key += "\x1f" + l;
        if ( keep_sets.count( key ) )
        {
          reused.insert( name + "\x1f" + pattern );
          continue;  // the report carries its earlier result
        }
      }
      setup_result_t s;
      s.name    = name;
      s.role    = "set";
      s.pattern = pattern;
      s.options = set_lines;
      sim_control_t control( base );
      apply_set( control, set_lines, sim->profileset_main_actor_index );
      if ( screen_base >= 0 )
      {
        // the same fights as current gear's screen: clearly behind it means no full check
        auto sp = probe( control, "combined", levels.back(), 0, screen_iterations, 0, &s, true );
        double h = probe_health( sp );
        if ( h >= 0 )
        {
          s.screen_fights = screen_iterations;
          s.screen_diff   = h - screen_base;
          s.screened_out  = s.screen_diff < -screen_margin;
        }
        if ( s.screened_out )
        {
          fmt::print( "  {}: {:+.2f} points on the quick screen, no full check\n", s.name, 100 * s.screen_diff );
          results.push_back( std::move( s ) );
          continue;
        }
      }
      if ( search_enabled && search_sets )
        s.tests.push_back( search( control, "combined", 0, score, 1.02, iterations, precision, 0, &s ) );
      run_levels( control, 0, s );
      results.push_back( std::move( s ) );
    }

    // The noise: the profile with other random numbers. The report counts differences smaller than this as equal.
    for ( int n = 1; n <= noise_runs && !sim->is_canceled(); n++ )
    {
      setup_result_t s;
      s.name    = "Current gear";
      s.role    = "noise";
      s.pattern = pattern;
      s.seed    = default_seed + 1000 * n;
      if ( search_enabled && search_sets )
        s.tests.push_back( search( base, "combined", 0, score, 1.02, iterations, precision, s.seed, nullptr ) );
      run_levels( base, s.seed, s );
      results.push_back( std::move( s ) );
    }

    // Only current gear's limit was searched: also search the limit of every gear set with more points, the same way.
    // More raid health on average doesn't always mean a higher limit (an item can heal better most of the time and
    // worse in the worst moments, e.g. by costing mana), so the report calls a set an upgrade only when its limit
    // doesn't drop.
    if ( search_enabled && !search_sets && score > 0 )
    {
      for ( size_t i : upgrade_limits ? point_upgrades( first ) : std::vector<size_t>() )
      {
        if ( sim->is_canceled() )
          return;
        fmt::print( "More points, its limit: {}\n", results[ i ].name );
        sim_control_t control( base );
        apply_set( control, results[ i ].options, sim->profileset_main_actor_index );
        auto t = cached_fast_search( control, results[ i ].options, &results[ i ] );
        results[ i ].tests.push_back( std::move( t ) );
      }

      // The upgrades together: items that each beat current gear may or may not add up (both can lean on the same
      // mana, or cover the same moments), so the combination is simmed like any other set, limit included.
      if ( auto combo = combination( first, score ); combo && !sim->is_canceled() )
      {
        setup_result_t s;
        s.name        = combo->first;
        s.role        = "set";
        s.pattern     = pattern;
        s.options     = combo->second;
        s.combination = true;
        fmt::print( "Upgrades together: {}\n", s.name );
        sim_control_t control( base );
        apply_set( control, s.options, sim->profileset_main_actor_index );
        run_levels( control, 0, s );
        s.tests.push_back( cached_fast_search( control, s.options, &s ) );
        results.push_back( std::move( s ) );
      }
    }
  }
}

// Raid health kept (the gear check's score) at the highest fixed level, -1 without one.
double raid_health_of( const setup_result_t& s )
{
  if ( s.levels.empty() || !s.levels.back().has_stats || s.levels.back().stats.health_seconds <= 0 )
    return -1;
  return s.levels.back().stats.health_time / s.levels.back().stats.health_seconds;
}

// The gear sets among one pattern's setups (from results[first]: current gear, the sets, the noise runs) that beat
// current gear's raid health by more than the noise runs' spread (at least 0.1 points), the report's rule.
std::vector<size_t> tolerance_t::point_upgrades( size_t first ) const
{
  std::vector<size_t> out;
  double base = raid_health_of( results[ first ] );
  if ( base < 0 )
    return out;
  double noise = 0.001;
  for ( size_t i = first + 1; i < results.size(); i++ )
    if ( results[ i ].role == "noise" && raid_health_of( results[ i ] ) >= 0 )
      noise = std::max( noise, std::abs( raid_health_of( results[ i ] ) - base ) );
  for ( size_t i = first + 1; i < results.size(); i++ )
    if ( results[ i ].role == "set" && raid_health_of( results[ i ] ) >= 0 && raid_health_of( results[ i ] ) - base > noise )
      out.push_back( i );
  return out;
}

// The upgrades of one pattern worn together, when at least two can be: single-item gear sets with more points whose
// limit didn't drop (by more than half a point, the report's rule), the best one per slot, never the same item twice.
// Returns the combination's name and its profile lines.
std::optional<std::pair<std::string, std::vector<std::string>>> tolerance_t::combination( size_t first,
                                                                                         double score ) const
{
  constexpr double limit_noise = 0.005;
  double base = raid_health_of( results[ first ] );
  struct pick_t
  {
    size_t index;
    double gain;
    std::string item;
  };
  std::map<std::string, pick_t> by_slot;
  for ( size_t i : point_upgrades( first ) )
  {
    const auto& s = results[ i ];
    if ( s.combination || s.options.size() != 1 || s.tests.empty() || s.tests.front().score < 0 ||
         s.tests.front().score < score - limit_noise )
      continue;
    const std::string& line = s.options.front();
    auto eq                 = line.find( '=' );
    // items only (a ",id=" line): talent switches (the talent values run) are never combined
    if ( eq == std::string::npos || line.find( ",id=" ) == std::string::npos )
      continue;
    std::string slot = line.substr( 0, eq );
    auto id          = line.find( ",id=" );
    std::string item = id == std::string::npos ? line : line.substr( id + 4, line.find_first_not_of( "0123456789", id + 4 ) - id - 4 );
    double gain      = raid_health_of( s ) - base;
    auto it          = by_slot.find( slot );
    if ( it == by_slot.end() || gain > it->second.gain )
      by_slot[ slot ] = { i, gain, item };
  }

  // The same item in two slots (a ring for either finger): keep the better one.
  std::vector<pick_t> picks;
  for ( const auto& [ slot, p ] : by_slot )
  {
    auto same = range::find_if( picks, [ &p ]( const pick_t& q ) { return q.item == p.item; } );
    if ( same == picks.end() )
      picks.push_back( p );
    else if ( p.gain > same->gain )
      *same = p;
  }
  if ( picks.size() < 2 )
    return std::nullopt;

  range::sort( picks, []( const pick_t& a, const pick_t& b ) { return a.gain > b.gain; } );
  std::string name;
  std::vector<std::string> lines;
  for ( const auto& p : picks )
  {
    name += ( name.empty() ? "" : " + " ) + results[ p.index ].name;
    lines.push_back( results[ p.index ].options.front() );
  }
  return std::make_pair( "Together: " + name, lines );
}

void tolerance_t::print_healer_html( const std::string& json ) const
{
  if ( healer_html.empty() )
    return;
  if ( healer_html_template.empty() )
    throw std::invalid_argument( "healer_html needs healer_html_template=<template file>." );

  std::ifstream in( healer_html_template, std::ios::binary );
  if ( !in )
    throw std::invalid_argument( fmt::format( "Cannot read healer_html_template '{}'.", healer_html_template ) );
  std::stringstream buf;
  buf << in.rdbuf();
  std::string html = buf.str();

  // Keep "</script>" in the data from closing the data tag early.
  std::string data;
  data.reserve( json.size() );
  for ( size_t i = 0; i < json.size(); i++ )
  {
    if ( json[ i ] == '<' && i + 1 < json.size() && json[ i + 1 ] == '/' )
      data += "<\\";
    else
      data += json[ i ];
  }

  auto at = html.find( "__SIMC_DATA__" );
  if ( at == std::string::npos )
    throw std::invalid_argument( "healer_html_template has no __SIMC_DATA__ placeholder." );
  html.replace( at, std::string( "__SIMC_DATA__" ).size(), data );

  io::ofstream out;
  out.open( healer_html, std::ios::out | std::ios::binary );
  if ( !out.is_open() )
    throw std::invalid_argument( fmt::format( "Cannot write healer_html '{}'.", healer_html ) );
  out << html;
  fmt::print( "Healer report written to {}\n", healer_html );
}
}  // namespace tolerance
