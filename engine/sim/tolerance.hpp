// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#pragma once

#include "config.hpp"

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
  int seed = 0;
  std::vector<std::string> options;  // a set's profile lines
  std::vector<std::pair<std::string, std::pair<std::string, unsigned>>> gear;  // slot -> (SimC item string, item level)
  std::vector<test_result_t> tests;
};

struct tolerance_t
{
  sim_t* sim;
  bool enabled = false;
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
  std::string healer_html;           // the healer report to write
  std::string healer_html_template;  // its template (an HTML file with a __SIMC_DATA__ placeholder)

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
  test_result_t search( const sim_control_t& base, const std::string& test, double held, double start, double step,
                        unsigned iters, double prec, int seed, setup_result_t* setup, bool start_passes = false );
  probe_t probe( const sim_control_t& base, const std::string& test, double pct, double held, unsigned iters,
                 int seed, setup_result_t* setup );
};
}  // namespace tolerance
