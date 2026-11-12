// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#pragma once

#include "config.hpp"
#include "sim/option.hpp"
#include "sim/tolerance_stats.hpp"

#include <map>
#include <set>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct sim_t;
struct sim_control_t;

// Healer damage tolerance (encounter sims): how much of the encounter's damage a healer survives. After the main sim,
// child sims raise the damage (the encounter_damage_* multipliers) to find the highest level where no player dies in
// more than tolerance_max_death_rate of fights. Profilesets become gear sets compared by the same search.
namespace tolerance
{
struct probe_t
{
  double pct  = 0;  // damage, as a fraction of the healer's share (1 = the share)
  double held = 0;  // the other damage types, for a single-type test
  bool pass   = false;
  std::vector<std::pair<std::string, double>> deaths;  // players with a death rate > 0, highest first
  int players_dying    = 0;   // above tolerance_max_death_rate
  double mana_end      = 1;   // healer's mana at the end, fraction of max
  double oom_at        = -1;  // seconds when the healer's average mana fell below tolerance_oom_threshold, -1 never
  double hps           = 0;
  double wipe_rate     = 0;
  double fight_length  = 0;
  bool has_stats       = false;  // a level run: the measurements below were collected
  fight_stats_t stats;
};

struct test_result_t
{
  std::string test;        // "combined" or a damage type
  double score      = -1;  // highest passing level, -1 none
  double first_fail = -1;  // lowest failing level, -1 none
  double held       = 0;
  std::vector<probe_t> probes;
};

struct setup_result_t
{
  std::string name;
  std::string role;  // "main" (the profile), "set" (a profileset), "noise" (the profile with another seed)
  std::string pattern;  // the damage pattern it was searched on (tolerance_t::patterns)
  bool combination = false;  // a set built from the pattern's upgrades worn together (not from the gear sets file)
  int seed = 0;
  std::vector<std::string> options;  // a set's profile lines
  std::vector<std::pair<std::string, std::pair<std::string, unsigned>>> gear;  // slot -> (SimC item string, item level)
  std::vector<test_result_t> tests;
  std::vector<probe_t> levels;  // fixed damage levels (tolerance_levels), with the measurements
  // The quick screen (tolerance_screen_iterations): raid health against current gear on the same few fights.
  unsigned screen_fights = 0;  // 0 = not screened
  double screen_diff     = 0;  // this set minus current gear, raid health (0-1)
  bool screened_out      = false;  // clearly behind: no full check, reported as a downgrade from the screen
};

struct tolerance_t
{
  sim_t* sim;
  bool enabled = false;
  // Search each setup's limit. Off (tolerance_search=0): the gear check only, every setup at the fixed levels.
  bool search_enabled = true;
  // Also search the gear sets' and noise runs' limits. Off (tolerance_search_sets=0): only current gear's limit, the
  // gear sets compared at the fixed levels.
  bool search_sets = true;
  // Current gear's limit with a cheap rough pass first (this many fights per step, 0 = off), then tests aimed with
  // the death rate.
  unsigned rough_iterations = 100;
  double share = 0.25;  // the healer's share of the raid's damage = 100%
  std::string share_source;
  std::string milestones_str;  // "Label:1.031/Label:1.122"
  std::string tests_str = "steady/burst/spot/tank/avoidable";
  unsigned iterations      = 1000;  // combined tests (the score, and what gear sets are compared by)
  unsigned type_iterations = 300;   // single-type tests
  double precision         = 0.0025;
  double type_precision    = 0.005;
  double max_death_rate    = 0.05;
  double oom_threshold     = 0.02;
  int noise_runs           = 2;  // with gear sets: the profile rerun with other seeds, to measure the noise
  // Every setup is also run at these fixed damage levels (1 = the share), collecting the measurements (stats) on
  // tolerance_level_iterations fights (0 = tolerance_iterations).
  std::string levels_str = "1/1.2";
  std::vector<double> levels;
  // tolerance_levels=limit: each pattern's level is current gear's limit there (levels holds the last one).
  bool levels_at_limit = false;
  unsigned level_iterations = 0;
  // Two-stage gear check: every gear set first plays tolerance_screen_iterations fights at the level (0 = off), current
  // gear too, on the same fights; a set more than tolerance_screen_margin raid health behind current gear stops there.
  unsigned screen_iterations = 0;
  double screen_margin       = 0.004;
  // This sim's measurements (tolerance_stats=1, set on the level runs): every sim and thread collects its own fights.
  fight_stats_t stats;
  // Limit searches remembered between runs (tolerance_cache=<file>, empty = off): each fast search is filed under a
  // fingerprint of everything that decides it (the run's options, the setup's own lines, and a short sample run of that
  // exact setup, which changes with any engine, profile or fight change), so a stale result is never reused.
  std::string cache_file;
  std::map<std::string, test_result_t> cache;
  bool cache_changed = false;
  // Results kept from an earlier run (tolerance_keep=<that run's JSON report>): its sets that this run doesn't play are
  // carried into this report unchanged, so a short run (e.g. only rotation choices) keeps the last full run's talent
  // values. Read at initialize(), because the report may be written to that same file.
  std::string keep_file;
  std::string keep_data;
  std::string keep_except;  // tolerance_keep_except=<prefix>: kept sets named so are dropped unless reused (rerun whole)
  // tolerance_reuse=1 (with tolerance_keep): a set the earlier report already has (same name, lines and damage pattern)
  // isn't played again when the profile's own result on that pattern is unchanged; the report carries its old result.
  bool reuse = false;
  std::set<std::string> keep_sets;              // name \x1f pattern \x1f lines of the earlier report's sets
  std::map<std::string, double> keep_main;      // pattern -> the profile's raid health in the earlier report
  // tolerance_upgrade_limits=0: no limit search for each set with more points (quick runs; finalists get one later).
  bool upgrade_limits = true;
  std::set<std::string> reused;  // "name, unit separator, pattern" of the sets this run reused (the report keeps them)
  std::string ran;  // when this run started (local time), stamped on its setups
  std::string healer_html;           // the healer report to write
  std::string healer_html_template;  // its template (an HTML file with a __SIMC_DATA__ placeholder)

