// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "tolerance.hpp"

#include "item/item.hpp"
#include "player/player.hpp"
#include "sim/sim.hpp"
#include "sim/sim_control.hpp"
#include "util/io.hpp"
#include "util/util.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace tolerance
{
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
  sim->add_option( opt_string( "healer_html", healer_html ) );
  sim->add_option( opt_string( "healer_html_template", healer_html_template ) );
}

void tolerance_t::initialize()
{
  if ( !enabled || sim->parent || sim->thread_index > 0 || sim->profileset_enabled )
    return;
  if ( sim->encounter_str.empty() )
    throw sc_invalid_sim_argument( "tolerance=1 needs an encounter (encounter=<name>)." );

  // Profilesets become gear sets: sorted by name so runs are reproducible.
  for ( auto& [ name, lines ] : sim->profileset_map )
    sets.emplace_back( name, lines );
  range::sort( sets, []( const auto& a, const auto& b ) { return a.first < b.first; } );
  sim->profileset_map.clear();
}

probe_t tolerance_t::probe( const sim_control_t& base, const std::string& test, double pct, double held,
                            unsigned iters, int seed, setup_result_t* setup )
{
  auto control = std::make_unique<sim_control_t>( base );
  std::vector<std::string> args = { fmt::format( "iterations={}", iters ), "deterministic=1", "target_error=0",
                                    "tolerance=0", "encounter_damage_all=1" };
  for ( const char* type : { "steady", "burst", "spot", "tank", "avoidable" } )
  {
    double level = ( test == "combined" || test == type ) ? pct : held;
    args.push_back( fmt::format( "encounter_damage_{}={}", type, level * share ) );
  }
  control->options.parse_args( args );

  auto child = std::make_unique<sim_t>( sim, 0, control.get() );
  child->seed            = seed ? seed : default_seed;
  child->report_progress = false;
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

  if ( setup && setup->gear.empty() )
  {
    for ( const auto& item : healer->items )
    {
      if ( item.active() && item.parsed.data.id )
        setup->gear.emplace_back( util::slot_type_string( item.slot ),
                                  std::make_pair( item.encoded_item(), item.item_level() ) );
    }
  }

  fmt::print( "  {:<24} {:<10} {:6.1f}%  {}  worst death rate {:.0f}%\n", setup ? setup->name : "", test_label( test ),
              pct * 100, p.pass ? "pass" : "FAIL", p.deaths.empty() ? 0.0 : p.deaths.front().second * 100 );
  std::fflush( stdout );
  return p;
}

// Steps up (or down) by the factor step until the breaking point is bracketed, then halves the bracket until it is
// narrower than prec.
test_result_t tolerance_t::search( const sim_control_t& base, const std::string& test, double held, double start,
                                   double step, unsigned iters, double prec, int seed, setup_result_t* setup )
{
  test_result_t r;
  r.test = test;
  r.held = held;
  double lo = -1, hi = -1;

  auto run = [ & ]( double pct ) {
    r.probes.push_back( probe( base, test, pct, held, iters, seed, setup ) );
    ( r.probes.back().pass ? lo : hi ) = pct;
  };

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

void tolerance_t::analyze()
{
  if ( !enabled || sim->parent || sim->is_canceled() )
    return;

  const sim_control_t& base = *sim->control;
  fmt::print( "\nDamage tolerance ({} fights per step{})...\n", iterations,
              sets.empty() ? "" : fmt::format( ", {} gear sets", sets.size() ) );

  // The profile: all damage raised together (the score), then each damage type with the others held at that score.
  setup_result_t main;
  main.name = "Current gear";
  main.role = "main";
  main.tests.push_back( search( base, "combined", 0, 1.0, 1.25, iterations, precision, 0, &main ) );
  double score = main.tests.front().score;
  if ( score > 0 )
  {
    for ( auto&& type : util::string_split<util::string_view>( tests_str, "/:," ) )
    {
      if ( type == "none" || sim->is_canceled() )
        continue;
      main.tests.push_back( search( base, std::string( type ), score, score, 1.1, type_iterations, type_precision, 0,
                                    &main ) );
    }
  }
  results.push_back( std::move( main ) );
  if ( score <= 0 || sets.empty() )
    return;

  // Gear sets: the same search on the same random numbers, starting at the profile's score with small steps.
  for ( const auto& [ name, lines ] : sets )
  {
    if ( sim->is_canceled() )
      return;
    setup_result_t s;
    s.name    = name;
    s.role    = "set";
    s.options = lines;
    sim_control_t control( base );
    apply_set( control, lines, sim->profileset_main_actor_index );
    s.tests.push_back( search( control, "combined", 0, score, 1.02, iterations, precision, 0, &s ) );
    results.push_back( std::move( s ) );
  }

  // The noise: the profile with other random numbers. The report counts differences smaller than this as equal.
  for ( int n = 1; n <= noise_runs && !sim->is_canceled(); n++ )
  {
    setup_result_t s;
    s.name = "Current gear";
    s.role = "noise";
    s.seed = default_seed + 1000 * n;
    s.tests.push_back( search( base, "combined", 0, score, 1.02, iterations, precision, s.seed, nullptr ) );
    results.push_back( std::move( s ) );
  }
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
