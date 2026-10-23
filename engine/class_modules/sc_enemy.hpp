// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
#ifndef SC_ENEMY_HPP
#define SC_ENEMY_HPP

#include "simulationcraft.hpp"

namespace enemy
{
enum class tank_dummy_e
{
  NONE = 0,
  WEAK,
  DUNGEON,
  RAID,
  HEROIC,
  MYTHIC,
  MAX
};

struct enemy_t : public player_t
{
  size_t enemy_id;
  double fixed_health, initial_health;
  double fixed_health_percentage, initial_health_percentage;
  std::vector<std::pair<double, double>> custom_health_timeline;
  double health_recalculation_dampening_exponent;
  timespan_t waiting_time;

  int current_target;
  int apply_damage_taken_debuff;
  double custom_armor_coeff;

  std::vector<buff_t*> buffs_health_decades;

  enemy_t( sim_t* s, util::string_view n, race_e r = RACE_HUMANOID, player_e type = ENEMY );

  role_e primary_role() const override;
  resource_e primary_resource() const override;

  action_t* create_action( util::string_view name, util::string_view options_str ) override;
  void init_race() override;
  void init_base_stats() override;
  void init_defense() override;
  void create_buffs() override;
  void init_resources( bool force = false ) override;
  void init_target() override;
  virtual std::string generate_action_list();
  virtual void generate_heal_raid_event();
  void init_action_list() override;
  void init_actions() override;
  double resource_loss( resource_e, double, gain_t*, action_t* ) override;
  void create_options() override;
  pet_t* create_pet( util::string_view add_name, util::string_view pet_type = {} ) override;
  void create_pets() override;
  double health_percentage() const override;
  timespan_t time_to_percent( double ) const override;
  void combat_begin() override;
  void combat_end() override;
  virtual void recalculate_health();
  void demise() override;
  double armor_coefficient( int level, tank_dummy_e diff );
  std::unique_ptr<expr_t> create_expression( util::string_view expression_str ) override;
  bool has_absorb() const override;
  double current_absorb_amount() const override;
  timespan_t available() const override;
  void actor_changed() override;
  bool taunt( player_t* source ) override;
  void reset_auto_attacks( timespan_t delay, proc_t* proc = nullptr ) override;
  void delay_auto_attacks( timespan_t delay, proc_t* proc = nullptr ) override;

  bool validate_custom_timeline();
  std::string generate_tank_action_list( tank_dummy_e );
  void add_tank_heal_raid_event( tank_dummy_e );
};
}  // namespace enemy

#endif