  // Damage patterns: the whole search (current gear, gear sets, noise) runs once per pattern, so gear can be compared
  // on differently shaped fights. The first is the fight as set up (named by tolerance_pattern_name); each
  // tolerance_pattern."Name"+=<boss option> line adds one, its options going into the boss's section.
  std::string pattern_name;
  opts::map_list_t pattern_map;
  std::vector<std::pair<std::string, std::vector<std::string>>> patterns;

  std::vector<std::pair<std::string, std::vector<std::string>>> sets;  // taken over from the profilesets
  std::vector<setup_result_t> results;

  tolerance_t( sim_t* s );
  // Takes the profilesets over as gear sets, so the normal profileset run doesn't happen.
  void initialize();
  void analyze();
  // The healer report: healer_html_template with the JSON report inserted, written to healer_html.
  void print_healer_html( const std::string& json ) const;

private:
  void create_options();
  // Current gear's limit (all damage): a rough pass of rough_iterations fights per step brackets it, then full tests
  // aimed where the worst player's death rate crosses max_death_rate narrow it to the precision.
  test_result_t fast_search( const sim_control_t& base, setup_result_t* setup );
  // fast_search through the cache (tolerance_cache): set_lines = the setup's own profile lines (empty for current gear)
  test_result_t cached_fast_search( const sim_control_t& base, const std::vector<std::string>& set_lines,
                                    setup_result_t* setup );
  void load_cache();
  void save_cache() const;
  // One pattern's gear sets with more raid health than current gear beyond the noise (indices into results); first =
  // that pattern's current gear.
  std::vector<size_t> point_upgrades( size_t first ) const;
  // One pattern's upgrades worn together (one per slot, limit not dropped), if at least two: name and profile lines.
  std::optional<std::pair<std::string, std::vector<std::string>>> combination( size_t first, double score ) const;
  test_result_t search( const sim_control_t& base, const std::string& test, double held, double start, double step,
                        unsigned iters, double prec, int seed, setup_result_t* setup, bool start_passes = false );
  probe_t probe( const sim_control_t& base, const std::string& test, double pct, double held, unsigned iters,
                 int seed, setup_result_t* setup, bool with_stats = false );
  void run_levels( const sim_control_t& base, int seed, setup_result_t& setup );
};
}  // namespace tolerance
